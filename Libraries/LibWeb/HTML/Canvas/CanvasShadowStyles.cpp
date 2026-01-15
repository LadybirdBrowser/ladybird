/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasShadowStyles.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

template<typename IncludingClass>
float CanvasShadowStyles<IncludingClass>::shadow_offset_x() const
{
    return drawing_state().shadow_offset_x;
}

template<typename IncludingClass>
void CanvasShadowStyles<IncludingClass>::set_shadow_offset_x(float offset_x)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsetx
    if (!isfinite(offset_x))
        return;

    drawing_state().shadow_offset_x = offset_x;
}

template<typename IncludingClass>
float CanvasShadowStyles<IncludingClass>::shadow_offset_y() const
{
    return drawing_state().shadow_offset_y;
}

template<typename IncludingClass>
void CanvasShadowStyles<IncludingClass>::set_shadow_offset_y(float offset_y)
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowoffsety
    if (!isfinite(offset_y))
        return;

    drawing_state().shadow_offset_y = offset_y;
}

template<typename IncludingClass>
float CanvasShadowStyles<IncludingClass>::shadow_blur() const
{
    return drawing_state().shadow_blur;
}

template<typename IncludingClass>
void CanvasShadowStyles<IncludingClass>::set_shadow_blur(float blur_radius)
{
    // On setting, the attribute must be set to the new value,
    // except if the value is negative, infinite or NaN, in which case the new value must be ignored.
    if (blur_radius < 0 || isinf(blur_radius) || isnan(blur_radius))
        return;

    drawing_state().shadow_blur = blur_radius;
}

template<typename IncludingClass>
String CanvasShadowStyles<IncludingClass>::shadow_color() const
{
    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-shadowcolor
    return drawing_state().shadow_color.to_string(Gfx::Color::HTMLCompatibleSerialization::Yes);
}

template class CanvasShadowStyles<CanvasRenderingContext2D>;
template class CanvasShadowStyles<OffscreenCanvasRenderingContext2D>;

}
