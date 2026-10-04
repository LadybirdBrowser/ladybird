/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Compositor/NavigablePresenter.h>

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
        keyboard_scroll_state.visual_context_tree_structural_epoch = display_list.compatible_visual_context_tree_structural_epoch();
    }
    auto async_scrolling_metadata = display_list.async_scrolling_metadata().value_or({});
    async_scrolling_metadata.keyboard_scroll_state = keyboard_scroll_state;
    display_list.set_async_scrolling_metadata(move(async_scrolling_metadata));

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

}
