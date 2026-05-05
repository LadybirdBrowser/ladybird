/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Types.h>
#include <LibCore/ElapsedTimer.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Resource/BitmapResource.h>
#include <LibGfx/Resource/FontResource.h>
#include <LibPaintServer/Debug.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/DisplayListSubmitter.h>
#include <LibWeb/Painting/DocumentFramePainter.h>
#include <LibWeb/Painting/NavigableContainerViewportPaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

using ScrollStateSnapshotByDisplayList = HashMap<NonnullRefPtr<DisplayList>, ScrollStateSnapshot>;

static void track_timing(StringView timing_name, Optional<Core::ElapsedTimer> const& timer)
{
    if (!timer.has_value())
        return;
    PaintServer::dbgtrack(timing_name, static_cast<f32>(timer->elapsed_time().to_microseconds()) / 1000.f, 5000);
}

static ScrollStateSnapshotByDisplayList collect_scroll_state_snapshots_by_display_list(DOM::Document& document, DisplayList& display_list, bool refresh_root_scroll_state)
{
    VERIFY(document.paintable());

    auto& document_paintable = *document.paintable();
    if (refresh_root_scroll_state)
        document_paintable.refresh_scroll_state();

    ScrollStateSnapshotByDisplayList scroll_state_snapshot_by_display_list;
    scroll_state_snapshot_by_display_list.set(display_list, document_paintable.scroll_state_snapshot());
    document_paintable.for_each_in_inclusive_subtree_of_type<NavigableContainerViewportPaintable>([&scroll_state_snapshot_by_display_list](auto& navigable_container_paintable) {
        auto const* hosted_document = navigable_container_paintable.navigable_container().content_document_without_origin_check();
        if (!hosted_document || !hosted_document->paintable())
            return TraversalDecision::Continue;
        auto retained_display_list = hosted_document->retained_display_list();
        auto* retained_display_list_ptr = retained_display_list.ptr();
        if (!retained_display_list_ptr)
            return TraversalDecision::Continue;
        const_cast<DOM::Document&>(*hosted_document).paintable()->refresh_scroll_state();
        scroll_state_snapshot_by_display_list.set(*retained_display_list_ptr, hosted_document->paintable()->scroll_state_snapshot());
        return TraversalDecision::Continue;
    });

    return scroll_state_snapshot_by_display_list;
}

