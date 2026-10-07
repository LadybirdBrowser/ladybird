/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Font/Font.h>
#include <LibWeb/Compositor/NavigablePresenter.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Compositor {

// What the Paint thread presents a frame through. Only Rust calls it, from a job that presents.
struct PresenterFFI {
    // A clock lane's frame that starts from an older frame than the last one a rendering update committed shows what the
    // screen no longer shows: it is not presented, and the compositor hears nothing of it.
    static bool is_stale(NavigablePresenter const& presenter, SealedPresentation const& sealed)
    {
        return sealed.presented_by == PresentedBy::Clock && sealed.generation < presenter.m_last_committed_generation;
    }

    static void note_presented(NavigablePresenter& presenter, SealedPresentation& sealed)
    {
        if (sealed.presented_by == PresentedBy::Commit) {
            presenter.m_last_committed_generation = max(presenter.m_last_committed_generation, sealed.generation);
            presenter.m_committed_command_resources = presenter.m_compositor_display_list_command_resources;
        }
        if (sealed.published.has_value())
            sealed.published->referenced_resources = presenter.m_compositor_display_list_command_resources;
    }

    static void present(NavigablePresenter& presenter, SealedPresentation& sealed, NonnullRefPtr<Compositing::DisplayList> display_list, CompositorFrameSink* sink)
    {
        MutexLocker locker(presenter.m_mutex);
        if (is_stale(presenter, sealed))
            return;
        auto frame = presenter.build_frame_beside_event_loop(sealed, move(display_list));
        note_presented(presenter, sealed);
        if (sink)
            sink->submit(move(frame));
    }

    static void present_unrecorded(NavigablePresenter& presenter, SealedPresentation& sealed, CompositorFrameSink* sink)
    {
        MutexLocker locker(presenter.m_mutex);
        if (is_stale(presenter, sealed))
            return;
        auto frame = presenter.build_frame(sealed, {});
        note_presented(presenter, sealed);
        if (sink)
            sink->submit(move(frame));
    }
};

void NavigablePresenter::forget_compositor_display_list()
{
    MutexLocker locker(m_mutex);
    m_compositor_visual_context_tree.clear();
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
        // The next frame a rendering update commits copies paint commands from the one it committed last, whatever a
        // clock lane presented since.
        if (sealed.presented_by == PresentedBy::Clock)
            resources.include(m_committed_command_resources);
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

    m_last_frame_presented_by = sealed.presented_by;
    CompositorFrame frame;
    frame.context_id = sealed.context_id;
    frame.present_viewport_rect = sealed.present_viewport_rect;
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
        m_compositor_display_list_visual_context_tree_structural_epoch = published->display_list->compatible_visual_context_tree_structural_epoch();
        m_compositor_visual_context_tree = visual_context_tree;
        m_resource_storage.retain_only(resources);
        m_compositor_display_list = published->display_list;
        m_compositor_display_list_command_resources = move(command_resources);
        m_compositor_display_list_resources = move(resources);
        m_compositor_display_list_paint_config = sealed.paint_config;
        return frame;
    }

    if (display_list_is_unchanged) {
        m_compositor_display_list_paint_config = sealed.paint_config;
        if (m_resource_storage.has_resources_added_since_last_retain())
            m_resource_storage.retain_only(m_compositor_display_list_resources);
    }
    if (sealed.sends_visual_context_tree) {
        auto const& visual_context_tree = sealed.visual_context_tree.value();
        VERIFY(sealed.visual_context_tree_mismatches_for_testing || visual_context_tree.structural_epoch() == m_compositor_display_list_visual_context_tree_structural_epoch);
        auto resources = resources_with(m_compositor_display_list_command_resources, visual_context_tree);
        frame.visual_context_tree_update = CompositorFrame::VisualContextTreeUpdate {
            .visual_context_tree = visual_context_tree,
            .resource_transaction = m_resource_storage.create_transaction(m_compositor_display_list_resources, resources),
        };
        m_resource_storage.retain_only(resources);
        m_compositor_display_list_resources = move(resources);
        m_compositor_visual_context_tree = visual_context_tree;
    }
    frame.scroll_state_update = CompositorFrame::ScrollStateUpdate {
        .scroll_state_snapshot = sealed.scroll_state_snapshot,
        .keyboard_scroll_state = move(keyboard_scroll_state),
    };
    return frame;
}

