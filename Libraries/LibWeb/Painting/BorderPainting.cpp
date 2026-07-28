/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BorderPainting.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

namespace Web::Painting {

static constexpr double dark_light_absolute_value_difference = 1. / 3;

static Color light_color_for_inset_and_outset(Color const& color)
{
    auto hsv = color.to_hsv();
    if (hsv.value >= dark_light_absolute_value_difference)
        return color;
    auto result = Color::from_hsv({ hsv.hue, hsv.saturation, hsv.value + dark_light_absolute_value_difference });
    result.set_alpha(color.alpha());
    return result;
}

static Color dark_color_for_inset_and_outset(Color const& color)
{
    auto hsv = color.to_hsv();
    if (hsv.value < dark_light_absolute_value_difference)
        return color;
    auto result = Color::from_hsv({ hsv.hue, hsv.saturation, hsv.value - dark_light_absolute_value_difference });
    result.set_alpha(color.alpha());
    return result;
}

Color border_color(BorderEdge edge, BordersDataDevicePixels const& borders_data)
{
    auto const& border_data = borders_data.for_edge(edge);

    if (border_data.line_style == CSS::LineStyle::Inset) {
        if (edge == BorderEdge::Left || edge == BorderEdge::Top)
            return dark_color_for_inset_and_outset(border_data.color);
        return light_color_for_inset_and_outset(border_data.color);
    }
    if (border_data.line_style == CSS::LineStyle::Outset) {
        if (edge == BorderEdge::Left || edge == BorderEdge::Top)
            return light_color_for_inset_and_outset(border_data.color);
        return dark_color_for_inset_and_outset(border_data.color);
    }

    return border_data.color;
}

// https://drafts.csswg.org/css-backgrounds-3/#corner-transitions
// Returns the offset from the center of a corner's ellipse to the point where the two borders meeting at that corner
// are split. Both components are positive; the caller applies the signs pointing towards its own corner. The spec
// leaves the point itself undefined, requiring only that it move continuously and monotonically with the ratio of the
// two border widths, so this takes where the curve is crossed by the line running from the corner of the border box to
// the corner of the padding box. That degenerates to the whole corner going to whichever border still has a width when
// the other reaches zero, which is the behavior the spec calls out separately.
static Gfx::FloatPoint compute_midpoint(float horizontal_radius, float vertical_radius, float horizontal_border_width, float vertical_border_width)
{
    // Without a curve in one of the two directions there is no arc to divide between the borders.
    if (horizontal_radius == 0 || vertical_radius == 0)
        return { horizontal_radius, vertical_radius };

    if (horizontal_border_width == 0 && vertical_border_width == 0)
        return {};

    // Substituting that line into the ellipse leaves a quadratic in how far along it the crossing lies, of which the
    // smaller root is the crossing nearest the corner.
    auto a = vertical_border_width * vertical_border_width / (horizontal_radius * horizontal_radius)
        + horizontal_border_width * horizontal_border_width / (vertical_radius * vertical_radius);
    auto b = vertical_border_width / horizontal_radius + horizontal_border_width / vertical_radius;
    auto distance = (b - AK::sqrt(2 * vertical_border_width * horizontal_border_width / (horizontal_radius * vertical_radius))) / a;

    return {
        horizontal_radius - distance * vertical_border_width,
        vertical_radius - distance * horizontal_border_width
    };
}

// Dashes and dots run along the middle of the border, so instead of filling the exact region covered by the edge -
// which is what the solid border painting below does - this strokes a centerline path: the half of the corner arc
// leading into the edge, the straight run, and the half of the corner arc leading out of it.
static void paint_patterned_border(DisplayListRecorder& painter, BorderEdge edge, DevicePixelRect const& rect,
    Gfx::CornerRadius const& radius, Gfx::CornerRadius const& opposite_radius,
    BordersDataDevicePixels const& borders_data, Color color, Gfx::LineStyle line_style)
{
    auto width = static_cast<float>(borders_data.for_edge(edge).width.value());
    auto half_width = width / 2.f;

    float joined_border_width = 0;
    float opposite_joined_border_width = 0;
    switch (edge) {
    case BorderEdge::Top:
        joined_border_width = static_cast<float>(borders_data.left.width.value());
        opposite_joined_border_width = static_cast<float>(borders_data.right.width.value());
        break;
    case BorderEdge::Right:
        joined_border_width = static_cast<float>(borders_data.top.width.value());
        opposite_joined_border_width = static_cast<float>(borders_data.bottom.width.value());
        break;
    case BorderEdge::Bottom:
        joined_border_width = static_cast<float>(borders_data.right.width.value());
        opposite_joined_border_width = static_cast<float>(borders_data.left.width.value());
        break;
    case BorderEdge::Left:
        joined_border_width = static_cast<float>(borders_data.bottom.width.value());
        opposite_joined_border_width = static_cast<float>(borders_data.top.width.value());
        break;
    }

    // Each corner arc is described by the center of its ellipse, the radii the centerline follows, and the direction
    // from that center towards the corner.
    struct Corner {
        Gfx::FloatPoint center;
        Gfx::FloatSize radii;
        Gfx::FloatPoint direction;
    };

    auto is_horizontal_edge = edge == BorderEdge::Top || edge == BorderEdge::Bottom;

    auto centerline_radii = [&](Gfx::CornerRadius const& corner, float joined_width) {
        auto horizontal_inset = is_horizontal_edge ? joined_width / 2.f : half_width;
        auto vertical_inset = is_horizontal_edge ? half_width : joined_width / 2.f;
        return Gfx::FloatSize(max(0.f, static_cast<float>(corner.horizontal_radius) - horizontal_inset), max(0.f, static_cast<float>(corner.vertical_radius) - vertical_inset));
    };

    auto left = static_cast<float>(rect.left().value());
    auto top = static_cast<float>(rect.top().value());
    auto right = static_cast<float>(rect.right().value());
    auto bottom = static_cast<float>(rect.bottom().value());

    auto horizontal_radius = static_cast<float>(radius.horizontal_radius);
    auto vertical_radius = static_cast<float>(radius.vertical_radius);
    auto opposite_horizontal_radius = static_cast<float>(opposite_radius.horizontal_radius);
    auto opposite_vertical_radius = static_cast<float>(opposite_radius.vertical_radius);

    Corner start;
    Corner end;
    Gfx::FloatPoint straight_start;
    Gfx::FloatPoint straight_end;
    switch (edge) {
    case BorderEdge::Top:
        start = { { left, top + vertical_radius }, centerline_radii(radius, joined_border_width), { -1, -1 } };
        end = { { right, top + opposite_vertical_radius }, centerline_radii(opposite_radius, opposite_joined_border_width), { 1, -1 } };
        straight_start = { left, top + half_width };
        straight_end = { right, top + half_width };
        break;
    case BorderEdge::Right:
        start = { { right - horizontal_radius, top }, centerline_radii(radius, joined_border_width), { 1, -1 } };
        end = { { right - opposite_horizontal_radius, bottom }, centerline_radii(opposite_radius, opposite_joined_border_width), { 1, 1 } };
        straight_start = { right - half_width, top };
        straight_end = { right - half_width, bottom };
        break;
    case BorderEdge::Bottom:
        start = { { right, bottom - vertical_radius }, centerline_radii(radius, joined_border_width), { 1, 1 } };
        end = { { left, bottom - opposite_vertical_radius }, centerline_radii(opposite_radius, opposite_joined_border_width), { -1, 1 } };
        straight_start = { right, bottom - half_width };
        straight_end = { left, bottom - half_width };
        break;
    case BorderEdge::Left:
        start = { { left + horizontal_radius, bottom }, centerline_radii(radius, joined_border_width), { -1, 1 } };
        end = { { left + opposite_horizontal_radius, top }, centerline_radii(opposite_radius, opposite_joined_border_width), { -1, -1 } };
        straight_start = { left + half_width, bottom };
        straight_end = { left + half_width, top };
        break;
    }

    auto split_point = [&](Corner const& corner, float joined_width) {
        auto midpoint = compute_midpoint(corner.radii.width(), corner.radii.height(),
            is_horizontal_edge ? width : joined_width,
            is_horizontal_edge ? joined_width : width);
        return corner.center + Gfx::FloatPoint(corner.direction.x() * midpoint.x(), corner.direction.y() * midpoint.y());
    };

    // A corner whose centerline radii have collapsed leaves the dashes meeting at a sharp angle instead of curving.
    auto has_arc = [](Corner const& corner) { return corner.radii.width() > 0 && corner.radii.height() > 0; };

    Gfx::Path centerline;
    if (has_arc(start)) {
        centerline.move_to(split_point(start, joined_border_width));
        centerline.elliptical_arc_to(straight_start, start.radii, 0, false, true);
    } else {
        centerline.move_to(straight_start);
    }
    centerline.line_to(straight_end);
    if (has_arc(end))
        centerline.elliptical_arc_to(split_point(end, opposite_joined_border_width), end.radii, 0, false, true);

    auto length = centerline.length();

    Gfx::Path::CapStyle cap_style;
    Vector<float> dash_array;
    float dash_offset = 0;
    if (line_style == Gfx::LineStyle::Dotted) {
        // Dots are zero-length dashes rounded out by the cap. Offsetting them by half an interval keeps every dot clear
        // of the corners, so that the spacing across a corner matches the spacing along an edge and neither of the two
        // borders meeting there has to win a dot sitting on the point where they split.
        cap_style = Gfx::Path::CapStyle::Round;
        auto interval = length / max(1.f, roundf(length / (2 * width)));
        dash_array = { 0, interval };
        dash_offset = interval / 2;
    } else {
        // Each edge begins and ends with half a dash, so that the two halves meeting at a corner form a single dash
        // centered on it instead of two dashes sitting flush. That requires the centerline to span a whole number of
        // dash-and-gap periods, with the pattern offset by half a dash.
        cap_style = Gfx::Path::CapStyle::Butt;
        auto interval = length / (2 * max(1.f, roundf(length / (4 * width))));
        dash_array = { interval, interval };
        dash_offset = interval / 2;
    }

    painter.stroke_path({
        .cap_style = cap_style,
        .join_style = Gfx::Path::JoinStyle::Miter,
        .miter_limit = 4,
        .dash_array = move(dash_array),
        .dash_offset = dash_offset,
        .path = move(centerline),
        .paint_style_or_color = color,
        .thickness = width,
    });
}

void paint_border(DisplayListRecorder& painter, BorderEdge edge, DevicePixelRect const& rect, Gfx::CornerRadius const& radius, Gfx::CornerRadius const& opposite_radius, BordersDataDevicePixels const& borders_data, Gfx::Path& path, bool last)
{
    auto const& border_data = borders_data.for_edge(edge);

    if (border_data.width <= 0)
        return;

    auto color = border_color(edge, borders_data);
    auto border_style = border_data.line_style;

    auto paint_double_border = [&](float proportional_line_thickness, CSS::LineStyle outer_style, CSS::LineStyle inner_style) {
        // AD-HOC: We clamp the individual borders to 1px thick if they're less so that they don't disappear entirely.
        //         This matches other browsers and is allowable per the spec, where the thickness is implementation-defined.

        // FIXME: Converting to floats and back is awkward, can we somehow do all this processing using CSSPixels?
        auto modified_borders_data = borders_data;
        modified_borders_data.top.width = max(1, static_cast<float>(modified_borders_data.top.width.value()) * proportional_line_thickness);
        modified_borders_data.right.width = max(1, static_cast<float>(modified_borders_data.right.width.value()) * proportional_line_thickness);
        modified_borders_data.bottom.width = max(1, static_cast<float>(modified_borders_data.bottom.width.value()) * proportional_line_thickness);
        modified_borders_data.left.width = max(1, static_cast<float>(modified_borders_data.left.width.value()) * proportional_line_thickness);

        auto modified_rect = rect;

        // Outer border
        switch (edge) {
        case BorderEdge::Top:
            modified_rect.set_height(max(1, static_cast<float>(rect.height().value()) * proportional_line_thickness));
            break;
        case BorderEdge::Right:
            modified_rect.set_width(max(1, static_cast<float>(rect.width().value()) * proportional_line_thickness));
            modified_rect.set_right_without_resize(rect.right());
            break;
        case BorderEdge::Bottom:
            modified_rect.set_height(max(1, static_cast<float>(rect.height().value()) * proportional_line_thickness));
            modified_rect.set_bottom_without_resize(rect.bottom());
            break;
        case BorderEdge::Left:
            modified_rect.set_width(max(1, static_cast<float>(rect.width().value()) * proportional_line_thickness));
            break;
        }
        modified_borders_data.for_edge(edge).line_style = outer_style;
        paint_border(painter, edge, modified_rect, radius, opposite_radius, modified_borders_data, path, true);

        // Inner border, with smaller rect and radii
        Gfx::CornerRadius modified_radius = radius;
        Gfx::CornerRadius modified_opposite_radius = opposite_radius;
        switch (edge) {
        case BorderEdge::Top: {
            auto top_inset = borders_data.top.width - modified_borders_data.top.width;
            auto right_inset = borders_data.right.width - modified_borders_data.right.width;
            auto left_inset = borders_data.left.width - modified_borders_data.left.width;

            modified_radius.horizontal_radius = max(0, modified_radius.horizontal_radius - left_inset.value());
            modified_opposite_radius.horizontal_radius = max(0, modified_opposite_radius.horizontal_radius - right_inset.value());
            modified_radius.vertical_radius = max(0, modified_radius.vertical_radius - top_inset.value());
            modified_opposite_radius.vertical_radius = modified_radius.vertical_radius;

            modified_rect.set_bottom_without_resize(rect.bottom());
            // FIXME: Figure out the correct shrink amounts. This is only correct for some cases.
            if (modified_radius.horizontal_radius == 0 && modified_opposite_radius.horizontal_radius == 0)
                modified_rect.shrink(0, right_inset, 0, left_inset);
            break;
        }
        case BorderEdge::Right: {
            auto top_inset = borders_data.top.width - modified_borders_data.top.width;
            auto right_inset = borders_data.right.width - modified_borders_data.right.width;
            auto bottom_inset = borders_data.bottom.width - modified_borders_data.bottom.width;

            modified_radius.horizontal_radius = max(0, modified_radius.horizontal_radius - right_inset.value());
            modified_opposite_radius.horizontal_radius = modified_radius.horizontal_radius;
            modified_radius.vertical_radius = max(0, modified_radius.vertical_radius - top_inset.value());
            modified_opposite_radius.vertical_radius = max(0, modified_opposite_radius.vertical_radius - bottom_inset.value());

            modified_rect.set_left(rect.left());
            // FIXME: Figure out the correct shrink amounts. This is only correct for some cases.
            if (modified_radius.vertical_radius == 0 && modified_opposite_radius.vertical_radius == 0)
                modified_rect.shrink(top_inset, 0, bottom_inset, 0);
            break;
        }
        case BorderEdge::Bottom: {
            auto right_inset = borders_data.right.width - modified_borders_data.right.width;
            auto bottom_inset = borders_data.bottom.width - modified_borders_data.bottom.width;
            auto left_inset = borders_data.left.width - modified_borders_data.left.width;

            modified_radius.horizontal_radius = max(0, modified_radius.horizontal_radius - right_inset.value());
            modified_opposite_radius.horizontal_radius = max(0, modified_opposite_radius.horizontal_radius - left_inset.value());
            modified_radius.vertical_radius = max(0, modified_radius.vertical_radius - bottom_inset.value());
            modified_opposite_radius.vertical_radius = modified_radius.vertical_radius;

            modified_rect.set_top(rect.top());
            // FIXME: Figure out the correct shrink amounts. This is only correct for some cases.
            if (modified_radius.horizontal_radius == 0 && modified_opposite_radius.horizontal_radius == 0)
                modified_rect.shrink(0, right_inset, 0, left_inset);
            break;
        }
        case BorderEdge::Left: {
            auto top_inset = borders_data.top.width - modified_borders_data.top.width;
            auto bottom_inset = borders_data.bottom.width - modified_borders_data.bottom.width;
            auto left_inset = borders_data.left.width - modified_borders_data.left.width;

            modified_radius.horizontal_radius = max(0, modified_radius.horizontal_radius - left_inset.value());
            modified_opposite_radius.horizontal_radius = modified_radius.horizontal_radius;
            modified_radius.vertical_radius = max(0, modified_radius.vertical_radius - bottom_inset.value());
            modified_opposite_radius.vertical_radius = max(0, modified_opposite_radius.vertical_radius - top_inset.value());

            modified_rect.set_right_without_resize(rect.right());
            // FIXME: Figure out the correct shrink amounts. This is only correct for some cases.
            if (modified_radius.vertical_radius == 0 && modified_opposite_radius.vertical_radius == 0)
                modified_rect.shrink(top_inset, 0, bottom_inset, 0);
            break;
        }
        }

        modified_borders_data.for_edge(edge).line_style = inner_style;
        paint_border(painter, edge, modified_rect, modified_radius, modified_opposite_radius, modified_borders_data, path, true);
    };

    auto gfx_line_style = Gfx::LineStyle::Solid;
    switch (border_style) {
    case CSS::LineStyle::None:
    case CSS::LineStyle::Hidden:
        return;
    case CSS::LineStyle::Dotted:
        gfx_line_style = Gfx::LineStyle::Dotted;
        break;
    case CSS::LineStyle::Dashed:
        gfx_line_style = Gfx::LineStyle::Dashed;
        break;
    case CSS::LineStyle::Solid:
    case CSS::LineStyle::Inset:
    case CSS::LineStyle::Outset:
        // The only difference between Inset/Outset and Solid is the color, and we already handled that above.
        gfx_line_style = Gfx::LineStyle::Solid;
        break;
    case CSS::LineStyle::Double: {
        // Treat this as two solid borders, each 1/3 of the width.
        paint_double_border(1.0f / 3.0f, CSS::LineStyle::Solid, CSS::LineStyle::Solid);
        return;
    }
    case CSS::LineStyle::Groove:
    case CSS::LineStyle::Ridge:
        // Two half-width borders
        paint_double_border(0.5f,
            border_style == CSS::LineStyle::Groove ? CSS::LineStyle::Inset : CSS::LineStyle::Outset,
            border_style == CSS::LineStyle::Groove ? CSS::LineStyle::Outset : CSS::LineStyle::Inset);
        return;
    }

    if (gfx_line_style != Gfx::LineStyle::Solid) {
        paint_patterned_border(painter, edge, rect, radius, opposite_radius, borders_data, color, gfx_line_style);
        return;
    }

    auto draw_border = [&](Vector<Gfx::FloatPoint> const& points, bool joined_corner_has_inner_corner, bool opposite_joined_corner_has_inner_corner, Gfx::FloatSize joined_inner_corner_offset, Gfx::FloatSize opposite_joined_inner_corner_offset, bool ready_to_draw) {
        int current = 0;
        path.move_to(points[current++]);
        path.elliptical_arc_to(
            points[current++],
            Gfx::FloatSize(radius.horizontal_radius, radius.vertical_radius),
            0,
            false,
            false);
        path.line_to(points[current++]);
        if (joined_corner_has_inner_corner) {
            path.elliptical_arc_to(
                points[current++],
                Gfx::FloatSize(radius.horizontal_radius - joined_inner_corner_offset.width(), radius.vertical_radius - joined_inner_corner_offset.height()),
                0,
                false,
                true);
        }
        path.line_to(points[current++]);
        if (opposite_joined_corner_has_inner_corner) {
            path.elliptical_arc_to(
                points[current++],
                Gfx::FloatSize(opposite_radius.horizontal_radius - opposite_joined_inner_corner_offset.width(), opposite_radius.vertical_radius - opposite_joined_inner_corner_offset.height()),
                0,
                false,
                true);
        }
        path.line_to(points[current++]);
        path.elliptical_arc_to(
            points[current++],
            Gfx::FloatSize(opposite_radius.horizontal_radius, opposite_radius.vertical_radius),
            0,
            false,
            false);

        // If joined borders have the same color, combine them to draw together.
        if (ready_to_draw) {
            painter.fill_path({ .path = path, .paint_style_or_color = color, .winding_rule = Gfx::WindingRule::EvenOdd });
            path.clear();
        }
    };

    /**
     *   0 /-------------\ 7
     *    / /-----------\ \
     *   /-/ 3         4 \-\
     *  1  2             5  6
     * For each border edge, need to compute 8 points at most, then paint them as closed path.
     * 8 points are the most complicated case, it happens when the joined border width is not 0 and border radius larger than border width on both side.
     * If border radius is smaller than the border width, then the inner corner of the border corner is a right angle.
     */
    switch (edge) {
    case BorderEdge::Top: {
        auto joined_border_width = borders_data.left.width;
        auto opposite_joined_border_width = borders_data.right.width;
        bool joined_corner_has_inner_corner = border_data.width < radius.vertical_radius && joined_border_width < radius.horizontal_radius;
        bool opposite_joined_corner_has_inner_corner = border_data.width < opposite_radius.vertical_radius && opposite_joined_border_width < opposite_radius.horizontal_radius;

        Gfx::FloatPoint joined_corner_endpoint_offset;
        Gfx::FloatPoint opposite_joined_border_corner_offset;

        {
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, border_data.width.value(), joined_border_width.value());
            joined_corner_endpoint_offset = Gfx::FloatPoint(-midpoint.x(), radius.vertical_radius - midpoint.y());
        }

        {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, border_data.width.value(), opposite_joined_border_width.value());
            opposite_joined_border_corner_offset = Gfx::FloatPoint(midpoint.x(), opposite_radius.vertical_radius - midpoint.y());
        }

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            Gfx::FloatPoint midpoint = compute_midpoint(
                radius.horizontal_radius - joined_border_width.value(),
                radius.vertical_radius - border_data.width.value(),
                border_data.width.value(),
                joined_border_width.value());
            Gfx::FloatPoint inner_corner_endpoint_offset = Gfx::FloatPoint(
                -midpoint.x(),
                radius.vertical_radius - border_data.width.value() - midpoint.y());
            points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) + inner_corner_endpoint_offset);
            points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()));
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(
                joined_border_width.value() - radius.horizontal_radius,
                0);
            points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) + inner_right_angle_offset);
        }

        if (opposite_joined_corner_has_inner_corner) {
            Gfx::FloatPoint midpoint = compute_midpoint(
                opposite_radius.horizontal_radius - opposite_joined_border_width.value(),
                opposite_radius.vertical_radius - border_data.width.value(),
                border_data.width.value(),
                opposite_joined_border_width.value());
            Gfx::FloatPoint inner_corner_endpoint_offset = Gfx::FloatPoint(
                midpoint.x(),
                opposite_radius.vertical_radius - border_data.width.value() - midpoint.y());
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()));
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) + inner_corner_endpoint_offset);
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(
                opposite_joined_border_width.value() - opposite_radius.horizontal_radius,
                0);
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) - inner_right_angle_offset);
        }

        points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + opposite_joined_border_corner_offset);
        points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()));

        draw_border(
            points,
            joined_corner_has_inner_corner,
            opposite_joined_corner_has_inner_corner,
            Gfx::FloatSize(joined_border_width.value(), border_data.width.value()),
            Gfx::FloatSize(opposite_joined_border_width.value(), border_data.width.value()),
            last || color != border_color(BorderEdge::Right, borders_data));
        break;
    }
    case BorderEdge::Right: {
        auto joined_border_width = borders_data.top.width;
        auto opposite_joined_border_width = borders_data.bottom.width;
        bool joined_corner_has_inner_corner = border_data.width < radius.horizontal_radius && joined_border_width < radius.vertical_radius;
        bool opposite_joined_corner_has_inner_corner = border_data.width < opposite_radius.horizontal_radius && opposite_joined_border_width < opposite_radius.vertical_radius;

        Gfx::FloatPoint joined_corner_endpoint_offset;
        Gfx::FloatPoint opposite_joined_border_corner_offset;

        {
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, joined_border_width.value(), border_data.width.value());
            joined_corner_endpoint_offset = Gfx::FloatPoint(midpoint.x() - radius.horizontal_radius, -midpoint.y());
        }

        {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, opposite_joined_border_width.value(), border_data.width.value());
            opposite_joined_border_corner_offset = Gfx::FloatPoint(midpoint.x() - opposite_radius.horizontal_radius, midpoint.y());
        }

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                radius.horizontal_radius - border_data.width.value(),
                radius.vertical_radius - joined_border_width.value(),
                joined_border_width.value(),
                border_data.width.value());
            Gfx::FloatPoint inner_corner = Gfx::FloatPoint(
                -(radius.horizontal_radius - midpoint.x() - border_data.width.value()),
                -midpoint.y());
            points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + inner_corner);
            points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()));
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(0, joined_border_width.value() - radius.horizontal_radius);
            points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + inner_right_angle_offset);
        }

        if (opposite_joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                opposite_radius.horizontal_radius - border_data.width.value(),
                opposite_radius.vertical_radius - opposite_joined_border_width.value(),
                opposite_joined_border_width.value(),
                border_data.width.value());
            Gfx::FloatPoint inner_corner = Gfx::FloatPoint(
                -(opposite_radius.horizontal_radius - midpoint.x() - border_data.width.value()),
                midpoint.y());
            points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()));
            points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) + inner_corner);
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(0, opposite_joined_border_width.value() - opposite_radius.horizontal_radius);
            points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) - inner_right_angle_offset);
        }

        points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) + opposite_joined_border_corner_offset);
        points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()));

        draw_border(
            points,
            joined_corner_has_inner_corner,
            opposite_joined_corner_has_inner_corner,
            Gfx::FloatSize(border_data.width.value(), joined_border_width.value()),
            Gfx::FloatSize(border_data.width.value(), opposite_joined_border_width.value()),
            last || color != border_color(BorderEdge::Bottom, borders_data));
        break;
    }
    case BorderEdge::Bottom: {
        auto joined_border_width = borders_data.right.width;
        auto opposite_joined_border_width = borders_data.left.width;
        bool joined_corner_has_inner_corner = border_data.width < radius.vertical_radius && joined_border_width < radius.horizontal_radius;
        bool opposite_joined_corner_has_inner_corner = border_data.width < opposite_radius.vertical_radius && opposite_joined_border_width < opposite_radius.horizontal_radius;

        Gfx::FloatPoint joined_corner_endpoint_offset = [&] -> Gfx::FloatPoint {
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, border_data.width.value(), joined_border_width.value());
            return { midpoint.x(), midpoint.y() - radius.vertical_radius };
        }();
        Gfx::FloatPoint opposite_joined_border_corner_offset = [&] -> Gfx::FloatPoint {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, border_data.width.value(), opposite_joined_border_width.value());
            return { -midpoint.x(), midpoint.y() - opposite_radius.vertical_radius };
        }();

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                radius.horizontal_radius - joined_border_width.value(),
                radius.vertical_radius - border_data.width.value(),
                border_data.width.value(),
                joined_border_width.value());
            Gfx::FloatPoint inner_corner = Gfx::FloatPoint(midpoint.x(), -(radius.vertical_radius - midpoint.y() - border_data.width.value()));
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + inner_corner);
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()));
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(joined_border_width.value() - radius.horizontal_radius, 0);
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) - inner_right_angle_offset);
        }

        if (opposite_joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                opposite_radius.horizontal_radius - opposite_joined_border_width.value(),
                opposite_radius.vertical_radius - border_data.width.value(),
                border_data.width.value(),
                opposite_joined_border_width.value());
            Gfx::FloatPoint inner_corner = Gfx::FloatPoint(
                -midpoint.x(),
                -(opposite_radius.vertical_radius - midpoint.y() - border_data.width.value()));
            points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()));
            points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + inner_corner);
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(opposite_joined_border_width.value() - opposite_radius.horizontal_radius, 0);
            points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + inner_right_angle_offset);
        }

        points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) + opposite_joined_border_corner_offset);
        points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()));
        draw_border(
            points,
            joined_corner_has_inner_corner,
            opposite_joined_corner_has_inner_corner,
            Gfx::FloatSize(joined_border_width.value(), border_data.width.value()),
            Gfx::FloatSize(opposite_joined_border_width.value(), border_data.width.value()),
            last || color != border_color(BorderEdge::Left, borders_data));
        break;
    }
    case BorderEdge::Left: {
        auto joined_border_width = borders_data.bottom.width;
        auto opposite_joined_border_width = borders_data.top.width;
        bool joined_corner_has_inner_corner = border_data.width < radius.horizontal_radius && joined_border_width < radius.vertical_radius;
        bool opposite_joined_corner_has_inner_corner = border_data.width < opposite_radius.horizontal_radius && opposite_joined_border_width < opposite_radius.vertical_radius;

        Gfx::FloatPoint joined_corner_endpoint_offset = [&] -> Gfx::FloatPoint {
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, joined_border_width.value(), border_data.width.value());
            return { radius.horizontal_radius - midpoint.x(), midpoint.y() };
        }();
        Gfx::FloatPoint opposite_joined_border_corner_offset = [&] -> Gfx::FloatPoint {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, opposite_joined_border_width.value(), border_data.width.value());
            return { opposite_radius.horizontal_radius - midpoint.x(), -midpoint.y() };
        }();

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                radius.horizontal_radius - border_data.width.value(),
                radius.vertical_radius - joined_border_width.value(),
                joined_border_width.value(),
                border_data.width.value());
            Gfx::FloatPoint inner_corner = { radius.horizontal_radius - border_data.width.value() - midpoint.x(), midpoint.y() };
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) + inner_corner);
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()));
        } else {
            Gfx::FloatPoint inner_right_angle_offset = { 0, joined_border_width.value() - radius.vertical_radius };
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) - inner_right_angle_offset);
        }

        if (opposite_joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                opposite_radius.horizontal_radius - border_data.width.value(),
                opposite_radius.vertical_radius - opposite_joined_border_width.value(),
                opposite_joined_border_width.value(),
                border_data.width.value());
            Gfx::FloatPoint inner_corner = {
                opposite_radius.horizontal_radius - border_data.width.value() - midpoint.x(),
                -midpoint.y(),
            };
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()));
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + inner_corner);
        } else {
            Gfx::FloatPoint inner_right_angle_offset = { 0, opposite_joined_border_width.value() - opposite_radius.vertical_radius };
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + inner_right_angle_offset);
        }
        points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + opposite_joined_border_corner_offset);
        points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()));

        draw_border(
            points,
            joined_corner_has_inner_corner,
            opposite_joined_corner_has_inner_corner,
            Gfx::FloatSize(border_data.width.value(), joined_border_width.value()),
            Gfx::FloatSize(border_data.width.value(), opposite_joined_border_width.value()),
            last || color != border_color(BorderEdge::Top, borders_data));
        break;
    }
    }
}

