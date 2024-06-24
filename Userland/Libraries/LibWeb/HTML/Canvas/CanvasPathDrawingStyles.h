/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/CanvasState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvaspathdrawingstyles
template<typename IncludingClass>
class CanvasPathDrawingStyles {
public:
    ~CanvasPathDrawingStyles() = default;

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linewidth
    void set_line_width(float line_width)
    {
        // On setting, zero, negative, infinite, and NaN values must be ignored, leaving the value unchanged;
        if (line_width <= 0 || !isfinite(line_width))
            return;
        // other values must change the current value to the new value.
        my_drawing_state().line_width = line_width;
    }
    float line_width() const
    {
        // On getting, it must return the current value.
        return my_drawing_state().line_width;
    }

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-setlinedash
    void set_line_dash(Vector<double> segments)
    {
        // 1. If any value in segments is not finite (e.g. an Infinity or a NaN value), or if any value is negative (less than zero), then return (without throwing an exception; user agents could show a message on a developer console, though, as that would be helpful for debugging).
        for (auto const& segment : segments) {
            if (!isfinite(segment) || segment < 0)
                return;
        }

        // 2. If the number of elements in segments is odd, then let segments be the concatenation of two copies of segments.
        if (segments.size() % 2 == 1)
            segments.extend(segments);

        // 3. Let the object's dash list be segments.
        my_drawing_state().dash_list = segments;
    }

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getlinedash
    Vector<double> get_line_dash()
    {
        return my_drawing_state().dash_list;
    }

protected:
    CanvasPathDrawingStyles() = default;

private:
    CanvasState::DrawingState& my_drawing_state() { return reinterpret_cast<IncludingClass&>(*this).drawing_state(); }
    CanvasState::DrawingState const& my_drawing_state() const { return reinterpret_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
