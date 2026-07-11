/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Span.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Export.h>

namespace Web::Painting {

class AccumulatedVisualContextTree;
class ScrollStateSnapshot;

WEB_API Optional<Gfx::IntRect> compute_display_list_damage(
    ReadonlyBytes old_display_list_commands,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    ReadonlyBytes new_display_list_commands,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect);

}