// When every edge shares a width, color and style there is nothing to attribute to one edge or the other, so the whole
// border is stroked as a single closed centerline. Splitting it per edge instead would cut each corner dash in two, and
// two separately flattened and antialiased strokes never join cleanly.
static void paint_uniform_patterned_border(DisplayListRecorder& painter, DevicePixelRect const& border_rect, Gfx::CornerRadii const& corner_radii, BordersDataDevicePixels const& borders_data)
{
    auto width = static_cast<float>(borders_data.top.width.value());
    auto half_width = width / 2.f;

    auto left = static_cast<float>(border_rect.left().value()) + half_width;
    auto top = static_cast<float>(border_rect.top().value()) + half_width;
    auto right = static_cast<float>(border_rect.right().value()) - half_width;
    auto bottom = static_cast<float>(border_rect.bottom().value()) - half_width;

    auto centerline_radii = [&](Gfx::CornerRadius const& corner) {
        auto radii = Gfx::FloatSize(max(0.f, static_cast<float>(corner.horizontal_radius) - half_width), max(0.f, static_cast<float>(corner.vertical_radius) - half_width));
        if (radii.is_empty())
            return Gfx::FloatSize(0, 0);
        return radii;
    };
    auto top_left = centerline_radii(corner_radii.top_left);
    auto top_right = centerline_radii(corner_radii.top_right);
    auto bottom_right = centerline_radii(corner_radii.bottom_right);
    auto bottom_left = centerline_radii(corner_radii.bottom_left);

    // https://drafts.csswg.org/css-backgrounds-3/#overlapping-curves
    // Corner curves must not overlap: When the sum of any two adjacent border radii exceeds the size of the border
    // box, UAs must proportionally reduce the used values of all border radii until none of them overlap.
    // AD-HOC: The reduction has to run again for the centerline, because a radius narrower than half the border
    //         collapses to nothing there instead of shrinking along with the rest, which lets two curves that do fit
    //         the border box overlap.

    // The algorithm for reducing radii is as follows:
    // Let f = min(Li/Si), where i ∈ {top, right, bottom, left}, Si is the sum of the two corresponding radii of the
    // corners on side i, and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box.
    auto f = 1.f;
    auto ratio_for_side = [&](float sum_of_radii, float side_length) {
        if (sum_of_radii > side_length)
            f = min(f, side_length / sum_of_radii);
    };
    ratio_for_side(top_left.width() + top_right.width(), right - left);
    ratio_for_side(bottom_left.width() + bottom_right.width(), right - left);
    ratio_for_side(top_right.height() + bottom_right.height(), bottom - top);
    ratio_for_side(top_left.height() + bottom_left.height(), bottom - top);

    // If f < 1, then all corner radii are reduced by multiplying them by f.
    if (f < 1) {
        for (auto* corner : { &top_left, &top_right, &bottom_right, &bottom_left })
            corner->scale_by(f);
    }

    // Starting halfway along the top edge puts the seam on an axis of symmetry, so the pattern laid down in one
    // direction mirrors the one laid down in the other.
    auto start_x = clamp((left + right) / 2, left + top_left.width(), right - top_right.width());

    Gfx::Path centerline;
    centerline.move_to({ start_x, top });
    centerline.line_to({ right - top_right.width(), top });
    if (!top_right.is_empty())
        centerline.elliptical_arc_to({ right, top + top_right.height() }, top_right, 0, false, true);
    centerline.line_to({ right, bottom - bottom_right.height() });
    if (!bottom_right.is_empty())
        centerline.elliptical_arc_to({ right - bottom_right.width(), bottom }, bottom_right, 0, false, true);
    centerline.line_to({ left + bottom_left.width(), bottom });
    if (!bottom_left.is_empty())
        centerline.elliptical_arc_to({ left, bottom - bottom_left.height() }, bottom_left, 0, false, true);
    centerline.line_to({ left, top + top_left.height() });
    if (!top_left.is_empty())
        centerline.elliptical_arc_to({ left + top_left.width(), top }, top_left, 0, false, true);
    centerline.line_to({ start_x, top });
    centerline.close();

    auto length = centerline.length();

    Gfx::Path::CapStyle cap_style;
    Vector<float> dash_array;
    float dash_offset = 0;
    if (borders_data.top.line_style == CSS::LineStyle::Dotted) {
        // Spacing the dots a whole number of times around the loop and offsetting them by half an interval centers a
        // gap on the seam, leaving the wrap the same distance as every other gap.
        cap_style = Gfx::Path::CapStyle::Round;
        auto interval = length / max(1.f, roundf(length / (2 * width)));
        dash_array = { 0, interval };
        dash_offset = interval / 2;
    } else {
        // Tiling the loop with a whole number of dash-and-gap periods keeps the pattern continuous across the seam, and
        // offsetting it by a dash and a half centers a gap there, so nothing is drawn where the two ends meet at all.
        cap_style = Gfx::Path::CapStyle::Butt;
        auto interval = length / (2 * max(1.f, roundf(length / (4 * width))));
        dash_array = { interval, interval };
        dash_offset = interval * 1.5f;
    }

    painter.stroke_path({
        .cap_style = cap_style,
        .join_style = Gfx::Path::JoinStyle::Miter,
        .miter_limit = 4,
        .dash_array = move(dash_array),
        .dash_offset = dash_offset,
        .path = move(centerline),
        .paint_style_or_color = borders_data.top.color,
        .thickness = width,
    });
}

