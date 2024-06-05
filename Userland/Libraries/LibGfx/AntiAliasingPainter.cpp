/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2022, Ben Maxwell <macdue@dueutil.tech>
 * Copyright (c) 2022, Torsten Engelmann <engelTorsten@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

#include <AK/Function.h>
#include <AK/NumericLimits.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/Line.h>
#include <LibGfx/Painter.h>

namespace Gfx {

void AntiAliasingPainter::draw_anti_aliased_line(FloatPoint actual_from, FloatPoint actual_to, Color color, float thickness, LineStyle style, Color, LineLengthMode line_length_mode)
{
    // FIXME: Implement this :P
    VERIFY(style == LineStyle::Solid);

    if (color.alpha() == 0)
        return;

    // FIMXE:
    // This is not a proper line drawing algorithm.
    // It's hack-ish AA rotated rectangle painting.
    // There's probably more optimal ways to achieve this
    // (though this still runs faster than the previous AA-line code)
    //
    // If you, reading this comment, know a better way that:
    //  1. Does not overpaint (i.e. painting a line with transparency looks correct)
    //  2. Has square end points (i.e. the line is a rectangle)
    //  3. Has good anti-aliasing
    //  4. Is less hacky than this
    //
    // Please delete this code and implement it!

    int int_thickness = AK::ceil(thickness);
    auto mapped_from = m_transform.map(actual_from);
    auto mapped_to = m_transform.map(actual_to);
    auto distance = mapped_to.distance_from(mapped_from);
    auto length = distance + (line_length_mode == LineLengthMode::PointToPoint);

    // Axis-aligned lines:
    if (mapped_from.y() == mapped_to.y()) {
        auto start_point = (mapped_from.x() < mapped_to.x() ? mapped_from : mapped_to).translated(0, -int_thickness / 2);
        return fill_rect(Gfx::FloatRect(start_point, { length, thickness }), color);
    }
    if (mapped_from.x() == mapped_to.x()) {
        auto start_point = (mapped_from.y() < mapped_to.y() ? mapped_from : mapped_to).translated(-int_thickness / 2, 0);
        return fill_rect(Gfx::FloatRect(start_point, { thickness, length }), color);
    }

    // The painting only works for the positive XY quadrant (because that is easier).
    // So flip things around until we're there:
    bool flip_x = false;
    bool flip_y = false;
    if (mapped_to.x() < mapped_from.x() && mapped_to.y() < mapped_from.y())
        swap(mapped_to, mapped_from);
    if ((flip_x = mapped_to.x() < mapped_from.x()))
        mapped_to.set_x(2 * mapped_from.x() - mapped_to.x());
    if ((flip_y = mapped_to.y() < mapped_from.y()))
        mapped_to.set_y(2 * mapped_from.y() - mapped_to.y());

    auto delta = mapped_to - mapped_from;
    auto line_angle_radians = AK::atan2(delta.y(), delta.x()) - 0.5f * AK::Pi<float>;
    float sin_inverse_angle;
    float cos_inverse_angle;
    AK::sincos(-line_angle_radians, sin_inverse_angle, cos_inverse_angle);

    auto inverse_rotate_point = [=](FloatPoint point) {
        return Gfx::FloatPoint(
            point.x() * cos_inverse_angle - point.y() * sin_inverse_angle,
            point.y() * cos_inverse_angle + point.x() * sin_inverse_angle);
    };

    Gfx::FloatRect line_rect({ -(thickness * 255) / 2.0f, 0 }, Gfx::FloatSize(thickness * 255, length * 255));

    auto gradient = delta.y() / delta.x();
    // Work out how long we need to scan along the X-axis to reach the other side of the line.
    // E.g. for a vertical line this would be `thickness', in general it is this:
    int scan_line_length = AK::ceil(AK::sqrt((gradient * gradient + 1) * thickness * thickness) / gradient);

    auto x_gradient = 1 / gradient;
    int x_step = floorf(x_gradient);

    float x_error = 0;
    float x_error_per_y = x_gradient - x_step;

    auto y_offset = int_thickness + 1;
    auto x_offset = int(x_gradient * y_offset);
    int const line_start_x = mapped_from.x();
    int const line_start_y = mapped_from.y();
    int const line_end_x = mapped_to.x();
    int const line_end_y = mapped_to.y();

    auto set_pixel = [=, this](int x, int y, Gfx::Color color) {
        // FIXME: The lines seem slightly off (<= 1px) when flipped.
        if (flip_x)
            x = 2 * line_start_x - x;
        if (flip_y)
            y = 2 * line_start_y - y;
        m_underlying_painter.set_pixel(x, y, color, true);
    };

    // Scan a bit extra to avoid issues from the x_error:
    int const overscan = max(x_step, 1) * 2 + 1;
    int x = line_start_x - x_offset;
    int const center_offset = (scan_line_length + 1) / 2;
    for (int y = line_start_y - y_offset; y < line_end_y + y_offset; y += 1) {
        for (int i = -overscan; i < scan_line_length + overscan; i++) {
            int scan_x_pos = x + i - center_offset;
            // Avoid scanning over pixels definitely outside the line:
            int dx = (line_start_x - int_thickness) - (scan_x_pos + 1);
            if (dx > 0) {
                i += dx;
                continue;
            }
            if (line_end_x + int_thickness <= scan_x_pos - 1)
                break;
            auto sample = inverse_rotate_point(Gfx::FloatPoint(scan_x_pos - line_start_x, y - line_start_y));
            Gfx::FloatRect sample_px(sample * 255, Gfx::FloatSize(255, 255));
            sample_px.intersect(line_rect);
            auto alpha = (sample_px.width() * sample_px.height()) / 255.0f;
            alpha = (alpha * color.alpha()) / 255;
            set_pixel(scan_x_pos, y, color.with_alpha(alpha));
        }
        x += x_step;
        x_error += x_error_per_y;
        if (x_error > 1.0f) {
            x_error -= 1.0f;
            x += 1;
        }
    }
}

void AntiAliasingPainter::draw_dotted_line(IntPoint point1, IntPoint point2, Color color, int thickness)
{
    // AA circles don't really work below a radius of 2px.
    if (thickness < 4)
        return m_underlying_painter.draw_line(point1, point2, color, thickness, LineStyle::Dotted);

    auto draw_spaced_dots = [&](int start, int end, auto to_point) {
        int step = thickness * 2;
        if (start > end)
            swap(start, end);
        int delta = end - start;
        int dots = delta / step;
        if (dots == 0)
            return;
        int fudge_per_dot = 0;
        int extra_fudge = 0;
        if (dots > 3) {
            // Fudge the numbers so the last dot is drawn at the `end' point (otherwise you can get lines cuts short).
            // You need at least a handful of dots to do this.
            int fudge = delta % step;
            fudge_per_dot = fudge / dots;
            extra_fudge = fudge % dots;
        }
        for (int dot = start; dot <= end; dot += (step + fudge_per_dot + (extra_fudge > 0))) {
            fill_circle(to_point(dot), thickness / 2, color);
            --extra_fudge;
        }
    };

    if (point1.y() == point2.y()) {
        draw_spaced_dots(point1.x(), point2.x(), [&](int dot_x) {
            return IntPoint { dot_x, point1.y() };
        });
    } else if (point1.x() == point2.x()) {
        draw_spaced_dots(point1.y(), point2.y(), [&](int dot_y) {
            return IntPoint { point1.x(), dot_y };
        });
    } else {
        TODO();
    }
}

void AntiAliasingPainter::draw_line(IntPoint actual_from, IntPoint actual_to, Color color, float thickness, LineStyle style, Color alternate_color, LineLengthMode line_length_mode)
{
    draw_line(actual_from.to_type<float>(), actual_to.to_type<float>(), color, thickness, style, alternate_color, line_length_mode);
}

void AntiAliasingPainter::draw_line(FloatPoint actual_from, FloatPoint actual_to, Color color, float thickness, LineStyle style, Color alternate_color, LineLengthMode line_length_mode)
{
    if (style == LineStyle::Dotted)
        return draw_dotted_line(actual_from.to_rounded<int>(), actual_to.to_rounded<int>(), color, static_cast<int>(round(thickness)));
    draw_anti_aliased_line(actual_from, actual_to, color, thickness, style, alternate_color, line_length_mode);
}

void AntiAliasingPainter::stroke_path(Path const& path, Color color, float thickness)
{
    if (thickness <= 0)
        return;
    // FIXME: Cache this? Probably at a higher level such as in LibWeb?
    fill_path(path.stroke_to_fill(thickness), color);
}

void AntiAliasingPainter::stroke_path(Path const& path, Gfx::PaintStyle const& paint_style, float thickness, float opacity)
{
    if (thickness <= 0)
        return;
    // FIXME: Cache this? Probably at a higher level such as in LibWeb?
    fill_path(path.stroke_to_fill(thickness), paint_style, opacity);
}

void AntiAliasingPainter::fill_rect(FloatRect const& float_rect, Color color)
{
    // Draw the integer part of the rectangle:
    float right_x = float_rect.x() + float_rect.width();
    float bottom_y = float_rect.y() + float_rect.height();
    int x1 = ceilf(float_rect.x());
    int y1 = ceilf(float_rect.y());
    int x2 = floorf(right_x);
    int y2 = floorf(bottom_y);
    auto solid_rect = Gfx::IntRect::from_two_points({ x1, y1 }, { x2, y2 });
    m_underlying_painter.fill_rect(solid_rect, color);

    if (float_rect == solid_rect)
        return;

    // Draw the rest:
    float left_subpixel = x1 - float_rect.x();
    float top_subpixel = y1 - float_rect.y();
    float right_subpixel = right_x - x2;
    float bottom_subpixel = bottom_y - y2;
    float top_left_subpixel = top_subpixel * left_subpixel;
    float top_right_subpixel = top_subpixel * right_subpixel;
    float bottom_left_subpixel = bottom_subpixel * left_subpixel;
    float bottom_right_subpixel = bottom_subpixel * right_subpixel;

    auto subpixel = [&](float alpha) {
        return color.with_alpha(color.alpha() * alpha);
    };

    auto set_pixel = [&](int x, int y, float alpha) {
        m_underlying_painter.set_pixel(x, y, subpixel(alpha), true);
    };

    auto line_to_rect = [&](int x1, int y1, int x2, int y2) {
        return IntRect::from_two_points({ x1, y1 }, { x2 + 1, y2 + 1 });
    };

    set_pixel(x1 - 1, y1 - 1, top_left_subpixel);
    set_pixel(x2, y1 - 1, top_right_subpixel);
    set_pixel(x2, y2, bottom_right_subpixel);
    set_pixel(x1 - 1, y2, bottom_left_subpixel);
    m_underlying_painter.fill_rect(line_to_rect(x1, y1 - 1, x2 - 1, y1 - 1), subpixel(top_subpixel));
    m_underlying_painter.fill_rect(line_to_rect(x1, y2, x2 - 1, y2), subpixel(bottom_subpixel));
    m_underlying_painter.fill_rect(line_to_rect(x1 - 1, y1, x1 - 1, y2 - 1), subpixel(left_subpixel));
    m_underlying_painter.fill_rect(line_to_rect(x2, y1, x2, y2 - 1), subpixel(right_subpixel));
}

void AntiAliasingPainter::draw_ellipse(IntRect const& a_rect, Color color, int thickness)
{
    // FIXME: Come up with an allocation-free version of this!
    // Using draw_line() for segments of an ellipse was attempted but gave really poor results :^(
    // There probably is a way to adjust the fill of draw_ellipse_part() to do this, but getting it rendering correctly is tricky.
    // The outline of the steps required to paint it efficiently is:
    //     - Paint the outer ellipse without the fill (from the fill() lambda in draw_ellipse_part())
    //     - Paint the inner ellipse, but in the set_pixel() invert the alpha values
    //     - Somehow fill in the gap between the two ellipses (the tricky part to get right)
    //          - Have to avoid overlapping pixels and accidentally painting over some of the edge pixels

    auto color_no_alpha = color;
    color_no_alpha.set_alpha(255);
    auto outline_ellipse_bitmap = ({
        auto bitmap = Bitmap::create(BitmapFormat::BGRA8888, a_rect.size());
        if (bitmap.is_error())
            return warnln("Failed to allocate temporary bitmap for antialiased outline ellipse!");
        bitmap.release_value();
    });

    auto outer_rect = a_rect;
    outer_rect.set_location({ 0, 0 });
    auto inner_rect = outer_rect.shrunken(thickness * 2, thickness * 2);
    Painter painter { outline_ellipse_bitmap };
    AntiAliasingPainter aa_painter { painter };
    aa_painter.fill_ellipse(outer_rect, color_no_alpha);
    aa_painter.fill_ellipse(inner_rect, color_no_alpha, BlendMode::AlphaSubtract);
    m_underlying_painter.blit(a_rect.location(), outline_ellipse_bitmap, outline_ellipse_bitmap->rect(), color.alpha() / 255.);
}

void AntiAliasingPainter::fill_circle(IntPoint center, int radius, Color color, BlendMode blend_mode)
{
    if (radius <= 0)
        return;
    draw_ellipse_part(center, radius, radius, color, false, {}, blend_mode);
}

void AntiAliasingPainter::fill_ellipse(IntRect const& a_rect, Color color, BlendMode blend_mode)
{
    auto center = a_rect.center();
    auto radius_a = a_rect.width() / 2;
    auto radius_b = a_rect.height() / 2;
    if (radius_a <= 0 || radius_b <= 0)
        return;
    if (radius_a == radius_b)
        return fill_circle(center, radius_a, color, blend_mode);
    auto x_paint_range = draw_ellipse_part(center, radius_a, radius_b, color, false, {}, blend_mode);
    // FIXME: This paints some extra fill pixels that are clipped
    draw_ellipse_part(center, radius_b, radius_a, color, true, x_paint_range, blend_mode);
}

FLATTEN AntiAliasingPainter::Range AntiAliasingPainter::draw_ellipse_part(
    IntPoint center, int radius_a, int radius_b, Color color, bool flip_x_and_y, Optional<Range> x_clip, BlendMode blend_mode)
{
    /*
    Algorithm from: https://cs.uwaterloo.ca/research/tr/1984/CS-84-38.pdf

    This method can draw a whole circle with a whole circle in one call using
    8-way symmetry, or an ellipse in two calls using 4-way symmetry.
    */

    // If this is a ellipse everything can be drawn in one pass with 8 way symmetry
    bool const is_circle = radius_a == radius_b;

    // These happen to be the same here, but are treated separately in the paper:
    // intensity is the fill alpha
    int const intensity = 255;
    // 0 to subpixel_resolution is the range of alpha values for the circle edges
    int const subpixel_resolution = intensity;

    // Current pixel address
    int i = 0;
    int q = radius_b;

    // 1st and 2nd order differences of y
    int delta_y = 0;
    int delta2_y = 0;

    int const a_squared = radius_a * radius_a;
    int const b_squared = radius_b * radius_b;

    // Exact and predicted values of f(i) -- the ellipse equation scaled by subpixel_resolution
    int y = subpixel_resolution * radius_b;
    int y_hat = 0;

    // The value of f(i)*f(i)
    int f_squared = y * y;

    // 1st and 2nd order differences of f(i)*f(i)
    int delta_f_squared = (static_cast<int64_t>(b_squared) * subpixel_resolution * subpixel_resolution) / a_squared;
    int delta2_f_squared = -delta_f_squared - delta_f_squared;

    // edge_intersection_area/subpixel_resolution = percentage of pixel intersected by circle
    // (aka the alpha for the pixel)
    int edge_intersection_area = 0;
    int old_area = edge_intersection_area;

    auto predict = [&] {
        delta_y += delta2_y;
        // y_hat is the predicted value of f(i)
        y_hat = y + delta_y;
    };

    auto minimize = [&] {
        // Initialize the minimization
        delta_f_squared += delta2_f_squared;
        f_squared += delta_f_squared;

        int min_squared_error = y_hat * y_hat - f_squared;
        int prediction_overshot = 1;
        y = y_hat;

        // Force error negative
        if (min_squared_error > 0) {
            min_squared_error = -min_squared_error;
            prediction_overshot = -1;
        }

        // Minimize
        int previous_error = min_squared_error;
        while (min_squared_error < 0) {
            y += prediction_overshot;
            previous_error = min_squared_error;
            min_squared_error += y + y - prediction_overshot;
        }

        if (min_squared_error + previous_error > 0)
            y -= prediction_overshot;
    };

    auto correct = [&] {
        int error = y - y_hat;

        delta2_y += error;
        delta_y += error;
    };

    int min_paint_x = NumericLimits<int>::max();
    int max_paint_x = NumericLimits<int>::min();
    auto pixel = [&](int x, int y, int alpha) {
        if (alpha <= 0 || alpha > 255)
            return;
        if (flip_x_and_y)
            swap(x, y);
        if (x_clip.has_value() && x_clip->contains_inclusive(x))
            return;
        min_paint_x = min(x, min_paint_x);
        max_paint_x = max(x, max_paint_x);
        alpha = (alpha * color.alpha()) / 255;
        if (blend_mode == BlendMode::AlphaSubtract)
            alpha = ~alpha;
        auto pixel_color = color;
        pixel_color.set_alpha(alpha);
        m_underlying_painter.set_pixel(center + IntPoint { x, y }, pixel_color, blend_mode == BlendMode::Normal);
    };

    auto fill = [&](int x, int ymax, int ymin, int alpha) {
        while (ymin <= ymax) {
            pixel(x, ymin, alpha);
            ymin += 1;
        }
    };

    auto symmetric_pixel = [&](int x, int y, int alpha) {
        pixel(x, y, alpha);
        pixel(x, -y - 1, alpha);
        pixel(-x - 1, -y - 1, alpha);
        pixel(-x - 1, y, alpha);
        if (is_circle) {
            pixel(y, x, alpha);
            pixel(y, -x - 1, alpha);
            pixel(-y - 1, -x - 1, alpha);
            pixel(-y - 1, x, alpha);
        }
    };

    // These are calculated incrementally (as it is possibly a tiny bit faster)
    int ib_squared = 0;
    int qa_squared = q * a_squared;

    auto in_symmetric_region = [&] {
        // Main fix two stop cond here
        return is_circle ? i < q : ib_squared < qa_squared;
    };

    // Draws a 8 octants for a circle or 4 quadrants for a (partial) ellipse
    while (in_symmetric_region()) {
        predict();
        minimize();
        correct();
        old_area = edge_intersection_area;
        edge_intersection_area += delta_y;
        if (edge_intersection_area >= 0) {
            // Single pixel on perimeter
            symmetric_pixel(i, q, (edge_intersection_area + old_area) / 2);
            fill(i, q - 1, -q, intensity);
            fill(-i - 1, q - 1, -q, intensity);
        } else {
            // Two pixels on perimeter
            edge_intersection_area += subpixel_resolution;
            symmetric_pixel(i, q, old_area / 2);
            q -= 1;
            qa_squared -= a_squared;
            fill(i, q - 1, -q, intensity);
            fill(-i - 1, q - 1, -q, intensity);
            if (!is_circle || in_symmetric_region()) {
                symmetric_pixel(i, q, (edge_intersection_area + subpixel_resolution) / 2);
                if (is_circle) {
                    fill(q, i - 1, -i, intensity);
                    fill(-q - 1, i - 1, -i, intensity);
                }
            } else {
                edge_intersection_area += subpixel_resolution;
            }
        }
        i += 1;
        ib_squared += b_squared;
    }

    if (is_circle) {
        int alpha = edge_intersection_area / 2;
        pixel(q, q, alpha);
        pixel(-q - 1, q, alpha);
        pixel(-q - 1, -q - 1, alpha);
        pixel(q, -q - 1, alpha);
    }

    return Range { min_paint_x, max_paint_x };
}

void AntiAliasingPainter::fill_rect_with_rounded_corners(IntRect const& a_rect, Color color, int radius)
{
    fill_rect_with_rounded_corners(a_rect, color, radius, radius, radius, radius);
}

void AntiAliasingPainter::fill_rect_with_rounded_corners(IntRect const& a_rect, Color color, int top_left_radius, int top_right_radius, int bottom_right_radius, int bottom_left_radius)
{
    fill_rect_with_rounded_corners(a_rect, color,
        { top_left_radius, top_left_radius },
        { top_right_radius, top_right_radius },
        { bottom_right_radius, bottom_right_radius },
        { bottom_left_radius, bottom_left_radius });
}

void AntiAliasingPainter::fill_rect_with_rounded_corners(IntRect const& a_rect, Color color, CornerRadius top_left, CornerRadius top_right, CornerRadius bottom_right, CornerRadius bottom_left, BlendMode blend_mode)
{
    if (!top_left && !top_right && !bottom_right && !bottom_left) {
        if (blend_mode == BlendMode::Normal)
            return m_underlying_painter.fill_rect(a_rect, color);
        else if (blend_mode == BlendMode::AlphaSubtract)
            return m_underlying_painter.clear_rect(a_rect, Color());
    }

    if (color.alpha() == 0)
        return;

    IntPoint top_left_corner {
        a_rect.x() + top_left.horizontal_radius,
        a_rect.y() + top_left.vertical_radius,
    };
    IntPoint top_right_corner {
        a_rect.x() + a_rect.width() - top_right.horizontal_radius,
        a_rect.y() + top_right.vertical_radius,
    };
    IntPoint bottom_left_corner {
        a_rect.x() + bottom_left.horizontal_radius,
        a_rect.y() + a_rect.height() - bottom_left.vertical_radius
    };
    IntPoint bottom_right_corner {
        a_rect.x() + a_rect.width() - bottom_right.horizontal_radius,
        a_rect.y() + a_rect.height() - bottom_right.vertical_radius
    };

    // All corners are centered at the same point, so this can be painted as a single ellipse.
    if (top_left_corner == top_right_corner && top_right_corner == bottom_left_corner && bottom_left_corner == bottom_right_corner)
        return fill_ellipse(a_rect, color, blend_mode);

    IntRect top_rect {
        a_rect.x() + top_left.horizontal_radius,
        a_rect.y(),
        a_rect.width() - top_left.horizontal_radius - top_right.horizontal_radius,
        top_left.vertical_radius
    };
    IntRect right_rect {
        a_rect.x() + a_rect.width() - top_right.horizontal_radius,
        a_rect.y() + top_right.vertical_radius,
        top_right.horizontal_radius,
        a_rect.height() - top_right.vertical_radius - bottom_right.vertical_radius
    };
    IntRect bottom_rect {
        a_rect.x() + bottom_left.horizontal_radius,
        a_rect.y() + a_rect.height() - bottom_right.vertical_radius,
        a_rect.width() - bottom_left.horizontal_radius - bottom_right.horizontal_radius,
        bottom_right.vertical_radius
    };
    IntRect left_rect {
        a_rect.x(),
        a_rect.y() + top_left.vertical_radius,
        bottom_left.horizontal_radius,
        a_rect.height() - top_left.vertical_radius - bottom_left.vertical_radius
    };

    IntRect inner = {
        left_rect.x() + left_rect.width(),
        left_rect.y(),
        a_rect.width() - left_rect.width() - right_rect.width(),
        a_rect.height() - top_rect.height() - bottom_rect.height()
    };

    if (blend_mode == BlendMode::Normal) {
        m_underlying_painter.fill_rect(top_rect, color);
        m_underlying_painter.fill_rect(right_rect, color);
        m_underlying_painter.fill_rect(bottom_rect, color);
        m_underlying_painter.fill_rect(left_rect, color);
        m_underlying_painter.fill_rect(inner, color);
    } else if (blend_mode == BlendMode::AlphaSubtract) {
        m_underlying_painter.clear_rect(top_rect, Color());
        m_underlying_painter.clear_rect(right_rect, Color());
        m_underlying_painter.clear_rect(bottom_rect, Color());
        m_underlying_painter.clear_rect(left_rect, Color());
        m_underlying_painter.clear_rect(inner, Color());
    }

    auto fill_corner = [&](auto const& ellipse_center, auto const& corner_point, CornerRadius const& corner) {
        PainterStateSaver save { m_underlying_painter };
        m_underlying_painter.add_clip_rect(IntRect::from_two_points(ellipse_center, corner_point));
        fill_ellipse(IntRect::centered_at(ellipse_center, { corner.horizontal_radius * 2, corner.vertical_radius * 2 }), color, blend_mode);
    };

    auto bounding_rect = a_rect.inflated(0, 1, 1, 0);
    if (top_left)
        fill_corner(top_left_corner, bounding_rect.top_left(), top_left);
    if (top_right)
        fill_corner(top_right_corner, bounding_rect.top_right().moved_left(1), top_right);
    if (bottom_left)
        fill_corner(bottom_left_corner, bounding_rect.bottom_left().moved_up(1), bottom_left);
    if (bottom_right)
        fill_corner(bottom_right_corner, bounding_rect.bottom_right().translated(-1), bottom_right);
}

}
