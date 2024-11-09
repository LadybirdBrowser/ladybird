/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Color.h>
#include <LibGfx/DeprecatedPath.h>
#include <LibGfx/Forward.h>
#include <LibGfx/LineStyle.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Quad.h>
#include <LibGfx/WindingRule.h>

namespace Gfx {

struct CornerRadius {
    int horizontal_radius { 0 };
    int vertical_radius { 0 };

    inline operator bool() const
    {
        return horizontal_radius > 0 && vertical_radius > 0;
    }
};

class AntiAliasingPainter {
public:
    explicit AntiAliasingPainter(DeprecatedPainter& painter)
        : m_underlying_painter(painter)
    {
    }

    void fill_path(DeprecatedPath const&, Color, WindingRule rule = WindingRule::Nonzero);
    void fill_path(DeprecatedPath const&, PaintStyle const& paint_style, float opacity = 1.0f, WindingRule rule = WindingRule::Nonzero);

    void stroke_path(DeprecatedPath const&, Color, float thickness);
    void stroke_path(DeprecatedPath const&, PaintStyle const& paint_style, float thickness, float opacity = 1.0f);

private:
    DeprecatedPainter& m_underlying_painter;
};

}
