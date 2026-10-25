/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

namespace FFI {

struct PathGeometry;
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

class Path {
public:
    AK_ALLOC_WITH_KMALLOC;

    Path();
    ~Path();

    Path(Path const&);
    Path& operator=(Path const&);
    Path(Path&&);
    Path& operator=(Path&&);

    [[nodiscard]] static Path from_serialized_bytes(ReadonlyBytes);

    void move_to(Gfx::FloatPoint const&);
    void line_to(Gfx::FloatPoint const&);
    void close();

    enum class CapStyle {
        Butt,
        Round,
        Square,
    };

    enum class JoinStyle {
        Miter,
        Round,
        Bevel,
    };

    Gfx::Path clone() const { return *this; }

    PathData data() const;

private:
    explicit Path(FFI::PathGeometry*);

    FFI::PathGeometry* m_geometry { nullptr };
};

}
