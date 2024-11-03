/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Vector2.h>
#include <LibWeb/HTML/Canvas/CanvasPath.h>

namespace Web::HTML {

Gfx::AffineTransform CanvasPath::active_transform() const
{
    if (m_canvas_state.has_value())
        return m_canvas_state->drawing_state().transform;
    return {};
}

void CanvasPath::ensure_subpath(float x, float y)
{
    if (m_path.is_empty())
        m_path.move_to(Gfx::FloatPoint { x, y });
}

void CanvasPath::close_path()
{
    m_path.close();
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-moveto
void CanvasPath::move_to(float x, float y)
{
    // 1. If either of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y))
        return;

    // 2. Create a new subpath with the specified point as its first (and only) point.
    m_path.move_to(Gfx::FloatPoint { x, y });
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-lineto
void CanvasPath::line_to(float x, float y)
{
    // 1. If either of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y))
        return;

    if (m_path.is_empty()) {
        // 2. If the object's path has no subpaths, then ensure there is a subpath for (x, y).
        ensure_subpath(x, y);
    } else {
        // 3. Otherwise, connect the last point in the subpath to the given point (x, y) using a straight line,
        // and then add the given point (x, y) to the subpath.
        m_path.line_to(Gfx::FloatPoint { x, y });
    }
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-quadraticcurveto
void CanvasPath::quadratic_curve_to(float cpx, float cpy, float x, float y)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(cpx) || !isfinite(cpy) || !isfinite(x) || !isfinite(y))
        return;

    // 2. Ensure there is a subpath for (cpx, cpy)
    ensure_subpath(cpx, cpy);

    // 3. Connect the last point in the subpath to the given point (x, y) using a quadratic Bézier curve with control point (cpx, cpy).
    // 4. Add the given point (x, y) to the subpath.
    m_path.quadratic_bezier_curve_to(Gfx::FloatPoint { cpx, cpy }, Gfx::FloatPoint { x, y });
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-beziercurveto
void CanvasPath::bezier_curve_to(double cp1x, double cp1y, double cp2x, double cp2y, double x, double y)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(cp1x) || !isfinite(cp1y) || !isfinite(cp2x) || !isfinite(cp2y) || !isfinite(x) || !isfinite(y))
        return;

    // 2. Ensure there is a subpath for (cp1x, cp1y)
    ensure_subpath(cp1x, cp1y);

    // 3. Connect the last point in the subpath to the given point (x, y) using a cubic Bézier curve with control poits (cp1x, cp1y) and (cp2x, cp2y).
    // 4. Add the point (x, y) to the subpath.
    m_path.cubic_bezier_curve_to(
        Gfx::FloatPoint { cp1x, cp1y }, Gfx::FloatPoint { cp2x, cp2y }, Gfx::FloatPoint { x, y });
}

WebIDL::ExceptionOr<void> CanvasPath::arc(float x, float y, float radius, float start_angle, float end_angle, bool counter_clockwise)
{
    if (radius < 0)
        return WebIDL::IndexSizeError::create(m_self->realm(), MUST(String::formatted("The radius provided ({}) is negative.", radius)));
    return ellipse(x, y, radius, radius, 0, start_angle, end_angle, counter_clockwise);
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ellipse
WebIDL::ExceptionOr<void> CanvasPath::ellipse(float x, float y, float radius_x, float radius_y, float rotation, float start_angle, float end_angle, bool counter_clockwise)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y) || !isfinite(radius_x) || !isfinite(radius_y) || !isfinite(rotation) || !isfinite(start_angle) || !isfinite(end_angle))
        return {};

    // 2. If either radiusX or radiusY are negative, then throw an "IndexSizeError" DOMException.
    if (radius_x < 0)
        return WebIDL::IndexSizeError::create(m_self->realm(), MUST(String::formatted("The major-axis radius provided ({}) is negative.", radius_x)));
    if (radius_y < 0)
        return WebIDL::IndexSizeError::create(m_self->realm(), MUST(String::formatted("The minor-axis radius provided ({}) is negative.", radius_y)));

    // "If counterclockwise is false and endAngle − startAngle is greater than or equal to 2π,
    // or, if counterclockwise is true and startAngle − endAngle is greater than or equal to 2π,
    // then the arc is the whole circumference of this ellipse"
    // Also draw the full ellipse if making a non-zero whole number of turns.
    if (constexpr float tau = M_PI * 2; (!counter_clockwise && (end_angle - start_angle) >= tau)
        || (counter_clockwise && (start_angle - end_angle) >= tau)
        || (start_angle != end_angle && fmodf(start_angle - end_angle, tau) == 0)) {
        start_angle = 0;
        // FIXME: elliptical_arc_to() incorrectly handles the case where the start/end points are very close.
        // So we slightly fudge the numbers here to correct for that.
        end_angle = tau * 0.9999f;
        counter_clockwise = false;
    } else {
        start_angle = fmodf(start_angle, tau);
        end_angle = fmodf(end_angle, tau);
    }

    // Then, figure out where the ends of the arc are.
    // To do so, we can pretend that the center of this ellipse is at (0, 0),
    // and the whole coordinate system is rotated `rotation` radians around the x axis, centered on `center`.
    // The sign of the resulting relative positions is just whether our angle is on one of the left quadrants.
    float sin_rotation;
    float cos_rotation;
    AK::sincos(rotation, sin_rotation, cos_rotation);

    auto resolve_point_with_angle = [&](float angle) {
        auto tan_relative = tanf(angle);
        auto tan2 = tan_relative * tan_relative;

        auto ab = radius_x * radius_y;
        auto a2 = radius_x * radius_x;
        auto b2 = radius_y * radius_y;
        auto sqrt = sqrtf(b2 + a2 * tan2);

        auto relative_x_position = ab / sqrt;
        auto relative_y_position = ab * tan_relative / sqrt;

        // Make sure to set the correct sign
        // -1 if 0 ≤ θ < 90° or 270°< θ ≤ 360°
        //  1 if 90° < θ< 270°
        float sn = cosf(angle) >= 0 ? 1 : -1;
        relative_x_position *= sn;
        relative_y_position *= sn;

        // Now rotate it (back) around the center point by 'rotation' radians, then move it back to our actual origin.
        auto relative_rotated_x_position = relative_x_position * cos_rotation - relative_y_position * sin_rotation;
        auto relative_rotated_y_position = relative_x_position * sin_rotation + relative_y_position * cos_rotation;
        return Gfx::FloatPoint { relative_rotated_x_position + x, relative_rotated_y_position + y };
    };

    auto start_point = resolve_point_with_angle(start_angle);
    auto end_point = resolve_point_with_angle(end_angle);

    float delta_theta;
    if (counter_clockwise) {
        delta_theta = start_angle - end_angle;
    } else {
        delta_theta = end_angle - start_angle;
    }

    if (delta_theta < 0)
        delta_theta += AK::Pi<float> * 2;

    // 3. If canvasPath's path has any subpaths, then add a straight line from the last point in the subpath to the start point of the arc.
    if (!m_path.is_empty())
        m_path.line_to(start_point);
    else
        m_path.move_to(start_point);

    // 4. Add the start and end points of the arc to the subpath, and connect them with an arc.
    m_path.elliptical_arc_to(
        Gfx::FloatPoint { end_point },
        Gfx::FloatSize { radius_x, radius_y },
        rotation,
        delta_theta > AK::Pi<float>, !counter_clockwise);

    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arcto
