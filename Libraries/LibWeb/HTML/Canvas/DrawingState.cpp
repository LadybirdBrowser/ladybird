/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DrawingState.h"

namespace Web::HTML {

bool FillOrStrokeStyle::is_visible() const
{
    return m_fill_or_stroke_style.visit(
        [](Gfx::Color color) { return color.alpha() > 0; },
        [](GC::Ref<CanvasGradient> gradient) { return gradient->is_visible(); },
        [](GC::Ref<CanvasPattern> pattern) { return pattern->to_gfx_paint_style()->is_visible(); });
}

Gfx::Color FillOrStrokeStyle::to_color_but_fixme_should_accept_any_paint_style() const
{
    return as_color().value_or(Gfx::Color::Black);
}

Optional<Gfx::Color> FillOrStrokeStyle::as_color() const
{
    if (auto* color = m_fill_or_stroke_style.get_pointer<Gfx::Color>())
        return *color;
    return {};
}

}
