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
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::Painting {

class AccumulatedVisualContextTree;
class ScrollStateSnapshot;

WEB_API Optional<Gfx::IntRect> compute_display_list_damage(
    DisplayList const& old_display_list,
    AccumulatedVisualContextTree const& old_visual_context_tree,
    ScrollStateSnapshot const& old_scroll_state,
    DisplayList const& new_display_list,
    AccumulatedVisualContextTree const& new_visual_context_tree,
    ScrollStateSnapshot const& new_scroll_state,
    Gfx::IntRect viewport_rect);

struct AnimatedContentViewportEffect {
    bool may_affect_viewport { true };
    // The answer holds until the scene changes: no finite animation is still running, so none can stop
    // contributing on its own.
    bool stable_until_scene_changes { false };
};

// Whether the content the tree's animations move at the sample time can reach the viewport.
WEB_API AnimatedContentViewportEffect animated_content_may_affect_viewport(
    ReadonlyBytes display_list_commands,
    AccumulatedVisualContextTree const&,
    ScrollStateSnapshot const&,
    Gfx::IntRect viewport_rect,
    i64 sample_time_ns);

}
