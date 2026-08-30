/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Painting/AccumulatedVisualContext.h>
#include <LibWeb/Painting/DisplayListDamage.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

Optional<Gfx::IntRect> compute_display_list_damage(
    ReadonlyBytes old_display_list_commands,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    ReadonlyBytes new_display_list_commands,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect)
{
    auto old_scroll_offsets = old_scroll_state.device_offsets();
    auto new_scroll_offsets = new_scroll_state.device_offsets();
    Gfx::IntRect damage_rect;
    bool damage_is_bounded = Layout::RustFFI::display_list_compute_damage(
        old_display_list_commands.data(), old_display_list_commands.size(), old_visual_context_tree.rust_handle(), old_scroll_offsets.data(), old_scroll_offsets.size(),
        new_display_list_commands.data(), new_display_list_commands.size(), new_visual_context_tree.rust_handle(), new_scroll_offsets.data(), new_scroll_offsets.size(),
        viewport_rect, &damage_rect);
    if (!damage_is_bounded)
        return {};
    return damage_rect;
}

}
