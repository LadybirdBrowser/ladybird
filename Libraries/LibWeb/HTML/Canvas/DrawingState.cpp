/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DrawingState.h"

namespace Web::HTML {

NonnullRefPtr<Gfx::PaintStyle> FillOrStrokeStyle::to_gfx_paint_style()
{
    return m_fill_or_stroke_style.visit(
        [&](Gfx::Color color) -> NonnullRefPtr<Gfx::PaintStyle> {
            if (!m_color_paint_style)
                m_color_paint_style = Gfx::SolidColorPaintStyle::create(color).release_value_but_fixme_should_propagate_errors();
            return m_color_paint_style.release_nonnull();
        },
        [&](auto handle) {
            return handle->to_gfx_paint_style();
        });
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
