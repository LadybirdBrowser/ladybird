/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <AK/TypeCasts.h>
#include <LibGfx/BoundingBox.h>
#include <LibGfx/DeprecatedPainter.h>
#include <LibGfx/DeprecatedPath.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/TextLayout.h>

namespace Gfx {

void DeprecatedPath::approximate_elliptical_arc_with_cubic_beziers(FloatPoint center, FloatSize radii, float x_axis_rotation, float theta, float theta_delta)
{
    float sin_x_rotation;
    float cos_x_rotation;
    AK::sincos(x_axis_rotation, sin_x_rotation, cos_x_rotation);
    auto arc_point_and_derivative = [&](float t, FloatPoint& point, FloatPoint& derivative) {
        float sin_angle;
        float cos_angle;
        AK::sincos(t, sin_angle, cos_angle);
        point = FloatPoint {
            center.x()
                + radii.width() * cos_x_rotation * cos_angle
                - radii.height() * sin_x_rotation * sin_angle,
            center.y()
                + radii.width() * sin_x_rotation * cos_angle
                + radii.height() * cos_x_rotation * sin_angle,
        };
        derivative = FloatPoint {
            -radii.width() * cos_x_rotation * sin_angle
                - radii.height() * sin_x_rotation * cos_angle,
            -radii.width() * sin_x_rotation * sin_angle
                + radii.height() * cos_x_rotation * cos_angle,
        };
    };
    auto approximate_arc_between = [&](float start_angle, float end_angle) {
        auto t = AK::tan((end_angle - start_angle) / 2);
        auto alpha = AK::sin(end_angle - start_angle) * ((AK::sqrt(4 + 3 * t * t) - 1) / 3);
        FloatPoint p1, d1;
        FloatPoint p2, d2;
        arc_point_and_derivative(start_angle, p1, d1);
        arc_point_and_derivative(end_angle, p2, d2);
        auto q1 = p1 + d1.scaled(alpha, alpha);
        auto q2 = p2 - d2.scaled(alpha, alpha);
        cubic_bezier_curve_to(q1, q2, p2);
    };
    // FIXME: Come up with a more mathematically sound step size (using some error calculation).
    auto step = theta_delta;
    int step_count = 1;
    while (fabs(step) > AK::Pi<float> / 4) {
        step /= 2;
        step_count *= 2;
    }
    float prev = theta;
    float t = prev + step;
    for (int i = 0; i < step_count; i++, prev = t, t += step)
        approximate_arc_between(prev, t);
}

void DeprecatedPath::elliptical_arc_to(FloatPoint point, FloatSize radii, float x_axis_rotation, bool large_arc, bool sweep)
{
    auto next_point = point;

    double rx = radii.width();
    double ry = radii.height();

    double x_axis_rotation_s;
    double x_axis_rotation_c;
    AK::sincos(static_cast<double>(x_axis_rotation), x_axis_rotation_s, x_axis_rotation_c);
    FloatPoint last_point = this->last_point();

    // Step 1 of out-of-range radii correction
    if (rx == 0.0 || ry == 0.0) {
        append_segment<DeprecatedPathSegment::LineTo>(next_point);
        return;
    }

    // Step 2 of out-of-range radii correction
    if (rx < 0)
        rx *= -1.0;
    if (ry < 0)
        ry *= -1.0;

    // POSSIBLY HACK: Handle the case where both points are the same.
    auto same_endpoints = next_point == last_point;
    if (same_endpoints) {
        if (!large_arc) {
            // Nothing is going to be drawn anyway.
            return;
        }

        // Move the endpoint by a small amount to avoid division by zero.
        next_point.translate_by(0.01f, 0.01f);
    }

    // Find (cx, cy), theta_1, theta_delta
    // Step 1: Compute (x1', y1')
    auto x_avg = static_cast<double>(last_point.x() - next_point.x()) / 2.0;
    auto y_avg = static_cast<double>(last_point.y() - next_point.y()) / 2.0;
    auto x1p = x_axis_rotation_c * x_avg + x_axis_rotation_s * y_avg;
    auto y1p = -x_axis_rotation_s * x_avg + x_axis_rotation_c * y_avg;

    // Step 2: Compute (cx', cy')
    double x1p_sq = x1p * x1p;
    double y1p_sq = y1p * y1p;
    double rx_sq = rx * rx;
    double ry_sq = ry * ry;

    // Step 3 of out-of-range radii correction
    double lambda = x1p_sq / rx_sq + y1p_sq / ry_sq;
    double multiplier;

    if (lambda > 1.0) {
        auto lambda_sqrt = AK::sqrt(lambda);
        rx *= lambda_sqrt;
        ry *= lambda_sqrt;
        multiplier = 0.0;
    } else {
        double numerator = rx_sq * ry_sq - rx_sq * y1p_sq - ry_sq * x1p_sq;
        double denominator = rx_sq * y1p_sq + ry_sq * x1p_sq;
        multiplier = AK::sqrt(AK::max(0., numerator) / denominator);
    }

    if (large_arc == sweep)
        multiplier *= -1.0;

    double cxp = multiplier * rx * y1p / ry;
    double cyp = multiplier * -ry * x1p / rx;

    // Step 3: Compute (cx, cy) from (cx', cy')
    x_avg = (last_point.x() + next_point.x()) / 2.0f;
    y_avg = (last_point.y() + next_point.y()) / 2.0f;
    double cx = x_axis_rotation_c * cxp - x_axis_rotation_s * cyp + x_avg;
    double cy = x_axis_rotation_s * cxp + x_axis_rotation_c * cyp + y_avg;

    double theta_1 = AK::atan2((y1p - cyp) / ry, (x1p - cxp) / rx);
    double theta_2 = AK::atan2((-y1p - cyp) / ry, (-x1p - cxp) / rx);

    auto theta_delta = theta_2 - theta_1;

    if (!sweep && theta_delta > 0.0) {
        theta_delta -= 2 * AK::Pi<double>;
    } else if (sweep && theta_delta < 0) {
        theta_delta += 2 * AK::Pi<double>;
    }

    approximate_elliptical_arc_with_cubic_beziers(
        { cx, cy },
        { rx, ry },
        x_axis_rotation,
        theta_1,
        theta_delta);
}

void DeprecatedPath::close()
{
    // If there's no `moveto` starting this subpath assume the start is (0, 0).
    FloatPoint first_point_in_subpath = { 0, 0 };
    for (auto it = end(); it-- != begin();) {
        auto segment = *it;
        if (segment.command() == DeprecatedPathSegment::MoveTo) {
            first_point_in_subpath = segment.point();
            break;
        }
    }
    if (first_point_in_subpath != last_point())
        line_to(first_point_in_subpath);
}

void DeprecatedPath::close_all_subpaths()
{
    auto it = begin();
    // Note: Get the end outside the loop as closing subpaths will move the end.
    auto end = this->end();
    while (it < end) {
        // If there's no `moveto` starting this subpath assume the start is (0, 0).
        FloatPoint first_point_in_subpath = { 0, 0 };
        auto segment = *it;
        if (segment.command() == DeprecatedPathSegment::MoveTo) {
            first_point_in_subpath = segment.point();
            ++it;
        }
        // Find the end of the current subpath.
        FloatPoint cursor = first_point_in_subpath;
        while (it < end) {
            auto segment = *it;
            if (segment.command() == DeprecatedPathSegment::MoveTo)
                break;
            cursor = segment.point();
            ++it;
        }
        // Close the subpath.
        if (first_point_in_subpath != cursor) {
            move_to(cursor);
            line_to(first_point_in_subpath);
        }
    }
}

ByteString DeprecatedPath::to_byte_string() const
{
    // Dumps this path as an SVG compatible string.
    StringBuilder builder;
    if (is_empty() || m_commands.first() != DeprecatedPathSegment::MoveTo)
        builder.append("M 0,0"sv);
    for (auto segment : *this) {
        if (!builder.is_empty())
            builder.append(' ');
        switch (segment.command()) {
        case DeprecatedPathSegment::MoveTo:
            builder.append('M');
            break;
        case DeprecatedPathSegment::LineTo:
            builder.append('L');
            break;
        case DeprecatedPathSegment::QuadraticBezierCurveTo:
            builder.append('Q');
            break;
        case DeprecatedPathSegment::CubicBezierCurveTo:
            builder.append('C');
            break;
        }
        for (auto point : segment.points())
            builder.appendff(" {},{}", point.x(), point.y());
    }
    return builder.to_byte_string();
}

void DeprecatedPath::segmentize_path()
{
    Vector<FloatLine> segments;
    FloatBoundingBox bounding_box;

    auto add_line = [&](auto const& p0, auto const& p1) {
        segments.append({ p0, p1 });
        bounding_box.add_point(p1);
    };

    FloatPoint cursor { 0, 0 };
    for (auto segment : *this) {
        switch (segment.command()) {
        case DeprecatedPathSegment::MoveTo:
            bounding_box.add_point(segment.point());
            break;
        case DeprecatedPathSegment::LineTo: {
            add_line(cursor, segment.point());
            break;
        }
        case DeprecatedPathSegment::QuadraticBezierCurveTo: {
            DeprecatedPainter::for_each_line_segment_on_bezier_curve(segment.through(), cursor, segment.point(), [&](FloatPoint p0, FloatPoint p1) {
                add_line(p0, p1);
            });
            break;
        }
        case DeprecatedPathSegment::CubicBezierCurveTo: {
            DeprecatedPainter::for_each_line_segment_on_cubic_bezier_curve(segment.through_0(), segment.through_1(), cursor, segment.point(), [&](FloatPoint p0, FloatPoint p1) {
                add_line(p0, p1);
            });
            break;
        }
        }
        cursor = segment.point();
    }

    m_split_lines = SplitLines { move(segments), bounding_box };
}

DeprecatedPath DeprecatedPath::copy_transformed(Gfx::AffineTransform const& transform) const
{
    DeprecatedPath result;
    result.m_commands = m_commands;
    result.m_points.ensure_capacity(m_points.size());
    for (auto point : m_points)
        result.m_points.unchecked_append(transform.map(point));
    return result;
}

template<typename T>
struct RoundTrip {
    RoundTrip(ReadonlySpan<T> span)
        : m_span(span)
    {
    }

