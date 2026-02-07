/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasPathDrawingStyles.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linewidth
template<typename IncludingClass>
void CanvasPathDrawingStyles<IncludingClass>::set_line_width(float line_width)
{
    // On setting, zero, negative, infinite, and NaN values must be ignored, leaving the value unchanged;
    if (line_width <= 0 || !isfinite(line_width))
        return;
    // other values must change the current value to the new value.
    my_drawing_state().line_width = line_width;
}
template<typename IncludingClass>
float CanvasPathDrawingStyles<IncludingClass>::line_width() const
{
    // On getting, it must return the current value.
    return my_drawing_state().line_width;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linecap
template<typename IncludingClass>
void CanvasPathDrawingStyles<IncludingClass>::set_line_cap(Bindings::CanvasLineCap line_cap)
{
    // On setting, the current value must be changed to the new value.
    my_drawing_state().line_cap = line_cap;
}
template<typename IncludingClass>
Bindings::CanvasLineCap CanvasPathDrawingStyles<IncludingClass>::line_cap() const
{
    // On getting, it must return the current value.
    return my_drawing_state().line_cap;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linejoin
template<typename IncludingClass>
void CanvasPathDrawingStyles<IncludingClass>::set_line_join(Bindings::CanvasLineJoin line_join)
{
    // On setting, the current value must be changed to the new value.
    my_drawing_state().line_join = line_join;
}
template<typename IncludingClass>
Bindings::CanvasLineJoin CanvasPathDrawingStyles<IncludingClass>::line_join() const
{
    // On getting, it must return the current value.
    return my_drawing_state().line_join;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-miterlimit
template<typename IncludingClass>
void CanvasPathDrawingStyles<IncludingClass>::set_miter_limit(float miter_limit)
{
    // On setting, zero, negative, infinite, and NaN values must be ignored, leaving the value unchanged;
    if (miter_limit <= 0 || !isfinite(miter_limit))
        return;
    // other values must change the current value to the new value.
    my_drawing_state().miter_limit = miter_limit;
}
template<typename IncludingClass>
float CanvasPathDrawingStyles<IncludingClass>::miter_limit() const
{
    // On getting, it must return the current value.
    return my_drawing_state().miter_limit;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-setlinedash
template<typename IncludingClass>
void CanvasPathDrawingStyles<IncludingClass>::set_line_dash(Vector<double> segments)
{
    // The setLineDash(segments) method, when invoked, must run these steps:

    // 1. If any value in segments is not finite (e.g. an Infinity or a NaN value), or if any value is negative (less than zero), then return
    //    (without throwing an exception; user agents could show a message on a developer console, though, as that would be helpful for debugging).
    for (auto const& segment : segments) {
        if (!isfinite(segment) || segment < 0)
            return;
    }

    // 2. If the number of elements in segments is odd, then let segments be the concatenation of two copies of segments.
    if (segments.size() % 2 == 1)
        segments.extend(segments);

    // 3. Set the object's dash list to segments.
    my_drawing_state().dash_list = move(segments);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getlinedash
template<typename IncludingClass>
Vector<double> CanvasPathDrawingStyles<IncludingClass>::get_line_dash()
{
    // When the getLineDash() method is invoked, it must return a sequence whose values are the values of the object's dash list, in the same order.
    return my_drawing_state().dash_list;
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linedashoffset
template<typename IncludingClass>
void CanvasPathDrawingStyles<IncludingClass>::set_line_dash_offset(float line_dash_offset)
{
    // On setting, infinite and NaN values must be ignored, leaving the value unchanged;
    if (!isfinite(line_dash_offset))
        return;
    // other values must change the current value to the new value.
    my_drawing_state().line_dash_offset = line_dash_offset;
}
template<typename IncludingClass>
float CanvasPathDrawingStyles<IncludingClass>::line_dash_offset() const
{
    // On getting, it must return the current value.
    return my_drawing_state().line_dash_offset;
}

template class CanvasPathDrawingStyles<CanvasRenderingContext2D>;
template class CanvasPathDrawingStyles<OffscreenCanvasRenderingContext2D>;

}