CompositorFrame NavigablePresenter::build_frame_beside_event_loop(SealedPresentation& sealed, NonnullRefPtr<Compositing::DisplayList> display_list)
{
    bool const replaces_paint_command_cache_source = sealed.recording->cache_mode == Painting::PaintCommandCacheMode::ReadWrite
        && display_list != sealed.paint_command_cache_source;
    PublishedDisplayList published { move(display_list), replaces_paint_command_cache_source, {} };
    auto frame = build_frame(sealed, published);
    if (replaces_paint_command_cache_source) {
        sealed.paint_command_cache_source = published.display_list;
        sealed.paint_command_cache_source_resources = m_compositor_display_list_command_resources;
        sealed.recording->paint_command_cache_source = published.display_list;
    }
    sealed.published = move(published);
    return frame;
}

OwnPtr<SealedPresentation> NavigablePresenter::seal_for_clock_lane(SealedPresentation const& committed) const
{
    MutexLocker locker(m_mutex);
    // A lane records the document again as the frame's recording did, or as the one an unrecorded frame keeps, which the
    // compositor has to show: a clock frame presented before the committed one may have taken its place.
    RefPtr<Compositing::DisplayList> source = committed.paint_command_cache_source;
    if (committed.published.has_value())
        source = committed.published->display_list;
    if (!source || source != m_compositor_display_list || !m_compositor_visual_context_tree.has_value())
        return nullptr;
    auto sealed = make<SealedPresentation>();
    sealed->paint_config = committed.paint_config;
    sealed->visual_context_tree = *m_compositor_visual_context_tree;
    sealed->scroll_state_snapshot = committed.scroll_state_snapshot;
    sealed->keyboard_scroll_state = committed.keyboard_scroll_state;
    sealed->paint_command_cache_source = m_compositor_display_list;
    sealed->paint_command_cache_source_resources = m_compositor_display_list_command_resources;
    sealed->recording = Painting::DisplayListRecording {
        .visual_context_tree = *m_compositor_visual_context_tree,
        .placeholder_display_list = Compositing::DisplayList::create(*m_compositor_visual_context_tree),
        .cache_mode = Painting::PaintCommandCacheMode::ReadWrite,
        .in_flight = true,
        .async_scrolling_metadata = m_compositor_display_list->async_scrolling_metadata(),
        .paint_command_cache_source = m_compositor_display_list,
    };
    if (auto color = m_compositor_display_list->surface_clear_color(); color.has_value())
        sealed->recording->placeholder_display_list->set_surface_clear_color(*color);
    sealed->context_id = committed.context_id;
    sealed->present_viewport_rect = committed.present_viewport_rect;
    sealed->presented_by = PresentedBy::Clock;
    sealed->generation = committed.generation;
    return sealed;
}

}

extern "C" WEB_API void web_navigable_presenter_unref(void* presenter)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->unref();
}

extern "C" WEB_API void* web_navigable_presenter_seal_for_clock_lane(void* presenter, void const* committed)
{
    auto const& navigable_presenter = *static_cast<Web::Compositor::NavigablePresenter const*>(presenter);
    auto sealed = navigable_presenter.seal_for_clock_lane(*static_cast<Web::Compositor::SealedPresentation const*>(committed));
    if (!sealed)
        return nullptr;
    navigable_presenter.ref();
    return sealed.leak_ptr();
}

extern "C" WEB_API void web_sealed_presentation_destroy(void* sealed)
{
    delete static_cast<Web::Compositor::SealedPresentation*>(sealed);
}

