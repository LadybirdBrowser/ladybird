/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CircularQueue.h>
#include <LibGfx/AntiAliasingPainter.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>

namespace Web::Painting {

static constexpr double dark_light_absolute_value_difference = 1. / 3;

static Color light_color_for_inset_and_outset(Color const& color)
{
    auto hsv = color.to_hsv();
    if (hsv.value >= dark_light_absolute_value_difference)
        return Color::from_hsv(hsv);
    return Color::from_hsv({ hsv.hue, hsv.saturation, hsv.value + dark_light_absolute_value_difference });
}

static Color dark_color_for_inset_and_outset(Color const& color)
{
    auto hsv = color.to_hsv();
    if (hsv.value < dark_light_absolute_value_difference)
        return Color::from_hsv(hsv);
    return Color::from_hsv({ hsv.hue, hsv.saturation, hsv.value - dark_light_absolute_value_difference });
}

Gfx::Color border_color(BorderEdge edge, BordersDataDevicePixels const& borders_data)
{
    auto const& border_data = [&] {
        switch (edge) {
        case BorderEdge::Top:
            return borders_data.top;
        case BorderEdge::Right:
            return borders_data.right;
        case BorderEdge::Bottom:
            return borders_data.bottom;
        case BorderEdge::Left:
            return borders_data.left;
        default:
            VERIFY_NOT_REACHED();
        }
    }();

    if (border_data.line_style == CSS::LineStyle::Inset) {
        auto top_left_color = dark_color_for_inset_and_outset(border_data.color);
        auto bottom_right_color = light_color_for_inset_and_outset(border_data.color);
        return (edge == BorderEdge::Left || edge == BorderEdge::Top) ? top_left_color : bottom_right_color;
    } else if (border_data.line_style == CSS::LineStyle::Outset) {
        auto top_left_color = light_color_for_inset_and_outset(border_data.color);
        auto bottom_right_color = dark_color_for_inset_and_outset(border_data.color);
        return (edge == BorderEdge::Left || edge == BorderEdge::Top) ? top_left_color : bottom_right_color;
    }

    return border_data.color;
}

void paint_border(DisplayListRecorder& painter, BorderEdge edge, DevicePixelRect const& rect, Gfx::CornerRadius const& radius, Gfx::CornerRadius const& opposite_radius, BordersDataDevicePixels const& borders_data, Gfx::Path& path, bool last)
{
    auto const& border_data = [&] {
        switch (edge) {
        case BorderEdge::Top:
            return borders_data.top;
        case BorderEdge::Right:
            return borders_data.right;
        case BorderEdge::Bottom:
            return borders_data.bottom;
        default: // BorderEdge::Left:
            return borders_data.left;
        }
    }();

    if (border_data.width <= 0)
        return;

    auto color = border_color(edge, borders_data);
    auto border_style = border_data.line_style;

    struct Points {
        DevicePixelPoint p1;
        DevicePixelPoint p2;
    };

    auto points_for_edge = [](BorderEdge edge, DevicePixelRect const& rect) -> Points {
        switch (edge) {
        case BorderEdge::Top:
            return { rect.top_left(), rect.top_right().moved_left(1) };
        case BorderEdge::Right:
            return { rect.top_right().moved_left(1), rect.bottom_right().translated(-1) };
        case BorderEdge::Bottom:
            return { rect.bottom_left().moved_up(1), rect.bottom_right().translated(-1) };
        default: // Edge::Left
            return { rect.top_left(), rect.bottom_left().moved_up(1) };
        }
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
        gfx_line_style = Gfx::LineStyle::Solid;
        break;
    case CSS::LineStyle::Double:
    case CSS::LineStyle::Groove:
    case CSS::LineStyle::Ridge:
    case CSS::LineStyle::Inset:
    case CSS::LineStyle::Outset:
        // FIXME: Implement these
        break;
    }

    if (gfx_line_style != Gfx::LineStyle::Solid) {
        auto [p1, p2] = points_for_edge(edge, rect);
        switch (edge) {
        case BorderEdge::Top:
            p1.translate_by(border_data.width / 2, border_data.width / 2);
            p2.translate_by(-border_data.width / 2, border_data.width / 2);
            break;
        case BorderEdge::Right:
            p1.translate_by(-border_data.width / 2, border_data.width / 2);
            p2.translate_by(-border_data.width / 2, -border_data.width / 2);
            break;
        case BorderEdge::Bottom:
            p1.translate_by(border_data.width / 2, -border_data.width / 2);
            p2.translate_by(-border_data.width / 2, -border_data.width / 2);
            break;
        case BorderEdge::Left:
            p1.translate_by(border_data.width / 2, border_data.width / 2);
            p2.translate_by(border_data.width / 2, -border_data.width / 2);
            break;
        }
        if (border_style == CSS::LineStyle::Dotted) {
            painter.draw_line(p1.to_type<int>(), p2.to_type<int>(), color, border_data.width.value(), gfx_line_style);
            return;
        }
        painter.draw_line(p1.to_type<int>(), p2.to_type<int>(), color, border_data.width.value(), gfx_line_style);
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
            path.close_all_subpaths();
            painter.fill_path({ .path = path,
                .color = color,
                .winding_rule = Gfx::WindingRule::EvenOdd });
            path.clear();
        }
    };

