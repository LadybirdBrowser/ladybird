/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CircularQueue.h>
#include <LibWeb/CSS/StyleValues/CSSKeywordValue.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/DisplayListRecorder.h>

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

Gfx::Color BorderPainter::border_color_for_edge(BorderEdge edge) const
{
    auto border_data = border_data_for_edge(edge);

    if (border_data.line_style == CSS::LineStyle::Inset) {
        auto top_left_color = dark_color_for_inset_and_outset(border_data.color);
        auto bottom_right_color = light_color_for_inset_and_outset(border_data.color);
        return (edge == BorderEdge::Left || edge == BorderEdge::Top) ? top_left_color : bottom_right_color;
    }

    if (border_data.line_style == CSS::LineStyle::Outset) {
        auto top_left_color = light_color_for_inset_and_outset(border_data.color);
        auto bottom_right_color = dark_color_for_inset_and_outset(border_data.color);
        return (edge == BorderEdge::Left || edge == BorderEdge::Top) ? top_left_color : bottom_right_color;
    }

    return border_data.color;
}

BorderDataDevicePixels BorderPainter::border_data_for_edge(BorderEdge edge) const
{
    switch (edge) {
    case BorderEdge::Top:
        return m_borders_data.top;
    case BorderEdge::Right:
        return m_borders_data.right;
    case BorderEdge::Bottom:
        return m_borders_data.bottom;
    default: // BorderEdge::Left:
        return m_borders_data.left;
    }
}

void BorderPainter::paint_border(BorderEdge edge, DevicePixelRect const& rect, CornerRadius const& radius, CornerRadius const& opposite_radius, bool last)
{
    auto border_data = border_data_for_edge(edge);
    if (border_data.width <= 0)
        return;

    auto border_style = border_data.line_style;

    switch (border_style) {
    case CSS::LineStyle::None:
    case CSS::LineStyle::Hidden:
        return;
    case CSS::LineStyle::Dotted:
        paint_simple_border(edge, rect, border_data, Gfx::LineStyle::Dotted);
        return;
    case CSS::LineStyle::Dashed:
        paint_simple_border(edge, rect, border_data, Gfx::LineStyle::Dashed);
        return;
    case CSS::LineStyle::Solid:
    case CSS::LineStyle::Inset:
    case CSS::LineStyle::Outset:
        paint_joined_border(edge, rect, border_data, radius, opposite_radius, last);
        return;
    case CSS::LineStyle::Double:
    case CSS::LineStyle::Groove:
    case CSS::LineStyle::Ridge:
        // FIXME: Implement these
        break;
    }
    paint_joined_border(edge, rect, border_data, radius, opposite_radius, last);
}

void BorderPainter::paint_simple_border(BorderEdge edge, DevicePixelRect const& rect, BorderDataDevicePixels const& border_data, Gfx::LineStyle gfx_line_style)
{
    auto color = border_color_for_edge(edge);

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
    m_painter.draw_line(p1.to_type<int>(), p2.to_type<int>(), color, border_data.width.value(), gfx_line_style);
}

void BorderPainter::paint_joined_border(BorderEdge edge, DevicePixelRect const& rect, BorderDataDevicePixels const& border_data, CornerRadius const& radius, CornerRadius const& opposite_radius, bool last)
{
    auto color = border_color_for_edge(edge);
    auto draw_border = [&](Vector<Gfx::FloatPoint> const& points, bool joined_corner_has_inner_corner, bool opposite_joined_corner_has_inner_corner, Gfx::FloatSize joined_inner_corner_offset, Gfx::FloatSize opposite_joined_inner_corner_offset, bool ready_to_draw) {
        int current = 0;
        m_path.move_to(points[current++]);
        m_path.elliptical_arc_to(
            points[current++],
            Gfx::FloatSize(radius.horizontal_radius, radius.vertical_radius),
            0,
            false,
            false);
        m_path.line_to(points[current++]);
        if (joined_corner_has_inner_corner) {
            m_path.elliptical_arc_to(
                points[current++],
                Gfx::FloatSize(radius.horizontal_radius - joined_inner_corner_offset.width(), radius.vertical_radius - joined_inner_corner_offset.height()),
                0,
                false,
                true);
        }
        m_path.line_to(points[current++]);
        if (opposite_joined_corner_has_inner_corner) {
            m_path.elliptical_arc_to(
                points[current++],
                Gfx::FloatSize(opposite_radius.horizontal_radius - opposite_joined_inner_corner_offset.width(), opposite_radius.vertical_radius - opposite_joined_inner_corner_offset.height()),
                0,
                false,
                true);
        }
        m_path.line_to(points[current++]);
        m_path.elliptical_arc_to(
            points[current++],
            Gfx::FloatSize(opposite_radius.horizontal_radius, opposite_radius.vertical_radius),
            0,
            false,
            false);

        // If joined borders have the same color, combine them to draw together.
        if (ready_to_draw) {
            m_path.close_all_subpaths();
            m_painter.fill_path({ .path = m_path,
                .color = color,
                .winding_rule = Gfx::WindingRule::EvenOdd });
            m_path.clear();
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
        auto joined_border_width = m_borders_data.left.width;
        auto opposite_joined_border_width = m_borders_data.right.width;
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
            last || color != border_color_for_edge(BorderEdge::Right));
        break;
    }
    case BorderEdge::Right: {
        auto joined_border_width = m_borders_data.top.width;
        auto opposite_joined_border_width = m_borders_data.bottom.width;
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
            last || color != border_color_for_edge(BorderEdge::Bottom));
        break;
    }
    case BorderEdge::Bottom: {
        auto joined_border_width = m_borders_data.right.width;
        auto opposite_joined_border_width = m_borders_data.left.width;
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
            last || color != border_color_for_edge(BorderEdge::Left));
        break;
    }
    case BorderEdge::Left: {
        auto joined_border_width = m_borders_data.bottom.width;
        auto opposite_joined_border_width = m_borders_data.top.width;
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
            last || color != border_color_for_edge(BorderEdge::Top));
        break;
    }
    }
}

void paint_all_borders(DisplayListRecorder& painter, DevicePixelRect const& border_rect, CornerRadii const& corner_radii, BordersDataDevicePixels const& borders_data)
{
    if (borders_data.top.width <= 0 && borders_data.right.width <= 0 && borders_data.left.width <= 0 && borders_data.bottom.width <= 0)
        return;

    BorderPainter border_painter(painter, border_rect, corner_radii, borders_data);

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
        if (border_painter.border_color_for_edge(borders.at(0)) != border_painter.border_color_for_edge(borders.at(1))) {
            borders.enqueue(borders.dequeue());
            break;
        }

        borders.enqueue(borders.dequeue());
    }

    for (BorderEdge edge : borders) {
        switch (edge) {
        case BorderEdge::Top:
            border_painter.paint_border(BorderEdge::Top, top_border_rect, top_left, top_right, edge == borders.last());
            break;
        case BorderEdge::Right:
            border_painter.paint_border(BorderEdge::Right, right_border_rect, top_right, bottom_right, edge == borders.last());
            break;
        case BorderEdge::Bottom:
            border_painter.paint_border(BorderEdge::Bottom, bottom_border_rect, bottom_right, bottom_left, edge == borders.last());
            break;
        case BorderEdge::Left:
            border_painter.paint_border(BorderEdge::Left, left_border_rect, bottom_left, top_left, edge == borders.last());
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
        outline_color = CSS::CSSKeywordValue::create(CSS::Keyword::Linktext)->to_color(*static_cast<Layout::NodeWithStyle const*>(&layout_node));
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
