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

// https://drafts.csswg.org/css-backgrounds-3/#border-style
// There is no control over the spacing of the dots and dashes, nor over the length of the dashes. Implementations are
// encouraged to choose a spacing that makes the corners symmetrical.
// NB: The proportions themselves are ours: a dot spans the border width and is followed by a gap of the same size,
//     while a dash spans twice the border width and is followed by a gap of that same doubled size.
static float pattern_period(float width, bool dotted)
{
    return (dotted ? 2.f : 4.f) * width;
}

// AD-HOC: Other browsers abandon the pattern and draw a solid line along a side with no room for more than a single
//         period, which is what keeps a border-only CSS triangle from breaking up into stray marks. They disagree on
//         a side of exactly one period: Chrome draws it solid, Firefox draws the pattern. We follow Chrome, because
//         that also settles the dotted case, where a side of exactly one period is what a CSS triangle comes out as
//         and where Firefox draws nothing at all.
static bool is_long_enough_for_pattern(float length, float width, bool dotted)
{
    return length > pattern_period(width, dotted);
}

// The pattern is stretched to fit the path a whole number of times, and offset so that it leaves the ends of an open
// path, and the seam of a closed one, in a gap.
static void stroke_patterned_path(DisplayListRecorder& painter, Gfx::Path path, Gfx::LineStyle line_style, float width, Color color, bool path_is_closed)
{
    auto length = path.length();
    auto dotted = line_style == Gfx::LineStyle::Dotted;
    auto periods = max(1.f, roundf(length / pattern_period(width, dotted)));
    auto interval = dotted ? length / periods : length / (2 * periods);

    painter.stroke_path({
        .cap_style = dotted ? Gfx::Path::CapStyle::Round : Gfx::Path::CapStyle::Butt,
        .join_style = Gfx::Path::JoinStyle::Miter,
        .miter_limit = 4,
        .dash_array = dotted ? Vector<float> { 0, interval } : Vector<float> { interval, interval },
        .dash_offset = path_is_closed && !dotted ? interval * 1.5f : interval / 2,
        .path = move(path),
        .paint_style_or_color = color,
        .thickness = width,
    });
}

// Dashes and dots run along the middle of the border, so instead of filling the exact region covered by the edge -
// which is what the solid border painting below does - they are stroked along a centerline path: the half of the
// corner arc leading into the edge, the straight run, and the half of the corner arc leading out of it.
static Gfx::Path patterned_border_centerline(BorderEdge edge, DevicePixelRect const& rect,
    Gfx::CornerRadius const& radius, Gfx::CornerRadius const& opposite_radius,
    BordersDataDevicePixels const& borders_data)
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

    return centerline;
}

