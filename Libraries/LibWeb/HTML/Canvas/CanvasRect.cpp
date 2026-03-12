/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Painter.h>
#include <LibGfx/Path.h>
#include <LibWeb/HTML/Canvas/CanvasRect.h>

namespace Web::HTML {

[[nodiscard]] static Gfx::Path rect_path(float x, float y, float width, float height)
{
    auto top_left = Gfx::FloatPoint(x, y);
    auto top_right = Gfx::FloatPoint(x + width, y);
    auto bottom_left = Gfx::FloatPoint(x, y + height);
    auto bottom_right = Gfx::FloatPoint(x + width, y + height);

    Gfx::Path path;
    path.move_to(top_left);
    path.line_to(top_right);
    path.line_to(bottom_right);
    path.line_to(bottom_left);
    path.line_to(top_left);
    return path;
}

void CanvasRect::fill_rect(float x, float y, float width, float height)
{
    fill_internal(rect_path(x, y, width, height), Gfx::WindingRule::EvenOdd);
}

void CanvasRect::stroke_rect(float x, float y, float width, float height)
{
    stroke_internal(rect_path(x, y, width, height));
}

void CanvasRect::clear_rect(float x, float y, float width, float height)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y) || !isfinite(width) || !isfinite(height))
        return;

    if (auto* painter = this->painter()) {
        auto rect = Gfx::FloatRect(x, y, width, height);
        painter->clear_rect(rect, clear_color());
        did_draw(rect);
    }
}

}