    auto compute_midpoint = [&](int horizontal_radius, int vertical_radius, int joined_border_width) {
        if (horizontal_radius == 0 && vertical_radius == 0) {
            return Gfx::FloatPoint(0, 0);
        }
        if (joined_border_width == 0) {
            switch (edge) {
            case BorderEdge::Top:
            case BorderEdge::Bottom:
                return Gfx::FloatPoint(horizontal_radius, 0);
            case BorderEdge::Right:
            case BorderEdge::Left:
                return Gfx::FloatPoint(0, vertical_radius);
            default:
                VERIFY_NOT_REACHED();
            }
        }
        // FIXME: this middle point rule seems not exacly the same as main browsers
        // compute the midpoint based on point whose tangent slope of 1
        // https://math.stackexchange.com/questions/3325134/find-the-points-on-the-ellipse-where-the-slope-of-the-tangent-line-is-1
        return Gfx::FloatPoint(
            (horizontal_radius * horizontal_radius) / AK::sqrt(1.0f * horizontal_radius * horizontal_radius + vertical_radius * vertical_radius),
            (vertical_radius * vertical_radius) / AK::sqrt(1.0f * horizontal_radius * horizontal_radius + vertical_radius * vertical_radius));
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
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, joined_border_width.value());
            joined_corner_endpoint_offset = Gfx::FloatPoint(-midpoint.x(), radius.vertical_radius - midpoint.y());
        }

        {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, opposite_joined_border_width.value());
            opposite_joined_border_corner_offset = Gfx::FloatPoint(midpoint.x(), opposite_radius.vertical_radius - midpoint.y());
        }

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.top_left().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            Gfx::FloatPoint midpoint = compute_midpoint(
                radius.horizontal_radius - joined_border_width.value(),
                radius.vertical_radius - border_data.width.value(),
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
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, joined_border_width.value());
            joined_corner_endpoint_offset = Gfx::FloatPoint(midpoint.x() - radius.horizontal_radius, -midpoint.y());
        }

        {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, opposite_joined_border_width.value());
            opposite_joined_border_corner_offset = Gfx::FloatPoint(midpoint.x() - opposite_radius.horizontal_radius, midpoint.y());
        }

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                radius.horizontal_radius - border_data.width.value(),
                radius.vertical_radius - joined_border_width.value(),
                joined_border_width.value());
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
                opposite_joined_border_width.value());
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

        Gfx::FloatPoint joined_corner_endpoint_offset;
        Gfx::FloatPoint opposite_joined_border_corner_offset;

        {
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, joined_border_width.value());
            joined_corner_endpoint_offset = Gfx::FloatPoint(midpoint.x(), midpoint.y() - radius.vertical_radius);
        }

        {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, opposite_joined_border_width.value());
            opposite_joined_border_corner_offset = Gfx::FloatPoint(-midpoint.x(), midpoint.y() - opposite_radius.vertical_radius);
        }

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                radius.horizontal_radius - joined_border_width.value(),
                radius.vertical_radius - border_data.width.value(),
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

        Gfx::FloatPoint joined_corner_endpoint_offset;
        Gfx::FloatPoint opposite_joined_border_corner_offset;

        {
            auto midpoint = compute_midpoint(radius.horizontal_radius, radius.vertical_radius, joined_border_width.value());
            joined_corner_endpoint_offset = Gfx::FloatPoint(radius.horizontal_radius - midpoint.x(), midpoint.y());
        }

        {
            auto midpoint = compute_midpoint(opposite_radius.horizontal_radius, opposite_radius.vertical_radius, opposite_joined_border_width.value());
            opposite_joined_border_corner_offset = Gfx::FloatPoint(opposite_radius.horizontal_radius - midpoint.x(), -midpoint.y());
        }

        Vector<Gfx::FloatPoint, 8> points;
        points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()));
        points.append(Gfx::FloatPoint(rect.bottom_left().to_type<int>()) + joined_corner_endpoint_offset);

        if (joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                radius.horizontal_radius - border_data.width.value(),
                radius.vertical_radius - joined_border_width.value(),
                joined_border_width.value());
            Gfx::FloatPoint inner_corner = Gfx::FloatPoint(radius.horizontal_radius - border_data.width.value() - midpoint.x(), midpoint.y());
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) + inner_corner);
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()));
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(0, joined_border_width.value() - radius.vertical_radius);
            points.append(Gfx::FloatPoint(rect.bottom_right().to_type<int>()) - inner_right_angle_offset);
        }

        if (opposite_joined_corner_has_inner_corner) {
            auto midpoint = compute_midpoint(
                opposite_radius.horizontal_radius - border_data.width.value(),
                opposite_radius.vertical_radius - opposite_joined_border_width.value(),
                opposite_joined_border_width.value());
            Gfx::FloatPoint inner_corner = Gfx::FloatPoint(
                opposite_radius.horizontal_radius - border_data.width.value() - midpoint.x(),
                -midpoint.y());
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()));
            points.append(Gfx::FloatPoint(rect.top_right().to_type<int>()) + inner_corner);
        } else {
            Gfx::FloatPoint inner_right_angle_offset = Gfx::FloatPoint(0, opposite_joined_border_width.value() - opposite_radius.vertical_radius);
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

void paint_all_borders(DisplayListRecorder& painter, DevicePixelRect const& border_rect, CornerRadii const& corner_radii, BordersDataDevicePixels const& borders_data)
{
    if (borders_data.top.width <= 0 && borders_data.right.width <= 0 && borders_data.left.width <= 0 && borders_data.bottom.width <= 0)
        return;

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

    AK::CircularQueue<BorderEdge, 4> borders;
    borders.enqueue(BorderEdge::Top);
    borders.enqueue(BorderEdge::Right);
    borders.enqueue(BorderEdge::Bottom);
    borders.enqueue(BorderEdge::Left);

    // Try to find the first border that has a different color than the previous one,
    // then start painting from that border.
    for (size_t i = 0; i < borders.size(); i++) {
        if (border_color(borders.at(0), borders_data) != border_color(borders.at(1), borders_data)) {
            borders.enqueue(borders.dequeue());
            break;
        }

        borders.enqueue(borders.dequeue());
    }

    Gfx::Path path;
    for (BorderEdge edge : borders) {
        switch (edge) {
        case BorderEdge::Top:
            paint_border(painter, BorderEdge::Top, top_border_rect, top_left, top_right, borders_data, path, edge == borders.last());
            break;
        case BorderEdge::Right:
            paint_border(painter, BorderEdge::Right, right_border_rect, top_right, bottom_right, borders_data, path, edge == borders.last());
            break;
        case BorderEdge::Bottom:
            paint_border(painter, BorderEdge::Bottom, bottom_border_rect, bottom_right, bottom_left, borders_data, path, edge == borders.last());
            break;
        case BorderEdge::Left:
            paint_border(painter, BorderEdge::Left, left_border_rect, bottom_left, top_left, borders_data, path, edge == borders.last());
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }
}

Optional<BordersData> borders_data_for_outline(Layout::Node const& layout_node, Color outline_color, CSS::OutlineStyle outline_style, CSSPixels outline_width)
{
    CSS::LineStyle line_style;
    if (outline_style == CSS::OutlineStyle::Auto) {
        // `auto` lets us do whatever we want for the outline. 2px of the link colour seems reasonable.
        line_style = CSS::LineStyle::Dotted;
        outline_color = layout_node.document().normal_link_color();
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
