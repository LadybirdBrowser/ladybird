/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibWeb/Compositor/NavigablePresenter.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Compositor {

void NavigablePresenter::did_hand_display_list_to_compositor(NonnullRefPtr<Compositing::DisplayList> display_list, HTML::PaintConfig paint_config, Compositing::DisplayListResourceSet command_resources, Compositing::DisplayListResourceSet resources)
{
    m_compositor_display_list_visual_context_tree_structural_epoch = display_list->compatible_visual_context_tree_structural_epoch();
    m_resource_storage.retain_only(resources);
    m_compositor_display_list = move(display_list);
    m_compositor_display_list_command_resources = move(command_resources);
    m_compositor_display_list_resources = move(resources);
    m_compositor_display_list_paint_config = paint_config;
}

void NavigablePresenter::did_hand_visual_context_tree_to_compositor(Compositing::DisplayListResourceSet resources)
{
    m_resource_storage.retain_only(resources);
    m_compositor_display_list_resources = move(resources);
}

void NavigablePresenter::forget_compositor_display_list()
{
    m_compositor_display_list_paint_config.clear();
    m_compositor_display_list = nullptr;
    m_compositor_display_list_resources = {};
    m_compositor_display_list_command_resources = {};
}

CompositorFrame NavigablePresenter::build_frame(SealedPresentation const& sealed, Optional<PublishedDisplayList> published)
{
    // A recording downgraded to cache-read-only leaves the retained source and the cached ranges into it live, so the
    // resources they reference must survive the pruning that follows. A recording that replaced the source references
    // everything the new source does.
    auto resources_with = [&](Compositing::DisplayListResourceSet resources, Compositing::AccumulatedVisualContextTree const& visual_context_tree) {
        if (!published.has_value() || !published->replaces_paint_command_cache_source)
            resources.include(sealed.paint_command_cache_source_resources);
        resources.include(m_resource_storage.collect_referenced_resources(visual_context_tree));
        return resources;
    };

    // Keyboard eligibility belongs to this publication, not to the cached paint commands. Refresh it even if recording
    // was skipped or returned the same display list, and send it with the corresponding scroll state.
    auto& display_list = published.has_value() ? *published->display_list : *m_compositor_display_list;
    Compositing::KeyboardScrollState keyboard_scroll_state;
    if (sealed.keyboard_scroll_state.has_value()) {
        keyboard_scroll_state = *sealed.keyboard_scroll_state;
        m_last_keyboard_scroll_state_generation = keyboard_scroll_state.generation;
        keyboard_scroll_state.visual_context_tree_structural_epoch = display_list.compatible_visual_context_tree_structural_epoch();
    }
    auto async_scrolling_metadata = display_list.async_scrolling_metadata().value_or({});
    async_scrolling_metadata.keyboard_scroll_state = keyboard_scroll_state;
    display_list.set_async_scrolling_metadata(move(async_scrolling_metadata));

    m_last_frame_presented_by = PresentedBy::Main;
    CompositorFrame frame;
    bool const display_list_is_unchanged = published.has_value() && m_compositor_display_list == published->display_list;
    if (published.has_value() && !display_list_is_unchanged) {
        auto command_resources = published->display_list == sealed.paint_command_cache_source
            ? sealed.paint_command_cache_source_resources
            : m_resource_storage.collect_referenced_resources(*published->display_list);
        auto const& visual_context_tree = sealed.visual_context_tree.value();
        auto resources = resources_with(command_resources, visual_context_tree);
        frame.display_list_update = CompositorFrame::DisplayListUpdate {
            .display_list = published->display_list,
            .visual_context_tree = visual_context_tree,
            .resource_transaction = m_resource_storage.create_transaction(m_compositor_display_list_resources, resources),
            .scroll_state_snapshot = sealed.scroll_state_snapshot,
        };
        did_hand_display_list_to_compositor(published->display_list, sealed.paint_config, move(command_resources), move(resources));
        return frame;
    }

    if (display_list_is_unchanged) {
        m_compositor_display_list_paint_config = sealed.paint_config;
        if (m_resource_storage.has_resources_added_since_last_retain())
            m_resource_storage.retain_only(m_compositor_display_list_resources);
    }
    if (sealed.sends_visual_context_tree) {
        auto const& visual_context_tree = sealed.visual_context_tree.value();
        VERIFY(visual_context_tree.structural_epoch() == m_compositor_display_list_visual_context_tree_structural_epoch);
        auto resources = resources_with(m_compositor_display_list_command_resources, visual_context_tree);
        frame.visual_context_tree_update = CompositorFrame::VisualContextTreeUpdate {
            .visual_context_tree = visual_context_tree,
            .resource_transaction = m_resource_storage.create_transaction(m_compositor_display_list_resources, resources),
        };
        did_hand_visual_context_tree_to_compositor(move(resources));
    }
    frame.scroll_state_update = CompositorFrame::ScrollStateUpdate {
        .scroll_state_snapshot = sealed.scroll_state_snapshot,
        .keyboard_scroll_state = move(keyboard_scroll_state),
    };
    return frame;
}

void NavigablePresenter::present_beside_event_loop(SealedPresentation& sealed, NonnullRefPtr<Compositing::DisplayList> display_list)
{
    bool const replaces_paint_command_cache_source = sealed.recording->cache_mode == Painting::PaintCommandCacheMode::ReadWrite
        && display_list != sealed.paint_command_cache_source;
    PublishedDisplayList published { move(display_list), replaces_paint_command_cache_source };
    auto frame = build_frame(sealed, published);
    frame.context_id = sealed.context_id;
    frame.present_viewport_rect = sealed.present_viewport_rect;
    sealed.sink->submit(move(frame));
    sealed.published = move(published);
    m_last_frame_presented_by = PresentedBy::Flight;
}

}

extern "C" WEB_API void web_navigable_presenter_destroy(void* presenter)
{
    delete static_cast<Web::Compositor::NavigablePresenter*>(presenter);
}

extern "C" WEB_API void web_sealed_presentation_destroy(void* sealed)
{
    delete static_cast<Web::Compositor::SealedPresentation*>(sealed);
}

extern "C" WEB_API void web_navigable_presenter_add_font(void* presenter, void const* font)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().add_font(*static_cast<Gfx::Font const*>(font));
}

extern "C" WEB_API void web_navigable_presenter_add_image_frame(void* presenter, void const* frame)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().add_image_frame(*static_cast<Gfx::DecodedImageFrame const*>(frame));
}

extern "C" WEB_API void web_navigable_presenter_add_video_sink(void* presenter, u64 resource_id, u64 sink_handle)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().add_video_sink(Compositing::VideoSinkResourceId { resource_id }, Media::VideoSinkHandle { sink_handle });
}

extern "C" WEB_API void web_navigable_presenter_present(void* presenter, void* sealed_pointer, Web::Layout::RustFFI::FfiPresentedRecording const* presented)
{
    auto& sealed = *static_cast<Web::Compositor::SealedPresentation*>(sealed_pointer);
    auto display_list = Web::Painting::display_list_of_published_recording(sealed.recording.value(), *presented);
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->present_beside_event_loop(sealed, move(display_list));
}
