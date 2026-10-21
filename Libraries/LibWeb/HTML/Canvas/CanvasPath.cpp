/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasPath.h>

namespace Web::HTML {

namespace {

// `CanvasPathResultKind` and `CanvasPathResult` of Bindings/Rust/src/html/exports.rs.
enum class CanvasPathResultKind : u8 {
    Ok,
    IndexSizeError,
    RangeError,
};

struct CanvasPathResult {
    CanvasPathResultKind kind;
    // An AK::Utf16String that the caller takes over, unless the kind is Ok.
    FlatPtr message;
};

}

// NB: `RoundRectRadius` of Bindings/Rust/src/html/canvas_path.rs is two doubles in this order.
static_assert(sizeof(CanvasRoundRectRadius) == 2 * sizeof(double));
static_assert(offsetof(CanvasRoundRectRadius, y) == sizeof(double));

// Defined in Bindings/Rust/src/html/exports.rs. The path is lent mutably for the call.
extern "C" {
void web_canvas_path_close_path(Gfx::Path*);
void web_canvas_path_move_to(Gfx::Path*, float x, float y);
void web_canvas_path_line_to(Gfx::Path*, float x, float y);
void web_canvas_path_quadratic_curve_to(Gfx::Path*, float cpx, float cpy, float x, float y);
void web_canvas_path_bezier_curve_to(Gfx::Path*, double cp1x, double cp1y, double cp2x, double cp2y, double x, double y);
CanvasPathResult web_canvas_path_arc_to(Gfx::Path*, Gfx::AffineTransform const*, double x1, double y1, double x2, double y2, double radius);
void web_canvas_path_rect(Gfx::Path*, double x, double y, double w, double h);
CanvasPathResult web_canvas_path_round_rect(Gfx::Path*, double x, double y, double w, double h, ReadonlySpan<CanvasRoundRectRadius> radii);
CanvasPathResult web_canvas_path_arc(Gfx::Path*, float x, float y, float radius, float start_angle, float end_angle, bool counter_clockwise);
CanvasPathResult web_canvas_path_ellipse(Gfx::Path*, float x, float y, float radius_x, float radius_y, float rotation, float start_angle, float end_angle, bool counter_clockwise);
}

static WebIDL::ExceptionOr<void> to_exception_or(CanvasPathResult result)
{
    switch (result.kind) {
    case CanvasPathResultKind::Ok:
        return {};
    case CanvasPathResultKind::IndexSizeError:
        return WebIDL::IndexSizeError::create(Utf16String::adopt_raw(result.message));
    case CanvasPathResultKind::RangeError:
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, Utf16String::adopt_raw(result.message) };
    }
    VERIFY_NOT_REACHED();
}

Gfx::AffineTransform CanvasPath::active_transform() const
{
    if (m_canvas_state.has_value())
        return m_canvas_state->drawing_state().transform;
    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-closepath
void CanvasPath::close_path()
{
    web_canvas_path_close_path(&m_path);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-moveto
void CanvasPath::move_to(float x, float y)
{
    web_canvas_path_move_to(&m_path, x, y);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-lineto
void CanvasPath::line_to(float x, float y)
{
    web_canvas_path_line_to(&m_path, x, y);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-quadraticcurveto
void CanvasPath::quadratic_curve_to(float cpx, float cpy, float x, float y)
{
    web_canvas_path_quadratic_curve_to(&m_path, cpx, cpy, x, y);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-beziercurveto
void CanvasPath::bezier_curve_to(double cp1x, double cp1y, double cp2x, double cp2y, double x, double y)
{
    web_canvas_path_bezier_curve_to(&m_path, cp1x, cp1y, cp2x, cp2y, x, y);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arc
WebIDL::ExceptionOr<void> CanvasPath::arc(float x, float y, float radius, float start_angle, float end_angle, bool counter_clockwise)
{
    return to_exception_or(web_canvas_path_arc(&m_path, x, y, radius, start_angle, end_angle, counter_clockwise));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ellipse
WebIDL::ExceptionOr<void> CanvasPath::ellipse(float x, float y, float radius_x, float radius_y, float rotation, float start_angle, float end_angle, bool counter_clockwise)
{
    return to_exception_or(web_canvas_path_ellipse(&m_path, x, y, radius_x, radius_y, rotation, start_angle, end_angle, counter_clockwise));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arcto
WebIDL::ExceptionOr<void> CanvasPath::arc_to(double x1, double y1, double x2, double y2, double radius)
{
    auto transform = active_transform();
    return to_exception_or(web_canvas_path_arc_to(&m_path, &transform, x1, y1, x2, y2, radius));
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-rect
void CanvasPath::rect(double x, double y, double w, double h)
{
    web_canvas_path_rect(&m_path, x, y, w, h);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-roundrect
WebIDL::ExceptionOr<void> CanvasPath::round_rect(double x, double y, double w, double h, Variant<double, Bindings::DOMPointInit, Vector<Variant<double, Bindings::DOMPointInit>>> const& radii)
{
    auto append_radius = [](Vector<CanvasRoundRectRadius>& radius_list, Variant<double, Bindings::DOMPointInit> const& radius) {
        if (radius.has<double>()) {
            auto radius_as_double = radius.get<double>();
            radius_list.append({ radius_as_double, radius_as_double });
            return;
        }

        auto const& radius_as_dom_point = radius.get<Bindings::DOMPointInit>();
        radius_list.append({ radius_as_dom_point.x, radius_as_dom_point.y });
    };

    Vector<CanvasRoundRectRadius> radius_list;
    if (radii.has<double>()) {
        auto radius = radii.get<double>();
        radius_list.append({ radius, radius });
    } else if (radii.has<Bindings::DOMPointInit>()) {
        auto const& radius = radii.get<Bindings::DOMPointInit>();
        radius_list.append({ radius.x, radius.y });
    } else {
        for (auto const& radius : radii.get<Vector<Variant<double, Bindings::DOMPointInit>>>())
            append_radius(radius_list, radius);
    }

    return round_rect(x, y, w, h, radius_list);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-roundrect
WebIDL::ExceptionOr<void> CanvasPath::round_rect(double x, double y, double w, double h, ReadonlySpan<CanvasRoundRectRadius> radii)
{
    return to_exception_or(web_canvas_path_round_rect(&m_path, x, y, w, h, radii));
}

}
