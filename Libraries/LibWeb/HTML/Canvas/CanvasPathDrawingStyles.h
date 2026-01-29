/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Canvas/DrawingState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvaspathdrawingstyles
template<typename IncludingClass>
class CanvasPathDrawingStyles {
public:
    ~CanvasPathDrawingStyles() = default;

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linewidth
    void set_line_width(float line_width);
    float line_width() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linecap
    void set_line_cap(Bindings::CanvasLineCap line_cap);
    Bindings::CanvasLineCap line_cap() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linejoin
    void set_line_join(Bindings::CanvasLineJoin line_join);
    Bindings::CanvasLineJoin line_join() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-miterlimit
    void set_miter_limit(float miter_limit);
    float miter_limit() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-setlinedash
    void set_line_dash(Vector<double> segments);

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-getlinedash
    Vector<double> get_line_dash();

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-linedashoffset
    void set_line_dash_offset(float line_dash_offset);
    float line_dash_offset() const;

protected:
    CanvasPathDrawingStyles() = default;

private:
    Web::HTML::DrawingState& my_drawing_state() { return static_cast<IncludingClass&>(*this).drawing_state(); }
    Web::HTML::DrawingState const& my_drawing_state() const { return static_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
