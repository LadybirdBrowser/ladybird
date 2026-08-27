/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Matrix4x4.h>
#include <LibGfx/Vector3.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/ContextRef.h>
#include <LibWeb/Painting/DisplayListCommand.h>

namespace Web::Painting {

struct PushPlaneClip {
    Vector<Gfx::FloatVector3, 8> vertices;
};
struct PopPlaneClip { };
// A span step names consecutive entries of the command run table the plan was built from.
using DepthSortedReplayStep = Variant<ReadonlySpan<DisplayListCommandRun>, PushPlaneClip, PopPlaneClip>;

Vector<DepthSortedReplayStep> build_depth_sorted_replay_plan(ReadonlySpan<DisplayListCommandRun> command_runs, AccumulatedVisualContextTree const&, ReadonlySpan<Gfx::FloatMatrix4x4> transform_palette, ReadonlySpan<SpatialNodeIndex> draw_space, ReadonlySpan<bool> backface_culled);

}