void paint_border(DisplayListRecorder& painter, BorderEdge edge, DevicePixelRect const& rect, Gfx::CornerRadius const& radius, Gfx::CornerRadius const& opposite_radius, BordersDataDevicePixels const& borders_data, Gfx::Path& path, bool last)
{
    auto const& border_data = borders_data.for_edge(edge);

    if (border_data.width <= 0)
        return;

    auto color = border_color(edge, borders_data);
    auto border_style = border_data.line_style;

    // Edges sharing a color are collected into one path and only filled once the color changes or the last edge is
    // reached. Anything that paints in a color of its own has to empty that queue before it does.
    auto flush_queued_edges = [&] {
        if (path.is_empty())
            return;
        painter.fill_path({ .path = path, .paint_style_or_color = color, .winding_rule = Gfx::WindingRule::EvenOdd });
        path.clear();
    };

    struct Frame {
        Gfx::FloatPoint along;
        Gfx::FloatPoint into;
        DevicePixelPoint start_outer;
        DevicePixelPoint start_inner;
        DevicePixelPoint end_inner;
        DevicePixelPoint end_outer;
        BorderEdge joined_edge;
        BorderEdge opposite_joined_edge;
    };

    auto is_horizontal_edge = edge == BorderEdge::Top || edge == BorderEdge::Bottom;

    Frame frame;
    switch (edge) {
    case BorderEdge::Top:
        frame = { { 1, 0 }, { 0, 1 }, rect.top_left(), rect.bottom_left(), rect.bottom_right(), rect.top_right(), BorderEdge::Left, BorderEdge::Right };
        break;
    case BorderEdge::Right:
        frame = { { 0, 1 }, { -1, 0 }, rect.top_right(), rect.top_left(), rect.bottom_left(), rect.bottom_right(), BorderEdge::Top, BorderEdge::Bottom };
        break;
    case BorderEdge::Bottom:
        frame = { { -1, 0 }, { 0, -1 }, rect.bottom_right(), rect.top_right(), rect.top_left(), rect.bottom_left(), BorderEdge::Right, BorderEdge::Left };
        break;
    case BorderEdge::Left:
        frame = { { 0, -1 }, { 1, 0 }, rect.bottom_left(), rect.bottom_right(), rect.top_right(), rect.top_left(), BorderEdge::Bottom, BorderEdge::Top };
        break;
    }

    auto joined_border_width = borders_data.for_edge(frame.joined_edge).width;
    auto opposite_joined_border_width = borders_data.for_edge(frame.opposite_joined_edge).width;

    auto paint_double_border = [&](float proportional_line_thickness, CSS::LineStyle outer_style, CSS::LineStyle inner_style) {
        // Each half is painted as a border of its own, deriving its own color from the style it is given.
        flush_queued_edges();

        // AD-HOC: We clamp the individual borders to 1px thick if they're less so that they don't disappear entirely.
        //         This matches other browsers and is allowable per the spec, where the thickness is implementation-defined.

        // FIXME: Converting to floats and back is awkward, can we somehow do all this processing using CSSPixels?
        auto modified_borders_data = borders_data;
        modified_borders_data.top.width = max(1, static_cast<float>(modified_borders_data.top.width.value()) * proportional_line_thickness);
        modified_borders_data.right.width = max(1, static_cast<float>(modified_borders_data.right.width.value()) * proportional_line_thickness);
        modified_borders_data.bottom.width = max(1, static_cast<float>(modified_borders_data.bottom.width.value()) * proportional_line_thickness);
        modified_borders_data.left.width = max(1, static_cast<float>(modified_borders_data.left.width.value()) * proportional_line_thickness);

        auto modified_rect = rect;

        // Outer border, scaled back towards the outer edge of the rect
        if (is_horizontal_edge)
            modified_rect.set_height(max(1, static_cast<float>(rect.height().value()) * proportional_line_thickness));
        else
            modified_rect.set_width(max(1, static_cast<float>(rect.width().value()) * proportional_line_thickness));
        if (edge == BorderEdge::Right)
            modified_rect.set_right_without_resize(rect.right());
        else if (edge == BorderEdge::Bottom)
            modified_rect.set_bottom_without_resize(rect.bottom());
        modified_borders_data.for_edge(edge).line_style = outer_style;
        paint_border(painter, edge, modified_rect, radius, opposite_radius, modified_borders_data, path, true);

        // Inner border, with each radius pulled in by the inset of the border that spans it, and reaching back to
        // where the original inner edge was.
        auto inset_of = [&](BorderEdge inset_edge) {
            return (borders_data.for_edge(inset_edge).width - modified_borders_data.for_edge(inset_edge).width).value();
        };

        Gfx::CornerRadius modified_radius = radius;
        Gfx::CornerRadius modified_opposite_radius = opposite_radius;
        auto& along_start = is_horizontal_edge ? modified_radius.horizontal_radius : modified_radius.vertical_radius;
        auto& along_end = is_horizontal_edge ? modified_opposite_radius.horizontal_radius : modified_opposite_radius.vertical_radius;
        auto& into_start = is_horizontal_edge ? modified_radius.vertical_radius : modified_radius.horizontal_radius;
        auto& into_end = is_horizontal_edge ? modified_opposite_radius.vertical_radius : modified_opposite_radius.horizontal_radius;
        auto outer_along_start = along_start;
        auto outer_along_end = along_end;
        along_start = max(0, along_start - inset_of(frame.joined_edge));
        along_end = max(0, along_end - inset_of(frame.opposite_joined_edge));
        into_start = max(0, into_start - inset_of(edge));
        into_end = max(0, into_end - inset_of(edge));

        switch (edge) {
        case BorderEdge::Top:
            modified_rect.set_bottom_without_resize(rect.bottom());
            break;
        case BorderEdge::Right:
            modified_rect.set_left(rect.left());
            break;
        case BorderEdge::Bottom:
            modified_rect.set_top(rect.top());
            break;
        case BorderEdge::Left:
            modified_rect.set_right_without_resize(rect.right());
            break;
        }

        // The straight run of an edge starts where its corner curve ends, so the inner border only begins further
        // along than the outer one where its curve has been reduced past the inset that moved it inwards.
        auto start_shrink = max(0, inset_of(frame.joined_edge) - outer_along_start);
        auto end_shrink = max(0, inset_of(frame.opposite_joined_edge) - outer_along_end);
        if (is_horizontal_edge) {
            auto rightwards = frame.along.x() > 0;
            modified_rect.shrink(0, rightwards ? end_shrink : start_shrink, 0, rightwards ? start_shrink : end_shrink);
        } else {
            auto downwards = frame.along.y() > 0;
            modified_rect.shrink(downwards ? start_shrink : end_shrink, 0, downwards ? end_shrink : start_shrink, 0);
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

    /**
     *   0 /-------------\ 7
     *    / /-----------\ \
     *   /-/ 3         4 \-\
     *  1  2             5  6
     * For each border edge, need to compute 8 points at most, then paint them as closed path. 8 points are the most
     * complicated case; it happens when the joined border width is not 0 and the border radius is larger than the
     * border width on both sides. If the border radius is smaller than the border width, then the inner corner of the
     * border corner is a right angle.
     */
    auto width_into = static_cast<float>(border_data.width.value());
    auto radius_along = [&](Gfx::CornerRadius const& corner) { return static_cast<float>(is_horizontal_edge ? corner.horizontal_radius : corner.vertical_radius); };
    auto radius_into = [&](Gfx::CornerRadius const& corner) { return static_cast<float>(is_horizontal_edge ? corner.vertical_radius : corner.horizontal_radius); };

    // compute_midpoint() works in page axes, so both its arguments and its result pass through the frame.
    auto midpoint = [&](float along, float into, float width_along) {
        auto point = is_horizontal_edge ? compute_midpoint(along, into, width_into, width_along) : compute_midpoint(into, along, width_along, width_into);
        return is_horizontal_edge ? point : Gfx::FloatPoint { point.y(), point.x() };
    };

    auto point_at = [&](DevicePixelPoint corner, Gfx::FloatPoint offset) {
        return Gfx::FloatPoint(corner.to_type<int>())
            + Gfx::FloatPoint(frame.along.x() * offset.x() + frame.into.x() * offset.y(), frame.along.y() * offset.x() + frame.into.y() * offset.y());
    };

    auto joined_width = static_cast<float>(joined_border_width.value());
    auto opposite_joined_width = static_cast<float>(opposite_joined_border_width.value());

    bool joined_corner_has_inner_corner = width_into < radius_into(radius) && joined_width < radius_along(radius);
    bool opposite_joined_corner_has_inner_corner = width_into < radius_into(opposite_radius) && opposite_joined_width < radius_along(opposite_radius);

    // An edge only gives up part of a corner where it meets a neighbour that has width, or where it follows a curve
    // into one. Without either it covers exactly its own rect.
    bool shares_a_corner = joined_width > 0 || opposite_joined_width > 0
        || radius.horizontal_radius > 0 || radius.vertical_radius > 0
        || opposite_radius.horizontal_radius > 0 || opposite_radius.vertical_radius > 0;

    auto joined_corner_endpoint = midpoint(radius_along(radius), radius_into(radius), joined_width);
    auto opposite_joined_corner_endpoint = midpoint(radius_along(opposite_radius), radius_into(opposite_radius), opposite_joined_width);

    Vector<Gfx::FloatPoint, 8> points;
    points.append(point_at(frame.start_outer, {}));
    points.append(point_at(frame.start_outer, { -joined_corner_endpoint.x(), radius_into(radius) - joined_corner_endpoint.y() }));

    if (joined_corner_has_inner_corner) {
        auto inner = midpoint(radius_along(radius) - joined_width, radius_into(radius) - width_into, joined_width);
        points.append(point_at(frame.start_inner, { -inner.x(), radius_into(radius) - width_into - inner.y() }));
        points.append(point_at(frame.start_inner, {}));
    } else {
        points.append(point_at(frame.start_inner, { joined_width - radius_along(radius), 0 }));
    }

    if (opposite_joined_corner_has_inner_corner) {
        auto inner = midpoint(radius_along(opposite_radius) - opposite_joined_width, radius_into(opposite_radius) - width_into, opposite_joined_width);
        points.append(point_at(frame.end_inner, {}));
        points.append(point_at(frame.end_inner, { inner.x(), radius_into(opposite_radius) - width_into - inner.y() }));
    } else {
        points.append(point_at(frame.end_inner, { radius_along(opposite_radius) - opposite_joined_width, 0 }));
    }

    points.append(point_at(frame.end_outer, { opposite_joined_corner_endpoint.x(), radius_into(opposite_radius) - opposite_joined_corner_endpoint.y() }));
    points.append(point_at(frame.end_outer, {}));

    auto inner_corner_offset = [&](float width_along) {
        return is_horizontal_edge ? Gfx::FloatSize(width_along, width_into) : Gfx::FloatSize(width_into, width_along);
    };
    auto joined_inner_corner_offset = inner_corner_offset(joined_width);
    auto opposite_joined_inner_corner_offset = inner_corner_offset(opposite_joined_width);

    auto append_edge_region = [&](Gfx::Path& target) {
        int current = 0;
        target.move_to(points[current++]);
        target.elliptical_arc_to(
            points[current++],
            Gfx::FloatSize(radius.horizontal_radius, radius.vertical_radius),
            0,
            false,
            false);
        target.line_to(points[current++]);
        if (joined_corner_has_inner_corner) {
            target.elliptical_arc_to(
                points[current++],
                Gfx::FloatSize(radius.horizontal_radius - joined_inner_corner_offset.width(), radius.vertical_radius - joined_inner_corner_offset.height()),
                0,
                false,
                true);
        }
        target.line_to(points[current++]);
        if (opposite_joined_corner_has_inner_corner) {
            target.elliptical_arc_to(
                points[current++],
                Gfx::FloatSize(opposite_radius.horizontal_radius - opposite_joined_inner_corner_offset.width(), opposite_radius.vertical_radius - opposite_joined_inner_corner_offset.height()),
                0,
                false,
                true);
        }
        target.line_to(points[current++]);
        target.elliptical_arc_to(
            points[current++],
            Gfx::FloatSize(opposite_radius.horizontal_radius, opposite_radius.vertical_radius),
            0,
            false,
            false);
    };

    // Fails for an edge too short to hold the pattern, which is painted solid instead.
    auto paint_patterned_edge = [&] {
        auto centerline = patterned_border_centerline(edge, rect, radius, opposite_radius, borders_data);
        if (!is_long_enough_for_pattern(centerline.length(), width_into, gfx_line_style == Gfx::LineStyle::Dotted))
            return false;

        flush_queued_edges();

        // The stroke is as wide as the border, so at a shared corner it would spill across the split into its
        // neighbour. Clipping it to the region a solid edge would have filled cuts it back there.
        Optional<DisplayListRecorderStateSaver> clip;
        if (shares_a_corner) {
            Gfx::Path edge_region;
            append_edge_region(edge_region);
            clip.emplace(painter);
            painter.add_clip_path(edge_region, Gfx::WindingRule::EvenOdd);
        }

        stroke_patterned_path(painter, move(centerline), gfx_line_style, width_into, color, false);
        return true;
    };

    if (gfx_line_style != Gfx::LineStyle::Solid && paint_patterned_edge())
        return;

    append_edge_region(path);

    // If joined borders have the same color, combine them to draw together.
    if (last || color != border_color(frame.opposite_joined_edge, borders_data))
        flush_queued_edges();
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
    // direction mirrors the one laid down in the other, which is the corner symmetry the spec encourages.
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

    auto line_style = borders_data.top.line_style == CSS::LineStyle::Dotted ? Gfx::LineStyle::Dotted : Gfx::LineStyle::Dashed;
    stroke_patterned_path(painter, move(centerline), line_style, width, borders_data.top.color, true);
}

void paint_all_borders(DisplayListRecorder& painter, DevicePixelRect const& border_rect, Gfx::CornerRadii const& corner_radii, BordersDataDevicePixels const& borders_data)
{
    if (borders_data.top.width <= 0 && borders_data.right.width <= 0 && borders_data.left.width <= 0 && borders_data.bottom.width <= 0)
        return;

    auto line_style = borders_data.top.line_style;
    if ((line_style == CSS::LineStyle::Dashed || line_style == CSS::LineStyle::Dotted) && borders_data.all_are_equal()) {
        // A box with a side too short for the pattern has to go through the per-edge painting below, which is where
        // that side falls back to a solid line.
        auto width = static_cast<float>(borders_data.top.width.value());
        auto dotted = line_style == CSS::LineStyle::Dotted;
        if (is_long_enough_for_pattern(static_cast<float>(border_rect.width().value()), width, dotted)
            && is_long_enough_for_pattern(static_cast<float>(border_rect.height().value()), width, dotted)) {
            paint_uniform_patterned_border(painter, border_rect, corner_radii, borders_data);
            return;
        }
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
