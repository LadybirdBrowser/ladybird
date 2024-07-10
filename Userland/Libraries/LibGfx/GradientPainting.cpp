/*
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGfx/Gradients.h>
#include <LibGfx/PaintStyle.h>
#include <LibGfx/Painter.h>

#if defined(AK_COMPILER_GCC)
#    pragma GCC optimize("O3")
#endif

namespace Gfx {

// Note: This file implements the CSS/Canvas gradients for LibWeb according to the spec.
// Please do not make ad-hoc changes that may break spec compliance!

float color_stop_step(ColorStop const& previous_stop, ColorStop const& next_stop, float position)
{
    if (position < previous_stop.position)
        return 0;
    if (position > next_stop.position)
        return 1;
    // For any given point between the two color stops,
    // determine the point’s location as a percentage of the distance between the two color stops.
    // Let this percentage be P.
    auto stop_length = next_stop.position - previous_stop.position;
    // FIXME: Avoids NaNs... Still not quite correct?
    if (stop_length <= 0)
        return 1;
    auto p = (position - previous_stop.position) / stop_length;
    if (!next_stop.transition_hint.has_value())
        return p;
    if (*next_stop.transition_hint >= 1)
        return 0;
    if (*next_stop.transition_hint <= 0)
        return 1;
    // Let C, the color weighting at that point, be equal to P^(logH(.5)).
    auto c = AK::pow(p, AK::log<float>(0.5) / AK::log(*next_stop.transition_hint));
    // The color at that point is then a linear blend between the colors of the two color stops,
    // blending (1 - C) of the first stop and C of the second stop.
    return c;
}

enum class UsePremultipliedAlpha {
    Yes,
    No
};

class GradientLine {
public:
    GradientLine(int gradient_length, ReadonlySpan<ColorStop> color_stops, Optional<float> repeat_length, UsePremultipliedAlpha use_premultiplied_alpha = UsePremultipliedAlpha::Yes)
        : m_repeat_mode(repeat_length.has_value() ? RepeatMode::Repeat : RepeatMode::None)
        , m_start_offset(round_to<int>((repeating() ? color_stops.first().position : 0.0f) * gradient_length))
        , m_color_stops(color_stops)
        , m_use_premultiplied_alpha(use_premultiplied_alpha)
    {
        // Avoid generating excessive amounts of colors when the not enough shades to fill that length.
        auto necessary_length = min<int>((color_stops.size() - 1) * 255, gradient_length);
        m_sample_scale = float(necessary_length) / gradient_length;
        // Note: color_count will be < gradient_length for repeating gradients.
        auto color_count = round_to<int>(repeat_length.value_or(1.0f) * necessary_length);
        m_gradient_line_colors.resize(color_count);

        for (int loc = 0; loc < color_count; loc++) {
            auto relative_loc = float(loc + m_start_offset) / necessary_length;
            Color gradient_color = color_blend(color_stops[0].color, color_stops[1].color,
                color_stop_step(color_stops[0], color_stops[1], relative_loc));
            for (size_t i = 1; i < color_stops.size() - 1; i++) {
                gradient_color = color_blend(gradient_color, color_stops[i + 1].color,
                    color_stop_step(color_stops[i], color_stops[i + 1], relative_loc));
            }
            m_gradient_line_colors[loc] = gradient_color;
            if (gradient_color.alpha() < 255)
                m_requires_blending = true;
        }
    }

    Color color_blend(Color a, Color b, float amount) const
    {
        // Note: color.mixed_with() performs premultiplied alpha mixing when necessary as defined in:
        // https://drafts.csswg.org/css-images/#coloring-gradient-line
        if (m_use_premultiplied_alpha == UsePremultipliedAlpha::Yes)
            return a.mixed_with(b, amount);
        return a.interpolate(b, amount);
    }

    Color get_color(i64 index) const
    {
        if (index < 0)
            return m_color_stops.first().color;
        if (index >= static_cast<i64>(m_gradient_line_colors.size()))
            return m_color_stops.last().color;
        return m_gradient_line_colors[index];
    }

    Color sample_color(float loc) const
    {
        if (!isfinite(loc))
            return Color();
        if (m_sample_scale != 1.0f)
            loc *= m_sample_scale;
        auto repeat_wrap_if_required = [&](i64 loc) {
            if (m_repeat_mode != RepeatMode::None) {
                auto current_loc = loc + m_start_offset;
                auto gradient_len = static_cast<i64>(m_gradient_line_colors.size());
                if (m_repeat_mode == RepeatMode::Repeat) {
                    auto color_loc = current_loc % gradient_len;
                    return color_loc < 0 ? gradient_len + color_loc : color_loc;
                } else if (m_repeat_mode == RepeatMode::Reflect) {
                    auto color_loc = AK::abs(current_loc % gradient_len);
                    auto repeats = current_loc / gradient_len;
                    return (repeats & 1) ? gradient_len - color_loc : color_loc;
                }
            }
            return loc;
        };
        auto int_loc = static_cast<i64>(floor(loc));
        auto blend = loc - int_loc;
        auto color = get_color(repeat_wrap_if_required(int_loc));
        // Blend between the two neighboring colors (this fixes some nasty aliasing issues at small angles)
        if (blend >= 0.004f)
            color = color_blend(color, get_color(repeat_wrap_if_required(int_loc + 1)), blend);
        return color;
    }

    void paint_into_physical_rect(Painter& painter, IntRect rect, auto location_transform)
    {
        auto clipped_rect = rect.intersected(painter.clip_rect());
        auto start_offset = clipped_rect.location() - rect.location();
        for (int y = 0; y < clipped_rect.height(); y++) {
            for (int x = 0; x < clipped_rect.width(); x++) {
                auto pixel = sample_color(location_transform(x + start_offset.x(), y + start_offset.y()));
                painter.set_physical_pixel(clipped_rect.location().translated(x, y), pixel, m_requires_blending);
            }
        }
    }

    bool repeating() const
    {
        return m_repeat_mode != RepeatMode::None;
    }

    enum class RepeatMode {
        None,
        Repeat,
        Reflect
    };

    void set_repeat_mode(RepeatMode repeat_mode)
    {
        // Note: A gradient can be set to repeating without a repeat length.
        // The repeat length is used for CSS gradients but not for SVG gradients.
        m_repeat_mode = repeat_mode;
    }

private:
    RepeatMode m_repeat_mode { RepeatMode::None };
    int m_start_offset { 0 };
    float m_sample_scale { 1 };
    ReadonlySpan<ColorStop> m_color_stops {};
    UsePremultipliedAlpha m_use_premultiplied_alpha { UsePremultipliedAlpha::Yes };

    Vector<Color, 1024> m_gradient_line_colors;
    bool m_requires_blending = false;
};

template<typename TransformFunction>
struct Gradient {
    Gradient(GradientLine gradient_line, TransformFunction transform_function)
        : m_gradient_line(move(gradient_line))
        , m_transform_function(move(transform_function))
    {
    }

    void paint(Painter& painter, IntRect rect)
    {
        m_gradient_line.paint_into_physical_rect(painter, rect, m_transform_function);
    }

    template<typename CoordinateType = int>
    auto sample_function()
    {
        return [this](Point<CoordinateType> point) {
            return m_gradient_line.sample_color(m_transform_function(point.x(), point.y()));
        };
    }

    GradientLine& gradient_line()
    {
        return m_gradient_line;
    }

private:
    GradientLine m_gradient_line;
    TransformFunction m_transform_function;
};

static auto create_linear_gradient(IntRect const& physical_rect, ReadonlySpan<ColorStop> color_stops, float angle, Optional<float> repeat_length)
{
    float normalized_angle = normalized_gradient_angle_radians(angle);
    float sin_angle, cos_angle;
    AK::sincos(normalized_angle, sin_angle, cos_angle);

    // Full length of the gradient
    auto gradient_length = calculate_gradient_length(physical_rect.size(), sin_angle, cos_angle);
    IntPoint offset { cos_angle * (gradient_length / 2), sin_angle * (gradient_length / 2) };
    auto center = physical_rect.translated(-physical_rect.location()).center();
    auto start_point = center - offset;
    // Rotate gradient line to be horizontal
    auto rotated_start_point_x = start_point.x() * cos_angle - start_point.y() * -sin_angle;

    GradientLine gradient_line(gradient_length, color_stops, repeat_length);
    return Gradient {
        move(gradient_line),
        [=](int x, int y) {
            return (x * cos_angle - (physical_rect.height() - y) * -sin_angle) - rotated_start_point_x;
        }
    };
}

static auto create_conic_gradient(ReadonlySpan<ColorStop> color_stops, FloatPoint center_point, float start_angle, Optional<float> repeat_length, UsePremultipliedAlpha use_premultiplied_alpha = UsePremultipliedAlpha::Yes)
{
    // FIXME: Do we need/want sub-degree accuracy for the gradient line?
    GradientLine gradient_line(360, color_stops, repeat_length, use_premultiplied_alpha);
    float normalized_start_angle = (360.0f - start_angle) + 90.0f;
    // The flooring can make gradients that want soft edges look worse, so only floor if we have hard edges.
    // Which makes sure the hard edge stay hard edges :^)
    bool should_floor_angles = false;
    for (size_t i = 0; i < color_stops.size() - 1; i++) {
        if (color_stops[i + 1].position - color_stops[i].position <= 0.01f) {
            should_floor_angles = true;
            break;
        }
    }
    return Gradient {
        move(gradient_line),
        [=](int x, int y) {
            auto point = FloatPoint { x, y } - center_point;
            // FIXME: We could probably get away with some approximation here:
            auto loc = fmod((AK::to_degrees(AK::atan2(point.y(), point.x())) + 360.0f + normalized_start_angle), 360.0f);
            return should_floor_angles ? floor(loc) : loc;
        }
    };
}

static auto create_radial_gradient(IntRect const& physical_rect, ReadonlySpan<ColorStop> color_stops, IntPoint center, IntSize size, Optional<float> repeat_length, Optional<float> rotation_angle = {})
{
    // A conservative guesstimate on how many colors we need to generate:
    auto max_dimension = max(physical_rect.width(), physical_rect.height());
    auto max_visible_gradient = max(max_dimension / 2, min(size.width(), max_dimension));
    GradientLine gradient_line(max_visible_gradient, color_stops, repeat_length);
    auto center_point = FloatPoint { center }.translated(0.5, 0.5);
    AffineTransform rotation_transform;
    if (rotation_angle.has_value()) {
        auto angle_as_radians = AK::to_radians(rotation_angle.value());
        rotation_transform.rotate_radians(angle_as_radians);
    }

    return Gradient {
        move(gradient_line),
        [=](int x, int y) {
            // FIXME: See if there's a more efficient calculation we do there :^)
            auto point = FloatPoint(x, y) - center_point;

            if (rotation_angle.has_value())
                point.transform_by(rotation_transform);

            auto gradient_x = point.x() / size.width();
            auto gradient_y = point.y() / size.height();
            return AK::sqrt(gradient_x * gradient_x + gradient_y * gradient_y) * max_visible_gradient;
        }
    };
}

void Painter::fill_rect_with_linear_gradient(IntRect const& rect, ReadonlySpan<ColorStop> color_stops, float angle, Optional<float> repeat_length)
{
    auto a_rect = to_physical(rect);
    if (a_rect.intersected(clip_rect()).is_empty())
        return;
    auto linear_gradient = create_linear_gradient(a_rect, color_stops, angle, repeat_length);
    linear_gradient.paint(*this, a_rect);
}

static FloatPoint pixel_center(IntPoint point)
{
    return point.to_type<float>().translated(0.5f, 0.5f);
}

void Painter::fill_rect_with_conic_gradient(IntRect const& rect, ReadonlySpan<ColorStop> color_stops, IntPoint center, float start_angle, Optional<float> repeat_length)
{
    auto a_rect = to_physical(rect);
    if (a_rect.intersected(clip_rect()).is_empty())
        return;
    // Translate position/center to the center of the pixel (avoids some funky painting)
    auto center_point = pixel_center(center);
    auto conic_gradient = create_conic_gradient(color_stops, center_point, start_angle, repeat_length);
    conic_gradient.paint(*this, a_rect);
}

void Painter::fill_rect_with_radial_gradient(IntRect const& rect, ReadonlySpan<ColorStop> color_stops, IntPoint center, IntSize size, Optional<float> repeat_length, Optional<float> rotation_angle)
{
    auto a_rect = to_physical(rect);
    if (a_rect.intersected(clip_rect()).is_empty())
        return;

    auto radial_gradient = create_radial_gradient(a_rect, color_stops, center, size, repeat_length, rotation_angle);
    radial_gradient.paint(*this, a_rect);
}

// TODO: Figure out how to handle scale() here... Not important while not supported by fill_path()

void LinearGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    VERIFY(color_stops().size() > 2);
    auto linear_gradient = create_linear_gradient(physical_bounding_box, color_stops(), m_angle, repeat_length());
    paint(linear_gradient.sample_function());
}

void ConicGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    VERIFY(color_stops().size() > 2);
    (void)physical_bounding_box;
    auto conic_gradient = create_conic_gradient(color_stops(), pixel_center(m_center), m_start_angle, repeat_length());
    paint(conic_gradient.sample_function());
}

void RadialGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    VERIFY(color_stops().size() > 2);
    auto radial_gradient = create_radial_gradient(physical_bounding_box, color_stops(), m_center, m_size, repeat_length());
    paint(radial_gradient.sample_function());
}

// The following implements the gradient fill/stoke styles for the HTML canvas: https://html.spec.whatwg.org/multipage/canvas.html#fill-and-stroke-styles

static auto make_sample_non_relative(IntPoint draw_location, auto sample)
{
    return [=, sample = move(sample)](IntPoint point) { return sample(point.translated(draw_location)); };
}

static auto make_linear_gradient_between_two_points(FloatPoint p0, FloatPoint p1, ReadonlySpan<ColorStop> color_stops, Optional<float> repeat_length)
{
    auto delta = p1 - p0;
    auto angle = AK::atan2(delta.y(), delta.x());
    float sin_angle, cos_angle;
    AK::sincos(angle, sin_angle, cos_angle);
    int gradient_length = ceilf(p1.distance_from(p0));
    auto rotated_start_point_x = p0.x() * cos_angle - p0.y() * -sin_angle;

    return Gradient {
        GradientLine(gradient_length, color_stops, repeat_length, UsePremultipliedAlpha::No),
        [=](int x, int y) {
            return (x * cos_angle - y * -sin_angle) - rotated_start_point_x;
        }
    };
}

void CanvasLinearGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    // If x0 = x1 and y0 = y1, then the linear gradient must paint nothing.
    if (m_p0 == m_p1)
        return;
    if (color_stops().is_empty())
        return;
    if (color_stops().size() < 2)
        return paint([this](IntPoint) { return color_stops().first().color; });

    auto linear_gradient = make_linear_gradient_between_two_points(m_p0, m_p1, color_stops(), repeat_length());
    paint(make_sample_non_relative(physical_bounding_box.location(), linear_gradient.sample_function()));
}

static GradientLine::RepeatMode svg_spread_method_to_repeat_mode(SVGGradientPaintStyle::SpreadMethod spread_method)
{
    switch (spread_method) {
    case SVGGradientPaintStyle::SpreadMethod::Pad:
        return GradientLine::RepeatMode::None;
    case SVGGradientPaintStyle::SpreadMethod::Reflect:
        return GradientLine::RepeatMode::Reflect;
    case SVGGradientPaintStyle::SpreadMethod::Repeat:
        return GradientLine::RepeatMode::Repeat;
    default:
        VERIFY_NOT_REACHED();
    }
}

void SVGGradientPaintStyle::set_gradient_transform(AffineTransform transform)
{
    // Note: The scaling is removed so enough points on the gradient line are generated.
    // Otherwise, if you scale a tiny path the gradient looks pixelated.
    m_scale = 1.0f;
    if (auto inverse = transform.inverse(); inverse.has_value()) {
        auto transform_scale = transform.scale();
        m_scale = max(transform_scale.x(), transform_scale.y());
        m_inverse_transform = AffineTransform {}.scale(m_scale, m_scale).multiply(*inverse);
    } else {
        m_inverse_transform = OptionalNone {};
    }
}

void SVGLinearGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    if (color_stops().is_empty())
        return;
    // If ‘x1’ = ‘x2’ and ‘y1’ = ‘y2’, then the area to be painted will be painted as
    // a single color using the color and opacity of the last gradient stop.
    if (m_p0 == m_p1)
        return paint([this](IntPoint) { return color_stops().last().color; });
    if (color_stops().size() < 2)
        return paint([this](IntPoint) { return color_stops().first().color; });

    float scale = gradient_transform_scale();
    auto linear_gradient = make_linear_gradient_between_two_points(
        m_p0.scaled(scale, scale), m_p1.scaled(scale, scale),
        color_stops(), repeat_length());
    linear_gradient.gradient_line().set_repeat_mode(
        svg_spread_method_to_repeat_mode(spread_method()));

    paint([&, sampler = linear_gradient.sample_function<float>()](IntPoint target_point) {
        auto point = target_point.translated(physical_bounding_box.location()).to_type<float>();
        if (auto inverse_transform = scale_adjusted_inverse_gradient_transform(); inverse_transform.has_value())
            point = inverse_transform->map(point);

        return sampler(point);
    });
}

void CanvasConicGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    if (color_stops().is_empty())
        return;
    if (color_stops().size() < 2)
        return paint([this](IntPoint) { return color_stops().first().color; });

    // Follows the same rendering rule as CSS 'conic-gradient' and it is equivalent to CSS
    // 'conic-gradient(from adjustedStartAnglerad at xpx ypx, angularColorStopList)'.
    //  Here:
    //      adjustedStartAngle is given by startAngle + π/2;
    auto conic_gradient = create_conic_gradient(color_stops(), m_center, m_start_angle + 90.0f, repeat_length(), UsePremultipliedAlpha::No);
    paint(make_sample_non_relative(physical_bounding_box.location(), conic_gradient.sample_function()));
}

static auto create_radial_gradient_between_two_circles(Gfx::FloatPoint start_center, float start_radius, Gfx::FloatPoint end_center, float end_radius, ReadonlySpan<ColorStop> color_stops, Optional<float> repeat_length)
{
    bool reverse_gradient = end_radius < start_radius;
    if (reverse_gradient) {
        swap(end_radius, start_radius);
        swap(end_center, start_center);
    }

    // FIXME: Handle the start_radius == end_radius special case separately.
    // This hack is not quite correct.
    if (end_radius - start_radius < 1)
        end_radius += 1;

    // Spec steps: Useless for writing an actual implementation (give it a go :P):
    //
    // 2. Let x(ω) = (x1-x0)ω + x0
    //    Let y(ω) = (y1-y0)ω + y0
    //    Let r(ω) = (r1-r0)ω + r0
    // Let the color at ω be the color at that position on the gradient
    // (with the colors coming from the interpolation and extrapolation described above).
    //
    // 3. For all values of ω where r(ω) > 0, starting with the value of ω nearest to positive infinity and
    // ending with the value of ω nearest to negative infinity, draw the circumference of the circle with
    // radius r(ω) at position (x(ω), y(ω)), with the color at ω, but only painting on the parts of the
    // bitmap that have not yet been painted on by earlier circles in this step for this rendering of the gradient.

    auto center_dist = end_center.distance_from(start_center);
    bool inner_contained = ((center_dist + start_radius) < end_radius);

    auto start_point = start_center;
    if (start_radius != 0) {
        // Set the start point to the focal point.
        auto f = end_radius / (end_radius - start_radius);
        auto one_minus_f = 1 - f;
        start_point = start_center.scaled(f) + end_center.scaled(one_minus_f);
    }

    // This is just an approximate upperbound (the gradient line class will shorten this if necessary).
    int gradient_length = AK::ceil(center_dist + end_radius + start_radius);
    GradientLine gradient_line(gradient_length, color_stops, repeat_length, UsePremultipliedAlpha::No);

    // If you can simplify this please do, this is "best guess" implementation due to lack of specification.
    // It was implemented to visually match chrome/firefox in all cases:
    //      - Start circle inside end circle
    //      - Start circle outside end circle
    //      - Start circle radius == end circle radius
    //      - Start circle larger than end circle (inside end circle)
    //      - Start circle larger than end circle (outside end circle)
    //      - Start circle or end circle radius == 0

    auto circle_distance_finder = [=](auto radius, auto center) {
        auto radius2 = radius * radius;
        auto delta = center - start_point;
        auto delta_xy = delta.x() * delta.y();
        auto dx2_factor = radius2 - delta.y() * delta.y();
        auto dy2_factor = radius2 - delta.x() * delta.x();
        return [=](bool positive_root, auto vec) {
            // This works out the distance to the nearest point on the circle
            // in the direction of the "vec" vector.
            auto dx2 = vec.x() * vec.x();
            auto dy2 = vec.y() * vec.y();
            auto root = sqrtf(dx2 * dx2_factor + dy2 * dy2_factor
                + 2 * vec.x() * vec.y() * delta_xy);
            auto dot = vec.x() * delta.x() + vec.y() * delta.y();
            return ((positive_root ? root : -root) + dot) / (dx2 + dy2);
        };
    };

    auto end_circle_dist = circle_distance_finder(end_radius, end_center);
    auto start_circle_dist = [=, dist = circle_distance_finder(start_radius, start_center)](bool positive_root, auto vec) {
        if (start_center == start_point)
            return start_radius;
        return dist(positive_root, vec);
    };

    return Gradient {
        move(gradient_line),
        [=](float x, float y) {
            auto loc = [&] {
                FloatPoint point { x, y };
                // Add a little to avoid division by zero at the focal point.
                if (point == start_point)
                    point += FloatPoint { 0.001f, 0.001f };
                // The "vec" (unit) vector points from the focal point to the current point.
                auto dist = point.distance_from(start_point);
                auto vec = (point - start_point) / dist;
                bool use_positive_root = inner_contained || reverse_gradient;
                auto dist_end = end_circle_dist(use_positive_root, vec);
                auto dist_start = start_circle_dist(use_positive_root, vec);
                // FIXME: Returning nan is a hack for "Don't paint me!"
                if (dist_end < 0)
                    return AK::NaN<float>;
                if (dist_end - dist_start < 0)
                    return float(gradient_length);
                return (dist - dist_start) / (dist_end - dist_start);
            }();
            if (reverse_gradient)
                loc = 1.0f - loc;
            return loc * gradient_length;
        }
    };
}

void CanvasRadialGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    // 1. If x0 = x1 and y0 = y1 and r0 = r1, then the radial gradient must paint nothing. Return.
    if (m_start_center == m_end_center && m_start_radius == m_end_radius)
        return;
    if (color_stops().is_empty())
        return;
    if (color_stops().size() < 2)
        return paint([this](IntPoint) { return color_stops().first().color; });
    if (m_end_radius == 0 && m_start_radius == 0)
        return;
    auto radial_gradient = create_radial_gradient_between_two_circles(m_start_center, m_start_radius, m_end_center, m_end_radius, color_stops(), repeat_length());
    paint(make_sample_non_relative(physical_bounding_box.location(), radial_gradient.sample_function()));
}

void SVGRadialGradientPaintStyle::paint(IntRect physical_bounding_box, PaintFunction paint) const
{
    // FIXME: Ensure this handles all the edge cases of SVG gradients.
    if (color_stops().is_empty())
        return;
    if (color_stops().size() < 2 || (m_end_radius == 0 && m_start_radius == 0))
        return paint([this](IntPoint) { return color_stops().last().color; });

    float scale = gradient_transform_scale();
    auto radial_gradient = create_radial_gradient_between_two_circles(
        m_start_center.scaled(scale, scale), m_start_radius * scale, m_end_center.scaled(scale, scale), m_end_radius * scale,
        color_stops(), repeat_length());
    radial_gradient.gradient_line().set_repeat_mode(
        svg_spread_method_to_repeat_mode(spread_method()));

    paint([&, sampler = radial_gradient.sample_function<float>()](IntPoint target_point) {
        auto point = target_point.translated(physical_bounding_box.location()).to_type<float>();
        if (auto inverse_transform = scale_adjusted_inverse_gradient_transform(); inverse_transform.has_value())
            point = inverse_transform->map(point);
        return sampler(point);
    });
}

}
