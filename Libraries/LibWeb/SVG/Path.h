/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>

namespace Web::SVG {

struct MoveToInstruction {
    bool absolute;
    Gfx::FloatPoint point;

    bool operator==(MoveToInstruction const&) const = default;
};

struct ClosePathInstruction {
    bool operator==(ClosePathInstruction const&) const = default;
};

struct LineToInstruction {
    bool absolute;
    Gfx::FloatPoint point;

    bool operator==(LineToInstruction const&) const = default;
};

struct HorizontalLineToInstruction {
    bool absolute;
    float x;

    bool operator==(HorizontalLineToInstruction const&) const = default;
};

struct VerticalLineToInstruction {
    bool absolute;
    float y;

    bool operator==(VerticalLineToInstruction const&) const = default;
};

struct CurveToInstruction {
    bool absolute;
    Gfx::FloatPoint control_point_1;
    Gfx::FloatPoint control_point_2;
    Gfx::FloatPoint point;

    bool operator==(CurveToInstruction const&) const = default;
};

struct SmoothCurveToInstruction {
    bool absolute;
    Gfx::FloatPoint control_point_2;
    Gfx::FloatPoint point;

    bool operator==(SmoothCurveToInstruction const&) const = default;
};

struct QuadraticBezierCurveToInstruction {
    bool absolute;
    Gfx::FloatPoint control_point;
    Gfx::FloatPoint point;

    bool operator==(QuadraticBezierCurveToInstruction const&) const = default;
};

struct SmoothQuadraticBezierCurveToInstruction {
    bool absolute;
    Gfx::FloatPoint point;

    bool operator==(SmoothQuadraticBezierCurveToInstruction const&) const = default;
};

struct EllipticalArcInstruction {
    float rx;
    float ry;
    float x_axis_rotation;
    bool absolute;
    bool large_arc;
    bool sweep;
    Gfx::FloatPoint point;

    bool operator==(EllipticalArcInstruction const&) const = default;
};

using PathInstruction = Variant<MoveToInstruction, ClosePathInstruction, LineToInstruction, HorizontalLineToInstruction, VerticalLineToInstruction, CurveToInstruction, SmoothCurveToInstruction, QuadraticBezierCurveToInstruction, SmoothQuadraticBezierCurveToInstruction, EllipticalArcInstruction>;

class Path {
public:
    Path() = default;

    explicit Path(Vector<PathInstruction> instructions)
        : m_instructions(move(instructions))
    {
    }
    ReadonlySpan<PathInstruction> instructions() const { return m_instructions; }

    [[nodiscard]] Gfx::Path to_gfx_path() const;
    String serialize() const;

    bool operator==(Path const&) const = default;

private:
    Vector<PathInstruction> m_instructions;
};

}
