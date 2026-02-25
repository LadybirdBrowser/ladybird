/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#canvastransform
template<typename IncludingClass>
class CanvasTransform {
public:
    ~CanvasTransform() = default;

    Gfx::Path& mutable_path() { return static_cast<IncludingClass&>(*this).path(); }

    void scale(float sx, float sy);

    void translate(float tx, float ty);

    void rotate(float radians);

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-transform
    void transform(double a, double b, double c, double d, double e, double f);

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-gettransform
    WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> get_transform();

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform
    void set_transform(double a, double b, double c, double d, double e, double f);

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform-matrix
    WebIDL::ExceptionOr<void> set_transform(Geometry::DOMMatrix2DInit& init);

    // https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-resettransform
    void reset_transform();

    void flush_transform();

protected:
    CanvasTransform() = default;

private:
    DrawingState& my_drawing_state() { return static_cast<IncludingClass&>(*this).drawing_state(); }
    DrawingState const& my_drawing_state() const { return static_cast<IncludingClass const&>(*this).drawing_state(); }
};

}