template<typename Callback>
static TraversalDecision for_each_document_in_paint_tree(DOM::Document& document, Callback callback)
{
    if (callback(document) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (!document.paintable())
        return TraversalDecision::Continue;

    auto& document_paintable = *document.paintable();
    return document_paintable.for_each_in_inclusive_subtree_of_type<NavigableContainerViewportPaintable>([&](auto& navigable_container_paintable) {
        auto const* hosted_document = navigable_container_paintable.navigable_container().content_document_without_origin_check();
        if (!hosted_document || !hosted_document->layout_is_up_to_date())
            return TraversalDecision::Continue;
        return callback(const_cast<DOM::Document&>(*hosted_document));
    });
}

template<typename Callback>
static TraversalDecision for_each_document_in_display_list_tree(DOM::Document& document, Callback callback)
{
    return for_each_document_in_paint_tree(document, [&](DOM::Document& current_document) {
        if (!current_document.retained_display_list())
            return TraversalDecision::Continue;
        return callback(current_document);
    });
}

bool has_pending_scroll_state_update(DOM::Document& document)
{
    bool has_pending_update = false;
    (void)for_each_document_in_display_list_tree(document, [&](DOM::Document& current_document) {
        if (current_document.paintable() && current_document.paintable()->has_pending_scroll_state_update()) {
            has_pending_update = true;
            return TraversalDecision::Break;
        }
        return TraversalDecision::Continue;
    });
    return has_pending_update;
}

void clear_pending_scroll_state_update(DOM::Document& document)
{
    (void)for_each_document_in_display_list_tree(document, [&](DOM::Document& current_document) {
        if (current_document.paintable())
            current_document.paintable()->clear_pending_scroll_state_update();
        return TraversalDecision::Continue;
    });
}

static void clear_pending_scroll_state_update_in_paint_tree(DOM::Document& document)
{
    (void)for_each_document_in_paint_tree(document, [&](DOM::Document& current_document) {
        if (current_document.paintable())
            current_document.paintable()->clear_pending_scroll_state_update();
        return TraversalDecision::Continue;
    });
}

static Gfx::IntSize frame_size_for_document(DevicePixelRect viewport_rect, HTML::PaintConfig const& config)
{
    if (config.canvas_fill_rect.has_value())
        return config.canvas_fill_rect->size();
    return viewport_rect.size().to_type<int>();
}

struct LocalPaintEncodingState {
    Gfx::BitmapRegistry bitmap_registry;
    HashMap<u64, PaintServer::ResourceID> bitmap_uid_to_resource_id;
    u32 next_bitmap_resource_id_seed { 1 };
    Gfx::FontResourceRegistry font_registry;
};

static PaintCommandEncodingContext make_local_encoding_context(LocalPaintEncodingState& state, double device_pixels_per_css_pixel)
{
    return PaintCommandEncodingContext {
        .ensure_bitmap_resource = [&state](Gfx::DecodedImageFrame const& frame, Optional<u64> stable_bitmap_id) -> Optional<PaintServer::ResourceID> {
            u64 const bitmap_uid = stable_bitmap_id.value_or(static_cast<u64>(reinterpret_cast<FlatPtr>(frame.bitmap_ref().ptr())));
            auto it = state.bitmap_uid_to_resource_id.find(bitmap_uid);
            PaintServer::ResourceID stable_resource_id { 0 };
            if (it == state.bitmap_uid_to_resource_id.end()) {
                stable_resource_id = Gfx::ResourceID::make(Gfx::WireResourceType::Bitmap, state.next_bitmap_resource_id_seed++);
                state.bitmap_uid_to_resource_id.set(bitmap_uid, stable_resource_id);
            } else {
                stable_resource_id = it->value;
            }

            return state.bitmap_registry.register_bitmap(stable_resource_id, frame);
        },
        .ensure_font_resource = [&state](Gfx::Font const& font) -> PaintServer::ResourceID {
            return state.font_registry.ensure_font_resource(font);
        },
        .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
    };
}

static PaintCommandEncodingContext make_page_encoding_context(u64 page_id, double device_pixels_per_css_pixel)
{
    return PaintCommandEncodingContext {
        .ensure_bitmap_resource = [page_id](Gfx::DecodedImageFrame const& frame, Optional<u64> stable_bitmap_id) -> Optional<PaintServer::ResourceID> {
            return DisplayListSubmitter::register_bitmap_resource(page_id, frame, stable_bitmap_id);
        },
        .ensure_font_resource = [page_id](Gfx::Font const& font) -> PaintServer::ResourceID {
            auto resource_id = DisplayListSubmitter::register_font_resource(page_id, font);
            VERIFY(resource_id.has_value());
            return resource_id.release_value();
        },
        .device_pixels_per_css_pixel = device_pixels_per_css_pixel,
    };
}

static void paint_document_frame(DOM::Document& document, HTML::PaintConfig const& config, DisplayListRecorder& display_list_recorder, DevicePixelRect viewport_rect, Gfx::IntRect bitmap_rect)
{
    // https://drafts.csswg.org/css-color-adjust-1/#color-scheme-effect
    // On the root element, the used color scheme additionally must affect the surface color of the canvas, and the viewport’s scrollbars.
    auto color_scheme = CSS::PreferredColorScheme::Light;
    if (auto* html_element = document.html_element(); html_element && html_element->layout_node()) {
        if (html_element->layout_node()->computed_values().color_scheme() == CSS::PreferredColorScheme::Dark)
            color_scheme = CSS::PreferredColorScheme::Dark;
    }

    // .. in the case of embedded documents typically rendered over a transparent canvas
    // (such as provided via an HTML iframe element), if the used color scheme of the element
    // and the used color scheme of the embedded document's root element do not match,
    // then the UA must use an opaque canvas of the Canvas color appropriate to the
    // embedded document's used color scheme instead of a transparent canvas.
    bool opaque_canvas = false;
    if (auto navigable = document.navigable(); navigable) {
        if (auto container_element = navigable->container(); container_element && container_element->layout_node()) {
            auto container_scheme = container_element->layout_node()->computed_values().color_scheme();
            if (container_scheme == CSS::PreferredColorScheme::Auto)
                container_scheme = CSS::PreferredColorScheme::Light;

            opaque_canvas = container_scheme != color_scheme;
        }
    }

    if (config.canvas_fill_rect.has_value())
        display_list_recorder.fill_rect(config.canvas_fill_rect.value(), CSS::SystemColor::canvas(color_scheme));

    if (opaque_canvas)
        display_list_recorder.fill_rect(bitmap_rect, CSS::SystemColor::canvas(color_scheme));

    display_list_recorder.fill_rect(bitmap_rect, document.background_color());
    Web::DisplayListRecordingContext context(display_list_recorder, document.page().palette(), document.page().client().device_pixels_per_css_pixel(), document.page().chrome_metrics());
    context.set_device_viewport_rect(viewport_rect);
    context.set_should_show_line_box_borders(config.should_show_line_box_borders);
    context.set_should_paint_overlay(config.paint_overlay);

    auto& viewport_paintable = *document.paintable();
    viewport_paintable.paint_all_phases(context);
    if (document.highlighted_node() && document.highlighted_node()->paintable())
        document.highlighted_node()->paintable()->paint_inspector_overlay(context);
}

Optional<DeferredPaintCommandSubmission> prepare_paint_commands_for_document_frame(DOM::Document& document, HTML::PaintConfig config, u64 painting_id, PaintServer::FrameOutputType output_type, PaintServer::OffscreenRenderTarget offscreen_target)
{
    auto viewport_rect = document.page().css_to_device_rect(document.viewport_rect());
    Gfx::IntRect bitmap_rect { {}, viewport_rect.size().to_type<int>() };

    document.update_paint_and_hit_testing_properties_if_needed();

    if (!document.paintable())
        VERIFY_NOT_REACHED();

    document.paintable()->refresh_scroll_state();

    HashMap<FlatPtr, ScrollStateSnapshot> scroll_state_snapshots_by_source_context_namespace_id;
    scroll_state_snapshots_by_source_context_namespace_id.set(reinterpret_cast<FlatPtr>(&document), document.paintable()->scroll_state_snapshot());

    auto const& visual_context_tree = document.paintable()->visual_context_tree();
    auto display_list = DisplayList::create(visual_context_tree);
    display_list->set_source_context_namespace_id(reinterpret_cast<FlatPtr>(&document));
    display_list->set_viewport_size(bitmap_rect.size());

    auto submitter = make<DisplayListSubmitter>(
        painting_id,
        document.page().client().device_pixels_per_css_pixel(),
        frame_size_for_document(viewport_rect, config),
        move(scroll_state_snapshots_by_source_context_namespace_id),
        output_type,
        offscreen_target);

    LocalPaintEncodingState local_encoding_state;
    PaintCommandEncodingContext local_encoding_context = make_local_encoding_context(local_encoding_state, document.page().client().device_pixels_per_css_pixel());
    PaintCommandEncodingContext page_encoding_context = make_page_encoding_context(painting_id, document.page().client().device_pixels_per_css_pixel());
    PaintCommandEncodingContext& encoding_context = submitter->can_stream() ? page_encoding_context : local_encoding_context;
    DisplayListRecorder display_list_recorder(*display_list, encoding_context);

    Optional<Core::ElapsedTimer> recording_timer;
    if (PaintServer::is_logging_enabled(PaintServer::LOG_TIMING))
        recording_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    paint_document_frame(document, config, display_list_recorder, viewport_rect, bitmap_rect);
    track_timing("display list recording"sv, recording_timer);

    HashMap<FlatPtr, ScrollStateSnapshot> recorded_scroll_state_snapshots_by_source_context_namespace_id;
    for (auto const& it : collect_scroll_state_snapshots_by_display_list(document, *display_list, false)) {
        auto source_context_namespace_id = it.key->source_context_namespace_id();
        if (!source_context_namespace_id.has_value())
            continue;
        recorded_scroll_state_snapshots_by_source_context_namespace_id.set(*source_context_namespace_id, it.value);
    }
    display_list->set_scroll_state_snapshots_by_source_context_namespace_id(move(recorded_scroll_state_snapshots_by_source_context_namespace_id));
    auto external_content_sources = display_list->external_content_sources();

    clear_pending_scroll_state_update_in_paint_tree(document);
    return DeferredPaintCommandSubmission {
        .submit = [submitter = move(submitter), display_list = move(display_list)]() mutable -> Optional<PaintServer::ReleaseToken> {
            return submitter->finalize(*display_list);
        },
        .external_content_sources = move(external_content_sources),
    };
}

Optional<PaintServer::ReleaseToken> submit_paint_commands_for_document_frame(DOM::Document& document, HTML::PaintConfig config, u64 painting_id, PaintServer::FrameOutputType output_type, PaintServer::OffscreenRenderTarget offscreen_target)
{
    Optional<Core::ElapsedTimer> finalize_timer;
    if (PaintServer::is_logging_enabled(PaintServer::LOG_TIMING))
        finalize_timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto prepared_submission = prepare_paint_commands_for_document_frame(document, config, painting_id, output_type, offscreen_target);
    if (!prepared_submission.has_value())
        return {};

    auto submit = move(prepared_submission->submit);
    auto release_token = submit();
    track_timing("frame finalize"sv, finalize_timer);
    return release_token;
}

RefPtr<DisplayList> update_display_list_for_document_frame(DOM::Document& document, HTML::PaintConfig config)
{
    Optional<Core::ElapsedTimer> timer;
    if (PaintServer::is_logging_enabled(PaintServer::LOG_TIMING))
        timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);

    auto viewport_rect = document.page().css_to_device_rect(document.viewport_rect());
    Gfx::IntRect bitmap_rect { {}, viewport_rect.size().to_type<int>() };

    document.update_paint_and_hit_testing_properties_if_needed();

    if (!document.paintable())
        VERIFY_NOT_REACHED();

    auto const& visual_context_tree = document.paintable()->visual_context_tree();
    auto display_list = DisplayList::create(visual_context_tree);
    display_list->set_source_context_namespace_id(reinterpret_cast<FlatPtr>(&document));
    display_list->set_viewport_size(bitmap_rect.size());

    LocalPaintEncodingState local_encoding_state;
    PaintCommandEncodingContext local_encoding_context = make_local_encoding_context(local_encoding_state, document.page().client().device_pixels_per_css_pixel());
    DisplayListRecorder display_list_recorder(*display_list, local_encoding_context);

    paint_document_frame(document, config, display_list_recorder, viewport_rect, bitmap_rect);
    HashMap<FlatPtr, ScrollStateSnapshot> scroll_state_snapshots_by_source_context_namespace_id;
    for (auto const& it : collect_scroll_state_snapshots_by_display_list(document, *display_list, true)) {
        auto source_context_namespace_id = it.key->source_context_namespace_id();
        if (!source_context_namespace_id.has_value())
            continue;
        scroll_state_snapshots_by_source_context_namespace_id.set(*source_context_namespace_id, it.value);
    }
    display_list->set_scroll_state_snapshots_by_source_context_namespace_id(move(scroll_state_snapshots_by_source_context_namespace_id));
    track_timing("display list"sv, timer);

    clear_pending_scroll_state_update(document);

    return display_list;
}

Optional<DocumentFramePainterResult> update_display_list_and_scroll_state_for_document_frame(DOM::Document& document, HTML::PaintConfig paint_config)
{
    auto display_list = document.update_display_list(paint_config);
    if (!display_list)
        return {};
    auto source_context_namespace_id = display_list->source_context_namespace_id();
    VERIFY(source_context_namespace_id.has_value());
    auto root_scroll_state_snapshot = display_list->scroll_state_snapshots_by_source_context_namespace_id().get(*source_context_namespace_id);
    VERIFY(root_scroll_state_snapshot.has_value());

    return DocumentFramePainterResult {
        .display_list = move(display_list),
        .scroll_state_snapshot = root_scroll_state_snapshot.release_value(),
    };
}

}
