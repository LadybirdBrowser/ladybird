/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/AffineTransform.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Vector2.h>
#include <LibGfx/WindingRule.h>
#include <LibIPC/Forward.h>

namespace Gfx {

namespace FFI {

struct PathGeometry;
struct PathGeometryData;

}

struct PointAlongPath {
    FloatPoint point;
    FloatVector2 tangent;
};

struct PathVertex {
    FloatPoint point;
    float distance;
    FloatVector2 direction;
};

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

    void clear();
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

    struct StrokeStyle {
        float thickness { 1 };
        CapStyle cap_style { CapStyle::Butt };
        JoinStyle join_style { JoinStyle::Miter };
        float miter_limit { 4 };
        ReadonlySpan<float> dash_array;
        float dash_offset { 0 };
    };

    // The outline of the area that stroking this path with the given style covers, as a path to fill with the
    // nonzero winding rule.
    [[nodiscard]] Gfx::Path stroke_to_fill(StrokeStyle const&) const;

    void elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep);
    void arc_to(FloatPoint point, float radius, bool large_arc, bool sweep);
    void quadratic_bezier_curve_to(FloatPoint through, FloatPoint point);
    void cubic_bezier_curve_to(FloatPoint c1, FloatPoint c2, FloatPoint p2);
    void glyph_run(GlyphRun const&);

    void append_path(Gfx::Path const&);

    [[nodiscard]] Vector<u8> serialize_to_bytes() const;
    void serialize_into(void (*append)(void*, u8 const*, size_t), void* context) const;

    [[nodiscard]] bool is_empty() const;
    Gfx::FloatPoint last_point() const;
    Gfx::FloatRect bounding_box() const;
    float length() const;
    Optional<PointAlongPath> point_at_length(float distance) const;
    Vector<PathVertex> vertices() const;
    bool contains(FloatPoint point, Gfx::WindingRule) const;
    void set_fill_type(Gfx::WindingRule);

    Gfx::Path clone() const { return *this; }
    Gfx::Path copy_transformed(Gfx::AffineTransform const&) const;
    Gfx::Path place_glyph_runs_along(ReadonlySpan<NonnullRefPtr<GlyphRun>>, float offset = 0) const;

    String to_svg_string() const;

    void transform(Gfx::AffineTransform const&);

    PathData data() const;

    bool operator==(Path const&) const;

private:
    explicit Path(FFI::PathGeometry*);

    FFI::PathGeometry* m_geometry { nullptr };
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::Path const&);

template<>
ErrorOr<Gfx::Path> decode(Decoder&);

}