WebIDL::ExceptionOr<void> CanvasPath::arc_to(double x1, double y1, double x2, double y2, double radius)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2) || !isfinite(radius))
        return {};

    // 2. Ensure there is a subpath for (x1, y1).
    ensure_subpath(x1, y1);

    // 3. If radius is negative, then throw an "IndexSizeError" DOMException.
    if (radius < 0)
        return WebIDL::IndexSizeError::create(m_self->realm(), MUST(String::formatted("The radius provided ({}) is negative.", radius)));

    auto transform = active_transform();

    // 4. Let the point (x0, y0) be the last point in the subpath,
    //    transformed by the inverse of the current transformation matrix
    //    (so that it is in the same coordinate system as the points passed to the method).
    // Point (x0, y0)
    auto p0 = transform.inverse().value_or(Gfx::AffineTransform()).map(m_path.last_point());
    // Point (x1, y1)
    auto p1 = Gfx::FloatPoint { x1, y1 };
    // Point (x2, y2)
    auto p2 = Gfx::FloatPoint { x2, y2 };

    // 5. If the point (x0, y0) is equal to the point (x1, y1),
    //    or if the point (x1, y1) is equal to the point (x2, y2),
    //    or if radius is zero, then add the point (x1, y1) to the subpath,
    //    and connect that point to the previous point (x0, y0) by a straight line.
    if (p0 == p1 || p1 == p2 || radius == 0) {
        m_path.line_to(p1);
        return {};
    }

    auto v1 = Gfx::FloatVector2 { p0.x() - p1.x(), p0.y() - p1.y() };
    auto v2 = Gfx::FloatVector2 { p2.x() - p1.x(), p2.y() - p1.y() };
    auto cos_theta = v1.dot(v2) / (v1.length() * v2.length());
    // 6. Otherwise, if the points (x0, y0), (x1, y1), and (x2, y2) all lie on a single straight line,
    //    then add the point (x1, y1) to the subpath,
    //    and connect that point to the previous point (x0, y0) by a straight line.
    if (-1 == cos_theta || 1 == cos_theta) {
        m_path.line_to(p1);
        return {};
    }

    // 7. Otherwise, let The Arc be the shortest arc given by circumference of the circle that has radius radius,
    // and that has one point tangent to the half-infinite line that crosses the point (x0, y0) and ends at the point (x1, y1),
    // and that has a different point tangent to the half-infinite line that ends at the point (x1, y1) and crosses the point (x2, y2).
    // The points at which this circle touches these two lines are called the start and end tangent points respectively.
    auto adjacent = radius / static_cast<double>(tan(acos(cos_theta) / 2));
    auto factor1 = adjacent / static_cast<double>(v1.length());
    auto x3 = static_cast<double>(p1.x()) + factor1 * static_cast<double>(p0.x() - p1.x());
    auto y3 = static_cast<double>(p1.y()) + factor1 * static_cast<double>(p0.y() - p1.y());
    auto start_tangent = Gfx::FloatPoint { x3, y3 };

    auto factor2 = adjacent / static_cast<double>(v2.length());
    auto x4 = static_cast<double>(p1.x()) + factor2 * static_cast<double>(p2.x() - p1.x());
    auto y4 = static_cast<double>(p1.y()) + factor2 * static_cast<double>(p2.y() - p1.y());
    auto end_tangent = Gfx::FloatPoint { x4, y4 };

    // Connect the point (x0, y0) to the start tangent point by a straight line, adding the start tangent point to the subpath.
    m_path.line_to(start_tangent);

    bool const large_arc = false; // always small since tangent points define arc endpoints and lines meet at (x1, y1)
    auto cross_product = v1.x() * v2.y() - v1.y() * v2.x();
    bool const sweep = cross_product < 0; // right-hand rule, true means clockwise

    // and then connect the start tangent point to the end tangent point by The Arc, adding the end tangent point to the subpath.
    m_path.arc_to(end_tangent, radius, large_arc, sweep);
    return {};
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-rect
void CanvasPath::rect(double x, double y, double w, double h)
{
    // 1. If any of the arguments are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h))
        return;

    // 2. Create a new subpath containing just the four points (x, y), (x+w, y), (x+w, y+h), (x, y+h), in that order, with those four points connected by straight lines.
    m_path.move_to(Gfx::FloatPoint { x, y });
    m_path.line_to(Gfx::FloatPoint { x + w, y });
    m_path.line_to(Gfx::FloatPoint { x + w, y + h });
    m_path.line_to(Gfx::FloatPoint { x, y + h });

    // 3. Mark the subpath as closed.
    m_path.close();

    // 4. Create a new subpath with the point (x, y) as the only point in the subpath.
    m_path.move_to(Gfx::FloatPoint { x, y });
}

// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-roundrect
WebIDL::ExceptionOr<void> CanvasPath::round_rect(double x, double y, double w, double h, Variant<double, Geometry::DOMPointInit, Vector<Variant<double, Geometry::DOMPointInit>>> radii)
{
    using Radius = Variant<double, Geometry::DOMPointInit>;

    // 1. If any of x, y, w, or h are infinite or NaN, then return.
    if (!isfinite(x) || !isfinite(y) || !isfinite(w) || !isfinite(h))
        return {};

    // 2. If radii is an unrestricted double or DOMPointInit, then set radii to « radii ».
    if (radii.has<double>() || radii.has<Geometry::DOMPointInit>()) {
        Vector<Radius> radii_list;
        if (radii.has<double>())
            radii_list.append(radii.get<double>());
        else
            radii_list.append(radii.get<Geometry::DOMPointInit>());
        radii = radii_list;
    }

    // 3. If radii is not a list of size one, two, three, or four, then throw a RangeError.
    if (radii.get<Vector<Radius>>().is_empty() || radii.get<Vector<Radius>>().size() > 4)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "roundRect: Can have between 1 and 4 radii"sv };

    // 4. Let normalizedRadii be an empty list.
    Vector<Geometry::DOMPointInit> normalized_radii;

    // 5. For each radius of radii:
    for (auto const& radius : radii.get<Vector<Radius>>()) {
        // 5.1. If radius is a DOMPointInit:
        if (radius.has<Geometry::DOMPointInit>()) {
            auto const& radius_as_dom_point = radius.get<Geometry::DOMPointInit>();

            // 5.1.1. If radius["x"] or radius["y"] is infinite or NaN, then return.
            if (!isfinite(radius_as_dom_point.x) || !isfinite(radius_as_dom_point.y))
                return {};

            // 5.1.2. If radius["x"] or radius["y"] is negative, then throw a RangeError.
            if (radius_as_dom_point.x < 0 || radius_as_dom_point.y < 0)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "roundRect: Radius can't be negative"sv };

            // 5.1.3. Otherwise, append radius to normalizedRadii.
            normalized_radii.append(radius_as_dom_point);
        }

        // 5.2. If radius is a unrestricted double:
        if (radius.has<double>()) {
            auto radius_as_double = radius.get<double>();

            // 5.2.1. If radius is infinite or NaN, then return.
            if (!isfinite(radius_as_double))
                return {};

            // 5.2.2. If radius is negative, then throw a RangeError.
            if (radius_as_double < 0)
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "roundRect: Radius can't be negative"sv };

            // 5.2.3. Otherwise append «[ "x" → radius, "y" → radius ]» to normalizedRadii.
            normalized_radii.append(Geometry::DOMPointInit { radius_as_double, radius_as_double });
        }
    }

    // 6. Let upperLeft, upperRight, lowerRight, and lowerLeft be null.
    Geometry::DOMPointInit upper_left {};
    Geometry::DOMPointInit upper_right {};
    Geometry::DOMPointInit lower_right {};
    Geometry::DOMPointInit lower_left {};

    // 7. If normalizedRadii's size is 4, then set upperLeft to normalizedRadii[0], set upperRight to normalizedRadii[1], set lowerRight to normalizedRadii[2], and set lowerLeft to normalizedRadii[3].
    if (normalized_radii.size() == 4) {
        upper_left = normalized_radii.at(0);
        upper_right = normalized_radii.at(1);
        lower_right = normalized_radii.at(2);
        lower_left = normalized_radii.at(3);
    }

    // 8. If normalizedRadii's size is 3, then set upperLeft to normalizedRadii[0], set upperRight and lowerLeft to normalizedRadii[1], and set lowerRight to normalizedRadii[2].
    if (normalized_radii.size() == 3) {
        upper_left = normalized_radii.at(0);
        upper_right = lower_left = normalized_radii.at(1);
        lower_right = normalized_radii.at(2);
    }

    // 9. If normalizedRadii's size is 2, then set upperLeft and lowerRight to normalizedRadii[0] and set upperRight and lowerLeft to normalizedRadii[1].
    if (normalized_radii.size() == 2) {
        upper_left = lower_right = normalized_radii.at(0);
        upper_right = lower_left = normalized_radii.at(1);
    }

    // 10. If normalizedRadii's size is 1, then set upperLeft, upperRight, lowerRight, and lowerLeft to normalizedRadii[0].
    if (normalized_radii.size() == 1)
        upper_left = upper_right = lower_right = lower_left = normalized_radii.at(0);

    // 11. Corner curves must not overlap. Scale all radii to prevent this:
    // 11.1. Let top be upperLeft["x"] + upperRight["x"].
    double top = upper_left.x + upper_right.x;

    // 11.2. Let right be upperRight["y"] + lowerRight["y"].
    double right = upper_right.y + lower_right.y;

    // 11.3. Let bottom be lowerRight["x"] + lowerLeft["x"].
    double bottom = lower_right.x + lower_left.x;

    // 11.4. Let left be upperLeft["y"] + lowerLeft["y"].
    double left = upper_left.y + lower_left.y;

    // 11.5. Let scale be the minimum value of the ratios w / top, h / right, w / bottom, h / left.
    double scale = AK::min(AK::min(w / top, h / right), AK::min(w / bottom, h / left));

    // 11.6. If scale is less than 1, then set the x and y members of upperLeft, upperRight, lowerLeft, and lowerRight to their current values multiplied by scale.
    if (scale < 1) {
        upper_left.x *= scale;
        upper_left.y *= scale;
        upper_right.x *= scale;
        upper_right.y *= scale;
        lower_left.x *= scale;
        lower_left.y *= scale;
        lower_right.x *= scale;
        lower_right.y *= scale;
    }

    // 12. Create a new subpath:
    bool large_arc = false;
    bool sweep = true;

    // 12.1. Move to the point (x + upperLeft["x"], y).
    m_path.move_to(Gfx::FloatPoint { x + upper_left.x, y });

    // 12.2. Draw a straight line to the point (x + w − upperRight["x"], y).
    m_path.line_to(Gfx::FloatPoint { x + w - upper_right.x, y });

    // 12.3. Draw an arc to the point (x + w, y + upperRight["y"]).
    m_path.elliptical_arc_to(Gfx::FloatPoint { x + w, y + upper_right.y }, { upper_right.x, upper_right.y }, 0, large_arc, sweep);

    // 12.4. Draw a straight line to the point (x + w, y + h − lowerRight["y"]).
    m_path.line_to(Gfx::FloatPoint { x + w, y + h - lower_right.y });

    // 12.5. Draw an arc to the point (x + w − lowerRight["x"], y + h).
    m_path.elliptical_arc_to(Gfx::FloatPoint { x + w - lower_right.x, y + h }, { lower_right.x, lower_right.y }, 0, large_arc, sweep);

    // 12.6. Draw a straight line to the point (x + lowerLeft["x"], y + h).
    m_path.line_to(Gfx::FloatPoint { x + lower_left.x, y + h });

    // 12.7. Draw an arc to the point (x, y + h − lowerLeft["y"]).
    m_path.elliptical_arc_to(Gfx::FloatPoint { x, y + h - lower_left.y }, { lower_left.x, lower_left.y }, 0, large_arc, sweep);

    // 12.8. Draw a straight line to the point (x, y + upperLeft["y"]).
    m_path.line_to(Gfx::FloatPoint { x, y + upper_left.y });

    // 12.9. Draw an arc to the point (x + upperLeft["x"], y).
    m_path.elliptical_arc_to(Gfx::FloatPoint { x + upper_left.x, y }, { upper_left.x, upper_left.y }, 0, large_arc, sweep);

    // 13. Mark the subpath as closed.
    m_path.close();

    // 14. Create a new subpath with the point (x, y) as the only point in the subpath.
    m_path.move_to(Gfx::FloatPoint { x, y });
    return {};
}

}