void paint_all_borders(DisplayListRecorder& painter, DevicePixelRect const& border_rect, Gfx::CornerRadii const& corner_radii, BordersDataDevicePixels const& borders_data)
{
    if (borders_data.top.width <= 0 && borders_data.right.width <= 0 && borders_data.left.width <= 0 && borders_data.bottom.width <= 0)
        return;

    auto line_style = borders_data.top.line_style;
    if ((line_style == CSS::LineStyle::Dashed || line_style == CSS::LineStyle::Dotted) && borders_data.all_are_equal()) {
        paint_uniform_patterned_border(painter, border_rect, corner_radii, borders_data);
        return;
    }

    auto top_left = corner_radii.top_left;
    auto top_right = corner_radii.top_right;
    auto bottom_right = corner_radii.bottom_right;
    auto bottom_left = corner_radii.bottom_left;

    // Disable border radii if the corresponding borders don't exist:
    if (borders_data.bottom.width <= 0 && borders_data.left.width <= 0)
        bottom_left = { 0, 0 };
    if (borders_data.bottom.width <= 0 && borders_data.right.width <= 0)
        bottom_right = { 0, 0 };
    if (borders_data.top.width <= 0 && borders_data.left.width <= 0)
        top_left = { 0, 0 };
    if (borders_data.top.width <= 0 && borders_data.right.width <= 0)
        top_right = { 0, 0 };

    DevicePixelRect top_border_rect = {
        border_rect.x() + top_left.horizontal_radius,
        border_rect.y(),
        border_rect.width() - top_left.horizontal_radius - top_right.horizontal_radius,
        borders_data.top.width
    };
    DevicePixelRect right_border_rect = {
        border_rect.x() + (border_rect.width() - borders_data.right.width),
        border_rect.y() + top_right.vertical_radius,
        borders_data.right.width,
        border_rect.height() - top_right.vertical_radius - bottom_right.vertical_radius
    };
    DevicePixelRect bottom_border_rect = {
        border_rect.x() + bottom_left.horizontal_radius,
        border_rect.y() + (border_rect.height() - borders_data.bottom.width),
        border_rect.width() - bottom_left.horizontal_radius - bottom_right.horizontal_radius,
        borders_data.bottom.width
    };
    DevicePixelRect left_border_rect = {
        border_rect.x(),
        border_rect.y() + top_left.vertical_radius,
        borders_data.left.width,
        border_rect.height() - top_left.vertical_radius - bottom_left.vertical_radius
    };

    static constexpr Array edges { BorderEdge::Top, BorderEdge::Right, BorderEdge::Bottom, BorderEdge::Left };

    // Find the first border that has a different color than the previous one, then start painting from that border.
    size_t start_index = 0;
    for (size_t i = 0; i < edges.size(); i++) {
        if (border_color(edges[i], borders_data) != border_color(edges[(i + 1) % edges.size()], borders_data)) {
            start_index = (i + 1) % edges.size();
            break;
        }
    }

    Gfx::Path path;
    for (size_t i = 0; i < edges.size(); i++) {
        auto last = i == edges.size() - 1;
        switch (edges[(start_index + i) % edges.size()]) {
        case BorderEdge::Top:
            paint_border(painter, BorderEdge::Top, top_border_rect, top_left, top_right, borders_data, path, last);
            break;
        case BorderEdge::Right:
            paint_border(painter, BorderEdge::Right, right_border_rect, top_right, bottom_right, borders_data, path, last);
            break;
        case BorderEdge::Bottom:
            paint_border(painter, BorderEdge::Bottom, bottom_border_rect, bottom_right, bottom_left, borders_data, path, last);
            break;
        case BorderEdge::Left:
            paint_border(painter, BorderEdge::Left, left_border_rect, bottom_left, top_left, borders_data, path, last);
            break;
        }
    }
}

Optional<BordersData> borders_data_for_outline(Layout::Node const& layout_node, Color outline_color, CSS::OutlineStyle outline_style, CSSPixels outline_width)
{
    CSS::LineStyle line_style;
    if (outline_style == CSS::OutlineStyle::Auto) {
        // `auto` lets us do whatever we want for the outline. 2px of the accent colour seems reasonable.
        line_style = CSS::LineStyle::Solid;
        outline_color = CSS::KeywordStyleValue::create(CSS::Keyword::Accentcolor)->to_color(CSS::ColorResolutionContext::for_layout_node_with_style(*static_cast<Layout::NodeWithStyle const*>(&layout_node))).value();
        outline_width = 2;
    } else {
        line_style = CSS::keyword_to_line_style(CSS::to_keyword(outline_style)).value_or(CSS::LineStyle::None);
    }

    if (outline_color.alpha() == 0 || line_style == CSS::LineStyle::None || outline_width == 0)
        return {};

    CSS::BorderData border_data {
        .color = outline_color,
        .line_style = line_style,
        .width = outline_width,
    };
    return BordersData { border_data, border_data, border_data, border_data };
}

}
