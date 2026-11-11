/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Span.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

namespace FFI {

struct PathGeometryData;

}

enum class PathVerb : u8 {
    Move,
    Line,
    Quad,
    Conic,
    Cubic,
    Close,
};

// The contours of a path, valid until the path changes or is destroyed. A path with a point that is not finite has
// none.
struct PathData {
    static PathData from_ffi(FFI::PathGeometryData const&);

    ReadonlySpan<PathVerb> verbs;
    ReadonlySpan<FloatPoint> points;
    ReadonlySpan<float> conic_weights;
    WindingRule winding_rule { WindingRule::Nonzero };
};

}
