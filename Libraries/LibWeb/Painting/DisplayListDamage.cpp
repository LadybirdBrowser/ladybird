/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayList.h>
#include <LibWeb/Painting/DisplayListDamage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

Optional<Gfx::IntRect> compute_display_list_damage(
    DisplayList const& old_display_list,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    DisplayList const& new_display_list,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect)
{
    auto old_command_bytes = old_display_list.command_bytes();
    auto old_command_runs = old_display_list.command_runs();
    auto old_scroll_offsets = old_scroll_state.device_offsets();
    auto new_command_bytes = new_display_list.command_bytes();
    auto new_command_runs = new_display_list.command_runs();
    auto new_scroll_offsets = new_scroll_state.device_offsets();
    Gfx::IntRect damage_rect;
    bool damage_is_bounded = Compositing::RustFFI::display_list_compute_damage(
        old_command_bytes.data(), old_command_bytes.size(), old_command_runs.data(), old_command_runs.size(), old_visual_context_tree.rust_handle(), old_scroll_offsets.data(), old_scroll_offsets.size(),
        new_command_bytes.data(), new_command_bytes.size(), new_command_runs.data(), new_command_runs.size(), new_visual_context_tree.rust_handle(), new_scroll_offsets.data(), new_scroll_offsets.size(),
        viewport_rect, &damage_rect);
    if (!damage_is_bounded)
        return {};
    return damage_rect;
}

AnimatedContentViewportEffect animated_content_may_affect_viewport(
    ReadonlyBytes display_list_commands,
    AccumulatedVisualContextTree const& visual_context_tree,
    ScrollStateSnapshot const& scroll_state,
    Gfx::IntRect viewport_rect,
    i64 sample_time_ns)
{
    auto scroll_offsets = scroll_state.device_offsets();
    auto effect = Compositing::RustFFI::display_list_animated_content_may_affect_viewport(
        display_list_commands.data(), display_list_commands.size(), visual_context_tree.rust_handle(),
        scroll_offsets.data(), scroll_offsets.size(), viewport_rect, sample_time_ns);
    return {
        .may_affect_viewport = effect.may_affect_viewport,
        .stable_until_scene_changes = effect.stable_until_scene_changes,
    };
}

}
