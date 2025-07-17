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

namespace Web::SVG {

enum class PathInstructionType : u8 {
    Move,
    ClosePath,
    Line,
    HorizontalLine,
    VerticalLine,
    Curve,
    SmoothCurve,
    QuadraticBezierCurve,
    SmoothQuadraticBezierCurve,
    EllipticalArc,
    Invalid,
};

struct PathInstruction {
    PathInstructionType type;
    bool absolute;
    Vector<float> data;

    bool operator==(PathInstruction const&) const = default;

    void serialize(StringBuilder&) const;

    void dump() const;
};

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
