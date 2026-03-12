/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/Painter.h>
#include <LibWeb/HTML/Canvas/CanvasPath.h>
#include <LibWeb/HTML/Canvas/CanvasTransform.h>
#include <LibWeb/HTML/CanvasRenderingContext2D.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

template<typename IncludingClass>
void CanvasTransform<IncludingClass>::scale(float sx, float sy)
{
    dbgln_if(CANVAS_RENDERING_CONTEXT_2D_DEBUG, "CanvasTransform::scale({}, {})", sx, sy);
    if (!isfinite(sx) || !isfinite(sy))
        return;
    my_drawing_state().transform.scale(sx, sy);
    flush_transform();
    mutable_path().transform(Gfx::AffineTransform().scale(1.0 / sx, 1.0 / sy));
}

template<typename IncludingClass>
void CanvasTransform<IncludingClass>::translate(float tx, float ty)
{
    dbgln_if(CANVAS_RENDERING_CONTEXT_2D_DEBUG, "CanvasTransform::translate({}, {})", tx, ty);
    if (!isfinite(tx) || !isfinite(ty))
        return;
    my_drawing_state().transform.translate(tx, ty);
    flush_transform();
    mutable_path().transform(Gfx::AffineTransform().translate(-tx, -ty));
}

template<typename IncludingClass>
void CanvasTransform<IncludingClass>::rotate(float radians)
{
    dbgln_if(CANVAS_RENDERING_CONTEXT_2D_DEBUG, "CanvasTransform::rotate({})", radians);
    if (!isfinite(radians))
        return;
    my_drawing_state().transform.rotate_radians(radians);
    flush_transform();
    mutable_path().transform(Gfx::AffineTransform().rotate_radians(-radians));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-transform
template<typename IncludingClass>
void CanvasTransform<IncludingClass>::transform(double a, double b, double c, double d, double e, double f)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) || !isfinite(e) || !isfinite(f))
        return;

    // 2. Replace the current transformation matrix with the result of multiplying the current transformation matrix with the matrix described by:
    //    a c e
    //    b d f
    //    0 0 1
    auto transform = Gfx::AffineTransform(a, b, c, d, e, f);
    my_drawing_state().transform.multiply(transform);

    if (auto inverse = transform.inverse(); inverse.has_value()) {
        mutable_path().transform(inverse.value());
    }
    flush_transform();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-gettransform
template<typename IncludingClass>
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CanvasTransform<IncludingClass>::get_transform()
{
    auto& realm = static_cast<IncludingClass&>(*this).realm();
    auto transform = my_drawing_state().transform;
    Geometry::DOMMatrix2DInit init = { transform.a(), transform.b(), transform.c(), transform.d(), transform.e(), transform.f(), {}, {}, {}, {}, {}, {} };
    return Geometry::DOMMatrix::create_from_dom_matrix_2d_init(realm, init);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform
template<typename IncludingClass>
void CanvasTransform<IncludingClass>::set_transform(double a, double b, double c, double d, double e, double f)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(a) || !isfinite(b) || !isfinite(c) || !isfinite(d) || !isfinite(e) || !isfinite(f))
        return;

    // 2. Reset the current transformation matrix to the identity matrix.
    my_drawing_state().transform = {};
    flush_transform();

    // 3. Invoke the transform(a, b, c, d, e, f) method with the same arguments.
    transform(a, b, c, d, e, f);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-settransform-matrix
template<typename IncludingClass>
WebIDL::ExceptionOr<void> CanvasTransform<IncludingClass>::set_transform(Geometry::DOMMatrix2DInit& init)
{
    // 1. Let matrix be the result of creating a DOMMatrix from the 2D dictionary transform.
    auto& realm = static_cast<IncludingClass&>(*this).realm();
    auto matrix = TRY(Geometry::DOMMatrix::create_from_dom_matrix_2d_init(realm, init));

    // 2. If one or more of matrix's m11 element, m12 element, m21 element, m22 element, m41 element, or m42 element are infinite or NaN, then return.
    if (!isfinite(matrix->m11()) || !isfinite(matrix->m12()) || !isfinite(matrix->m21()) || !isfinite(matrix->m22()) || !isfinite(matrix->m41()) || !isfinite(matrix->m42()))
        return {};

    auto original_transform = my_drawing_state().transform;

    // 3. Reset the current transformation matrix to matrix.
    auto transform = Gfx::AffineTransform { static_cast<float>(matrix->a()), static_cast<float>(matrix->b()), static_cast<float>(matrix->c()), static_cast<float>(matrix->d()), static_cast<float>(matrix->e()), static_cast<float>(matrix->f()) };
    my_drawing_state().transform = transform;

    mutable_path().transform(original_transform);

    flush_transform();
    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-resettransform
template<typename IncludingClass>
void CanvasTransform<IncludingClass>::reset_transform()
{
    // The resetTransform() method, when invoked, must reset the current transformation matrix to the identity matrix.
    my_drawing_state().transform = {};
    flush_transform();
}

template<typename IncludingClass>
void CanvasTransform<IncludingClass>::flush_transform()
{
    if (auto* painter = static_cast<IncludingClass&>(*this).painter())
        painter->set_transform(my_drawing_state().transform);
}

template class CanvasTransform<CanvasRenderingContext2D>;
template class CanvasTransform<OffscreenCanvasRenderingContext2D>;

}
