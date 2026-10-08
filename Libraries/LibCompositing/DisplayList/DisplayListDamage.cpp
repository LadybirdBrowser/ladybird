/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/DisplayList/AccumulatedVisualContext.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCompositing/DisplayList/DisplayListDamage.h>
#include <LibCompositing/RustFFI.h>
#include <LibCompositing/Scrolling/ScrollState.h>

namespace Compositing {

Optional<Gfx::IntRect> compute_display_list_damage(
    DisplayList const& old_display_list,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    DisplayList const& new_display_list,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect)
{
    auto old_scroll_offsets = old_scroll_state.device_offsets();
    auto new_scroll_offsets = new_scroll_state.device_offsets();
    Gfx::IntRect damage_rect;
    bool damage_is_bounded = Compositing::RustFFI::display_list_compute_damage(
        old_display_list.rust_handle(), old_visual_context_tree.rust_handle(), old_scroll_offsets.data(), old_scroll_offsets.size(),
        new_display_list.rust_handle(), new_visual_context_tree.rust_handle(), new_scroll_offsets.data(), new_scroll_offsets.size(),
        viewport_rect, &damage_rect);
    if (!damage_is_bounded)
        return {};
    return damage_rect;
}

AnimatedContentViewportEffect animated_content_may_affect_viewport(
    DisplayList const& display_list,
    AccumulatedVisualContextTree const& visual_context_tree,
    ScrollStateSnapshot const& scroll_state,
    Gfx::IntRect viewport_rect,
    i64 sample_time_ns)
{
    auto scroll_offsets = scroll_state.device_offsets();
    auto effect = Compositing::RustFFI::display_list_animated_content_may_affect_viewport(
        display_list.rust_handle(), visual_context_tree.rust_handle(), scroll_offsets.data(), scroll_offsets.size(), viewport_rect, sample_time_ns);
    return {
        .may_affect_viewport = effect.may_affect_viewport,
        .stable_until_scene_changes = effect.stable_until_scene_changes,
    };
}

}
