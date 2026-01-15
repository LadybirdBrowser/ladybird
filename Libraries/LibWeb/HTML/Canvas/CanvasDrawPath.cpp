/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibWeb/HTML/Canvas/CanvasDrawPath.h>

namespace Web::HTML {

[[maybe_unused]] static Gfx::WindingRule parse_fill_rule(StringView fill_rule)
{
    if (fill_rule == "evenodd"sv)
        return Gfx::WindingRule::EvenOdd;
    if (fill_rule == "nonzero"sv)
        return Gfx::WindingRule::Nonzero;
    dbgln("Unrecognized fillRule for CRC2D.fill() - this problem goes away once we pass an enum instead of a string");
    return Gfx::WindingRule::Nonzero;
}

void CanvasDrawPath::begin_path()
{
    path().clear();
}

void CanvasDrawPath::fill(StringView fill_rule)
{
    fill_internal(path(), parse_fill_rule(fill_rule));
}

void CanvasDrawPath::fill(Path2D& path, StringView fill_rule)
{
    fill_internal(path.path(), parse_fill_rule(fill_rule));
}

void CanvasDrawPath::stroke()
{
    stroke_internal(path());
}
void CanvasDrawPath::stroke(Path2D const& path)
{
    stroke_internal(path.path());
}

void CanvasDrawPath::clip(StringView fill_rule)
{
    clip_internal(path(), parse_fill_rule(fill_rule));
}
void CanvasDrawPath::clip(Path2D& path, StringView fill_rule)
{
    clip_internal(path.path(), parse_fill_rule(fill_rule));
}

static bool is_point_in_path_internal(Gfx::Path path, Gfx::AffineTransform const& transform, double x, double y, StringView fill_rule)
{
    auto point = Gfx::FloatPoint(x, y);
    if (auto inverse_transform = transform.inverse(); inverse_transform.has_value())
        point = inverse_transform->map(point);
    return path.contains(point, parse_fill_rule(fill_rule));
}

bool CanvasDrawPath::is_point_in_path(double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path(), drawing_state().transform, x, y, fill_rule);
}

bool CanvasDrawPath::is_point_in_path(Path2D const& path, double x, double y, StringView fill_rule)
{
    return is_point_in_path_internal(path.path(), drawing_state().transform, x, y, fill_rule);
}

}