    size_t size() const
    {
        return m_span.size() * 2 - 1;
    }

    T const& operator[](size_t index) const
    {
        // Follow the path:
        if (index < m_span.size())
            return m_span[index];
        // Then in reverse:
        if (index < size())
            return m_span[size() - index - 1];
        // Then wrap around again:
        return m_span[index - size() + 1];
    }

private:
    ReadonlySpan<T> m_span;
};

DeprecatedPath DeprecatedPath::stroke_to_fill(float thickness) const
{
    // Note: This convolves a polygon with the path using the algorithm described
    // in https://keithp.com/~keithp/talks/cairo2003.pdf (3.1 Stroking Splines via Convolution)

    VERIFY(thickness > 0);

    auto lines = split_lines();
    if (lines.is_empty())
        return DeprecatedPath {};

    // Paths can be disconnected, which a pain to deal with, so split it up.
    Vector<Vector<FloatPoint>> segments;
    segments.append({ lines.first().a() });
    for (auto& line : lines) {
        if (line.a() == segments.last().last()) {
            segments.last().append(line.b());
        } else {
            segments.append({ line.a(), line.b() });
        }
    }

    constexpr auto flatness = 0.15f;
    auto pen_vertex_count = 4;
    if (thickness > flatness) {
        pen_vertex_count = max(
            static_cast<int>(ceilf(AK::Pi<float>
                / acosf(1 - (2 * flatness) / thickness))),
            pen_vertex_count);
    }

    if (pen_vertex_count % 2 == 1)
        pen_vertex_count += 1;

    Vector<FloatPoint, 128> pen_vertices;
    pen_vertices.ensure_capacity(pen_vertex_count);

    // Generate vertices for the pen (going counterclockwise). The pen does not necessarily need
    // to be a circle (or an approximation of one), but other shapes are untested.
    float theta = 0;
    float theta_delta = (AK::Pi<float> * 2) / pen_vertex_count;
    for (int i = 0; i < pen_vertex_count; i++) {
        float sin_theta;
        float cos_theta;
        AK::sincos(theta, sin_theta, cos_theta);
        pen_vertices.unchecked_append({ cos_theta * thickness / 2, sin_theta * thickness / 2 });
        theta -= theta_delta;
    }

    auto wrapping_index = [](auto& vertices, auto index) {
        return vertices[(index + vertices.size()) % vertices.size()];
    };

    auto angle_between = [](auto p1, auto p2) {
        auto delta = p2 - p1;
        return atan2f(delta.y(), delta.x());
    };

    struct ActiveRange {
        float start;
        float end;

        bool in_range(float angle) const
        {
            // Note: Since active ranges go counterclockwise start > end unless we wrap around at 180 degrees
            return ((angle <= start && angle >= end)
                || (start < end && angle <= start)
                || (start < end && angle >= end));
        }
    };

    Vector<ActiveRange, 128> active_ranges;
    active_ranges.ensure_capacity(pen_vertices.size());
    for (auto i = 0; i < pen_vertex_count; i++) {
        active_ranges.unchecked_append({ angle_between(wrapping_index(pen_vertices, i - 1), pen_vertices[i]),
            angle_between(pen_vertices[i], wrapping_index(pen_vertices, i + 1)) });
    }

    auto clockwise = [](float current_angle, float target_angle) {
        if (target_angle < 0)
            target_angle += AK::Pi<float> * 2;
        if (current_angle < 0)
            current_angle += AK::Pi<float> * 2;
        if (target_angle < current_angle)
            target_angle += AK::Pi<float> * 2;
        return (target_angle - current_angle) <= AK::Pi<float>;
    };

    DeprecatedPath convolution;
    for (auto& segment : segments) {
        RoundTrip<FloatPoint> shape { segment };

        bool first = true;
        auto add_vertex = [&](auto v) {
            if (first) {
                convolution.move_to(v);
                first = false;
            } else {
                convolution.line_to(v);
            }
        };

        auto shape_idx = 0u;

        auto slope = [&] {
            return angle_between(shape[shape_idx], shape[shape_idx + 1]);
        };

        auto start_slope = slope();
        // Note: At least one range must be active.
        auto active = *active_ranges.find_first_index_if([&](auto& range) {
            return range.in_range(start_slope);
        });

        while (shape_idx < shape.size()) {
            add_vertex(shape[shape_idx] + pen_vertices[active]);
            auto slope_now = slope();
            auto range = active_ranges[active];
            if (range.in_range(slope_now)) {
                shape_idx++;
            } else {
                if (clockwise(slope_now, range.end)) {
                    if (active == static_cast<size_t>(pen_vertex_count - 1))
                        active = 0;
                    else
                        active++;
                } else {
                    if (active == 0)
                        active = pen_vertex_count - 1;
                    else
                        active--;
                }
            }
        }
    }

    return convolution;
}

}