extern "C" WEB_API void web_sealed_presentation_take_visual_context_tree(void* sealed_pointer, void const* tree, Gfx::FloatPoint const* restructured_scroll_offsets, size_t scroll_offset_count)
{
    auto& sealed = *static_cast<Web::Compositor::SealedPresentation*>(sealed_pointer);
    auto visual_context_tree = Compositing::AccumulatedVisualContextTree::adopt_rust_handle(tree);
    // A tree of another structure takes the scroll offsets of its own nodes, and a new display list recorded against it.
    // A frame that records nothing seals no tree, and takes the render state's: the compositor's display list was cut
    // from a tree of its structure.
    if (restructured_scroll_offsets)
        sealed.scroll_state_snapshot.assign_device_offsets({ restructured_scroll_offsets, scroll_offset_count });
    else if (sealed.visual_context_tree.has_value())
        VERIFY(visual_context_tree.structural_epoch() == sealed.visual_context_tree->structural_epoch());
    if (sealed.recording.has_value())
        sealed.recording->visual_context_tree = visual_context_tree;
    sealed.visual_context_tree = move(visual_context_tree);
    // A frame whose display list is the one the compositor has takes the tree on its own.
    sealed.sends_visual_context_tree = true;
}

extern "C" WEB_API void web_navigable_presenter_add_font(void* presenter, void const* font)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().add_font(*static_cast<Gfx::Font const*>(font));
}

extern "C" WEB_API void web_navigable_presenter_add_image_frame(void* presenter, void const* frame)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().add_image_frame(*static_cast<Gfx::DecodedImageFrame const*>(frame));
}

extern "C" WEB_API void web_vector_image_resources_destroy(void* resources)
{
    delete static_cast<Web::Compositor::VectorImageResources*>(resources);
}

extern "C" WEB_API void web_navigable_presenter_take_vector_image_resources(void* presenter, void* resources_pointer, u64 const* display_list_ids, size_t display_list_id_count)
{
    auto& storage = static_cast<Web::Compositor::VectorImageResources*>(resources_pointer)->storage;
    Compositing::DisplayListResourceSet resources;
    for (auto raw_id : ReadonlySpan<u64> { display_list_ids, display_list_id_count }) {
        Compositing::DisplayListResourceId id { raw_id };
        resources.display_lists.set(id);
        resources.include(storage.collect_referenced_resources(storage.display_list(id)));
        resources.include(storage.collect_referenced_resources(storage.display_list_visual_context_tree(id)));
    }
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().apply_transaction(storage.create_transaction({}, resources));
}

extern "C" WEB_API void web_navigable_presenter_add_video_sink(void* presenter, u64 resource_id, u64 sink_handle)
{
    static_cast<Web::Compositor::NavigablePresenter*>(presenter)->display_list_resource_storage().add_video_sink(Compositing::VideoSinkResourceId { resource_id }, Media::VideoSinkHandle { sink_handle });
}

// Declared here, not in a header, so that no C++ presents a frame through them.
extern "C" WEB_API void web_navigable_presenter_present(void* presenter, void* sealed, Web::Layout::RustFFI::FfiPresentedRecording const* presented, void* sink);
extern "C" WEB_API void web_navigable_presenter_present_unrecorded(void* presenter, void* sealed, void* sink);

extern "C" WEB_API void web_navigable_presenter_present_unrecorded(void* presenter, void* sealed, void* sink)
{
    Web::Compositor::PresenterFFI::present_unrecorded(*static_cast<Web::Compositor::NavigablePresenter*>(presenter), *static_cast<Web::Compositor::SealedPresentation*>(sealed), static_cast<Web::Compositor::CompositorFrameSink*>(sink));
}

extern "C" WEB_API void web_navigable_presenter_present(void* presenter, void* sealed_pointer, Web::Layout::RustFFI::FfiPresentedRecording const* presented, void* sink)
{
    auto& sealed = *static_cast<Web::Compositor::SealedPresentation*>(sealed_pointer);
    auto display_list = Web::Painting::display_list_of_published_recording(sealed.recording.value(), *presented);
    Web::Compositor::PresenterFFI::present(*static_cast<Web::Compositor::NavigablePresenter*>(presenter), sealed, move(display_list), static_cast<Web::Compositor::CompositorFrameSink*>(sink));
}
