/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::line_style;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::painting::display_list::recorder::{
    DisplayListRecorder, FillPathParams, PaintStyleOrColor, StrokePathParams,
};
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::record::{BasePaintFacts, PaintRecorder};
use libgfx_rust::path::PathBuilder;
use libgfx_rust::{
    CapStyle, Color, CornerRadii, CornerRadius, FloatPoint, FloatSize, IntPoint, IntRect, JoinStyle, LineStyle,
    ShouldAntiAlias, WindingRule,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum BorderEdge {
    Top,
    Right,
    Bottom,
    Left,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BorderDataDevicePixels {
    pub color: Color,
    pub line_style: u8,
    pub width: i32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BordersDataDevicePixels {
    pub top: BorderDataDevicePixels,
    pub right: BorderDataDevicePixels,
    pub bottom: BorderDataDevicePixels,
    pub left: BorderDataDevicePixels,
}

impl BordersDataDevicePixels {
    pub fn for_edge(&self, edge: BorderEdge) -> &BorderDataDevicePixels {
        match edge {
            BorderEdge::Top => &self.top,
            BorderEdge::Right => &self.right,
            BorderEdge::Bottom => &self.bottom,
            BorderEdge::Left => &self.left,
        }
    }

    pub fn for_edge_mut(&mut self, edge: BorderEdge) -> &mut BorderDataDevicePixels {
        match edge {
            BorderEdge::Top => &mut self.top,
            BorderEdge::Right => &mut self.right,
            BorderEdge::Bottom => &mut self.bottom,
            BorderEdge::Left => &mut self.left,
        }
    }

    pub fn all_are_equal(&self) -> bool {
        self.top == self.right && self.top == self.bottom && self.top == self.left
    }
}

const DARK_LIGHT_ABSOLUTE_VALUE_DIFFERENCE: f64 = 1.0 / 3.0;

fn light_color_for_inset_and_outset(color: Color) -> Color {
    let (hue, saturation, value) = color.to_hsv();
    if value >= DARK_LIGHT_ABSOLUTE_VALUE_DIFFERENCE {
        return color;
    }
    Color::from_hsv(hue, saturation, value + DARK_LIGHT_ABSOLUTE_VALUE_DIFFERENCE).with_alpha(color.alpha())
}

fn dark_color_for_inset_and_outset(color: Color) -> Color {
    let (hue, saturation, value) = color.to_hsv();
    if value < DARK_LIGHT_ABSOLUTE_VALUE_DIFFERENCE {
        return color;
    }
    Color::from_hsv(hue, saturation, value - DARK_LIGHT_ABSOLUTE_VALUE_DIFFERENCE).with_alpha(color.alpha())
}

pub fn border_color(edge: BorderEdge, borders_data: &BordersDataDevicePixels) -> Color {
    let border_data = borders_data.for_edge(edge);
    if border_data.line_style == line_style::INSET {
        if edge == BorderEdge::Left || edge == BorderEdge::Top {
            return dark_color_for_inset_and_outset(border_data.color);
        }
        return light_color_for_inset_and_outset(border_data.color);
    }
    if border_data.line_style == line_style::OUTSET {
        if edge == BorderEdge::Left || edge == BorderEdge::Top {
            return light_color_for_inset_and_outset(border_data.color);
        }
        return dark_color_for_inset_and_outset(border_data.color);
    }
    border_data.color
}

// https://drafts.csswg.org/css-backgrounds-3/#corner-transitions
// Returns the offset from the center of a corner's ellipse to the point where the two borders meeting at that corner
// are split. Both components are positive; the caller applies the signs pointing towards its own corner. The spec
// leaves the point itself undefined, requiring only that it move continuously and monotonically with the ratio of the
// two border widths, so this takes where the curve is crossed by the line running from the corner of the border box to
// the corner of the padding box. That degenerates to the whole corner going to whichever border still has a width when
// the other reaches zero, which is the behavior the spec calls out separately.
fn compute_midpoint(
    horizontal_radius: f32,
    vertical_radius: f32,
    horizontal_border_width: f32,
    vertical_border_width: f32,
) -> FloatPoint {
    // Without a curve in one of the two directions there is no arc to divide between the borders.
    if horizontal_radius == 0.0 || vertical_radius == 0.0 {
        return FloatPoint {
            x: horizontal_radius,
            y: vertical_radius,
        };
    }
    if horizontal_border_width == 0.0 && vertical_border_width == 0.0 {
        return FloatPoint::default();
    }
    // Substituting that line into the ellipse leaves a quadratic in how far along it the crossing lies, of which the
    // smaller root is the crossing nearest the corner.
    let a = vertical_border_width * vertical_border_width / (horizontal_radius * horizontal_radius)
        + horizontal_border_width * horizontal_border_width / (vertical_radius * vertical_radius);
    let b = vertical_border_width / horizontal_radius + horizontal_border_width / vertical_radius;
    let distance = (b
        - (2.0 * vertical_border_width * horizontal_border_width / (horizontal_radius * vertical_radius)).sqrt())
        / a;
    FloatPoint {
        x: horizontal_radius - distance * vertical_border_width,
        y: vertical_radius - distance * horizontal_border_width,
    }
}

// https://drafts.csswg.org/css-backgrounds-3/#border-style
// There is no control over the spacing of the dots and dashes, nor over the length of the dashes. Implementations are
// encouraged to choose a spacing that makes the corners symmetrical.
// NB: The proportions themselves are ours: a dot spans the border width and is followed by a gap of the same size,
//     while a dash spans twice the border width and is followed by a gap of that same doubled size.
fn pattern_period(width: f32, dotted: bool) -> f32 {
    (if dotted { 2.0 } else { 4.0 }) * width
}

// AD-HOC: Other browsers abandon the pattern and draw a solid line along a side with no room for more than a single
//         period, which is what keeps a border-only CSS triangle from breaking up into stray marks. They disagree on
//         a side of exactly one period: Chrome draws it solid, Firefox draws the pattern. We follow Chrome, because
//         that also settles the dotted case, where a side of exactly one period is what a CSS triangle comes out as
//         and where Firefox draws nothing at all.
fn is_long_enough_for_pattern(length: f32, width: f32, dotted: bool) -> bool {
    length > pattern_period(width, dotted)
}

// The pattern is stretched to fit the path a whole number of times, and offset so that it leaves the ends of an open
// path, and the seam of a closed one, in a gap.
fn stroke_patterned_path(
    painter: &mut DisplayListRecorder,
    path: &libgfx_rust::path::OwnedPath,
    style: LineStyle,
    width: f32,
    color: Color,
    path_is_closed: bool,
) {
    let length = path.length();
    let dotted = style == LineStyle::Dotted;
    let periods = (length / pattern_period(width, dotted)).round().max(1.0);
    let interval = if dotted {
        length / periods
    } else {
        length / (2.0 * periods)
    };
    painter.stroke_path(StrokePathParams {
        cap_style: if dotted { CapStyle::Round } else { CapStyle::Butt },
        join_style: JoinStyle::Miter,
        miter_limit: 4.0,
        dash_array: if dotted {
            vec![0.0, interval]
        } else {
            vec![interval, interval]
        },
        dash_offset: if path_is_closed && !dotted {
            interval * 1.5
        } else {
            interval / 2.0
        },
        path,
        opacity: 1.0,
        paint_style_or_color: PaintStyleOrColor::Color(color),
        thickness: width,
        should_anti_alias: ShouldAntiAlias::Yes,
    });
}

// Each corner arc is described by the center of its ellipse, the radii the centerline follows, and the direction
// from that center towards the corner.
#[derive(Clone, Copy)]
struct Corner {
    center: FloatPoint,
    radii: FloatSize,
    direction: FloatPoint,
}

// Dashes and dots run along the middle of the border, so instead of filling the exact region covered by the edge -
// which is what the solid border painting below does - they are stroked along a centerline path: the half of the
// corner arc leading into the edge, the straight run, and the half of the corner arc leading out of it.
fn patterned_border_centerline(
    edge: BorderEdge,
    rect: IntRect,
    radius: CornerRadius,
    opposite_radius: CornerRadius,
    borders_data: &BordersDataDevicePixels,
) -> PathBuilder {
    let width = borders_data.for_edge(edge).width as f32;
    let half_width = width / 2.0;

    let (joined_border_width, opposite_joined_border_width) = match edge {
        BorderEdge::Top => (borders_data.left.width as f32, borders_data.right.width as f32),
        BorderEdge::Right => (borders_data.top.width as f32, borders_data.bottom.width as f32),
        BorderEdge::Bottom => (borders_data.right.width as f32, borders_data.left.width as f32),
        BorderEdge::Left => (borders_data.bottom.width as f32, borders_data.top.width as f32),
    };

    let is_horizontal_edge = matches!(edge, BorderEdge::Top | BorderEdge::Bottom);

    let centerline_radii = |corner: CornerRadius, joined_width: f32| -> FloatSize {
        let horizontal_inset = if is_horizontal_edge {
            joined_width / 2.0
        } else {
            half_width
        };
        let vertical_inset = if is_horizontal_edge {
            half_width
        } else {
            joined_width / 2.0
        };
        FloatSize {
            width: (corner.horizontal_radius as f32 - horizontal_inset).max(0.0),
            height: (corner.vertical_radius as f32 - vertical_inset).max(0.0),
        }
    };

    let left = rect.x as f32;
    let top = rect.y as f32;
    let right = rect.right() as f32;
    let bottom = rect.bottom() as f32;

    let horizontal_radius = radius.horizontal_radius as f32;
    let vertical_radius = radius.vertical_radius as f32;
    let opposite_horizontal_radius = opposite_radius.horizontal_radius as f32;
    let opposite_vertical_radius = opposite_radius.vertical_radius as f32;

    let point = |x: f32, y: f32| FloatPoint { x, y };
    let (start, end, straight_start, straight_end) = match edge {
        BorderEdge::Top => (
            Corner {
                center: point(left, top + vertical_radius),
                radii: centerline_radii(radius, joined_border_width),
                direction: point(-1.0, -1.0),
            },
            Corner {
                center: point(right, top + opposite_vertical_radius),
                radii: centerline_radii(opposite_radius, opposite_joined_border_width),
                direction: point(1.0, -1.0),
            },
            point(left, top + half_width),
            point(right, top + half_width),
        ),
        BorderEdge::Right => (
            Corner {
                center: point(right - horizontal_radius, top),
                radii: centerline_radii(radius, joined_border_width),
                direction: point(1.0, -1.0),
            },
            Corner {
                center: point(right - opposite_horizontal_radius, bottom),
                radii: centerline_radii(opposite_radius, opposite_joined_border_width),
                direction: point(1.0, 1.0),
            },
            point(right - half_width, top),
            point(right - half_width, bottom),
        ),
        BorderEdge::Bottom => (
            Corner {
                center: point(right, bottom - vertical_radius),
                radii: centerline_radii(radius, joined_border_width),
                direction: point(1.0, 1.0),
            },
            Corner {
                center: point(left, bottom - opposite_vertical_radius),
                radii: centerline_radii(opposite_radius, opposite_joined_border_width),
                direction: point(-1.0, 1.0),
            },
            point(right, bottom - half_width),
            point(left, bottom - half_width),
        ),
        BorderEdge::Left => (
            Corner {
                center: point(left + horizontal_radius, bottom),
                radii: centerline_radii(radius, joined_border_width),
                direction: point(-1.0, 1.0),
            },
            Corner {
                center: point(left + opposite_horizontal_radius, top),
                radii: centerline_radii(opposite_radius, opposite_joined_border_width),
                direction: point(-1.0, -1.0),
            },
            point(left + half_width, bottom),
            point(left + half_width, top),
        ),
    };

    let split_point = |corner: &Corner, joined_width: f32| -> FloatPoint {
        let midpoint = compute_midpoint(
            corner.radii.width,
            corner.radii.height,
            if is_horizontal_edge { width } else { joined_width },
            if is_horizontal_edge { joined_width } else { width },
        );
        FloatPoint {
            x: corner.center.x + corner.direction.x * midpoint.x,
            y: corner.center.y + corner.direction.y * midpoint.y,
        }
    };
    // A corner whose centerline radii have collapsed leaves the dashes meeting at a sharp angle instead of curving.
    let has_arc = |corner: &Corner| corner.radii.width > 0.0 && corner.radii.height > 0.0;

    let mut centerline = PathBuilder::new();
    if has_arc(&start) {
        let split = split_point(&start, joined_border_width);
        centerline.move_to(split.x, split.y);
        centerline.elliptical_arc_to(
            straight_start.x,
            straight_start.y,
            start.radii.width,
            start.radii.height,
            0.0,
            false,
            true,
        );
    } else {
        centerline.move_to(straight_start.x, straight_start.y);
    }
    centerline.line_to(straight_end.x, straight_end.y);
    if has_arc(&end) {
        let split = split_point(&end, opposite_joined_border_width);
        centerline.elliptical_arc_to(split.x, split.y, end.radii.width, end.radii.height, 0.0, false, true);
    }
    centerline
}

#[derive(Clone, Copy)]
struct Frame {
    along: FloatPoint,
    into: FloatPoint,
    start_outer: IntPoint,
    start_inner: IntPoint,
    end_inner: IntPoint,
    end_outer: IntPoint,
    joined_edge: BorderEdge,
    opposite_joined_edge: BorderEdge,
}

fn top_left(rect: IntRect) -> IntPoint {
    IntPoint { x: rect.x, y: rect.y }
}
fn top_right(rect: IntRect) -> IntPoint {
    IntPoint {
        x: rect.right(),
        y: rect.y,
    }
}
fn bottom_left(rect: IntRect) -> IntPoint {
    IntPoint {
        x: rect.x,
        y: rect.bottom(),
    }
}
fn bottom_right(rect: IntRect) -> IntPoint {
    IntPoint {
        x: rect.right(),
        y: rect.bottom(),
    }
}

#[allow(clippy::too_many_arguments)]
pub fn paint_border(
    painter: &mut DisplayListRecorder,
    edge: BorderEdge,
    rect: IntRect,
    radius: CornerRadius,
    opposite_radius: CornerRadius,
    borders_data: &BordersDataDevicePixels,
    path: &mut PathBuilder,
    last: bool,
) {
    let border_data = *borders_data.for_edge(edge);
    if border_data.width <= 0 {
        return;
    }
    let color = border_color(edge, borders_data);
    let border_style = border_data.line_style;

    // Edges sharing a color are collected into one path and only filled once the color changes or
    // the last edge is reached.
    let flush_queued_edges = |painter: &mut DisplayListRecorder, path: &mut PathBuilder| {
        if path.is_empty() {
            return;
        }
        let built = path.build();
        painter.fill_path(FillPathParams {
            path: &built,
            opacity: 1.0,
            paint_style_or_color: PaintStyleOrColor::Color(color),
            winding_rule: WindingRule::EvenOdd,
            should_anti_alias: ShouldAntiAlias::Yes,
        });
        *path = PathBuilder::new();
    };

    let is_horizontal_edge = matches!(edge, BorderEdge::Top | BorderEdge::Bottom);
    let point = |x: f32, y: f32| FloatPoint { x, y };
    let frame = match edge {
        BorderEdge::Top => Frame {
            along: point(1.0, 0.0),
            into: point(0.0, 1.0),
            start_outer: top_left(rect),
            start_inner: bottom_left(rect),
            end_inner: bottom_right(rect),
            end_outer: top_right(rect),
            joined_edge: BorderEdge::Left,
            opposite_joined_edge: BorderEdge::Right,
        },
        BorderEdge::Right => Frame {
            along: point(0.0, 1.0),
            into: point(-1.0, 0.0),
            start_outer: top_right(rect),
            start_inner: top_left(rect),
            end_inner: bottom_left(rect),
            end_outer: bottom_right(rect),
            joined_edge: BorderEdge::Top,
            opposite_joined_edge: BorderEdge::Bottom,
        },
        BorderEdge::Bottom => Frame {
            along: point(-1.0, 0.0),
            into: point(0.0, -1.0),
            start_outer: bottom_right(rect),
            start_inner: top_right(rect),
            end_inner: top_left(rect),
            end_outer: bottom_left(rect),
            joined_edge: BorderEdge::Right,
            opposite_joined_edge: BorderEdge::Left,
        },
        BorderEdge::Left => Frame {
            along: point(0.0, -1.0),
            into: point(1.0, 0.0),
            start_outer: bottom_left(rect),
            start_inner: bottom_right(rect),
            end_inner: top_right(rect),
            end_outer: top_left(rect),
            joined_edge: BorderEdge::Bottom,
            opposite_joined_edge: BorderEdge::Top,
        },
    };

    let joined_border_width = borders_data.for_edge(frame.joined_edge).width;
    let opposite_joined_border_width = borders_data.for_edge(frame.opposite_joined_edge).width;

    let paint_double_border = |painter: &mut DisplayListRecorder,
                               path: &mut PathBuilder,
                               proportional_line_thickness: f32,
                               outer_style: u8,
                               inner_style: u8| {
        // Each half is painted as a border of its own, deriving its own color from the style it is given.
        flush_queued_edges(painter, path);

        // AD-HOC: We clamp the individual borders to 1px thick if they're less so that they don't disappear entirely.
        //         This matches other browsers and is allowable per the spec, where the thickness is implementation-defined.
        // FIXME: Converting to floats and back is awkward, can we somehow do all this processing using CSSPixels?
        let mut modified_borders_data = *borders_data;
        let scaled = |width: i32| -> i32 { (width as f32 * proportional_line_thickness).max(1.0) as i32 };
        modified_borders_data.top.width = scaled(modified_borders_data.top.width);
        modified_borders_data.right.width = scaled(modified_borders_data.right.width);
        modified_borders_data.bottom.width = scaled(modified_borders_data.bottom.width);
        modified_borders_data.left.width = scaled(modified_borders_data.left.width);

        let mut modified_rect = rect;

        // Outer border, scaled back towards the outer edge of the rect
        if is_horizontal_edge {
            modified_rect.height = (rect.height as f32 * proportional_line_thickness).max(1.0) as i32;
        } else {
            modified_rect.width = (rect.width as f32 * proportional_line_thickness).max(1.0) as i32;
        }
        if edge == BorderEdge::Right {
            modified_rect.x += rect.right() - modified_rect.right();
        } else if edge == BorderEdge::Bottom {
            modified_rect.y += rect.bottom() - modified_rect.bottom();
        }
        modified_borders_data.for_edge_mut(edge).line_style = outer_style;
        paint_border(
            painter,
            edge,
            modified_rect,
            radius,
            opposite_radius,
            &modified_borders_data,
            path,
            true,
        );

        // Inner border, with each radius pulled in by the inset of the border that spans it, and
        // reaching back to where the original inner edge was.
        let inset_of = |inset_edge: BorderEdge| -> i32 {
            borders_data.for_edge(inset_edge).width - modified_borders_data.for_edge(inset_edge).width
        };

        let mut modified_radius = radius;
        let mut modified_opposite_radius = opposite_radius;
        let (outer_along_start, outer_along_end) = if is_horizontal_edge {
            (
                modified_radius.horizontal_radius,
                modified_opposite_radius.horizontal_radius,
            )
        } else {
            (
                modified_radius.vertical_radius,
                modified_opposite_radius.vertical_radius,
            )
        };
        {
            let (along_start, into_start) = if is_horizontal_edge {
                (
                    &mut modified_radius.horizontal_radius,
                    &mut modified_radius.vertical_radius,
                )
            } else {
                (
                    &mut modified_radius.vertical_radius,
                    &mut modified_radius.horizontal_radius,
                )
            };
            *along_start = (*along_start - inset_of(frame.joined_edge)).max(0);
            *into_start = (*into_start - inset_of(edge)).max(0);
        }
        {
            let (along_end, into_end) = if is_horizontal_edge {
                (
                    &mut modified_opposite_radius.horizontal_radius,
                    &mut modified_opposite_radius.vertical_radius,
                )
            } else {
                (
                    &mut modified_opposite_radius.vertical_radius,
                    &mut modified_opposite_radius.horizontal_radius,
                )
            };
            *along_end = (*along_end - inset_of(frame.opposite_joined_edge)).max(0);
            *into_end = (*into_end - inset_of(edge)).max(0);
        }

        match edge {
            BorderEdge::Top => modified_rect.y += rect.bottom() - modified_rect.bottom(),
            BorderEdge::Right => modified_rect.x = rect.x,
            BorderEdge::Bottom => modified_rect.y = rect.y,
            BorderEdge::Left => modified_rect.x += rect.right() - modified_rect.right(),
        }

        // The straight run of an edge starts where its corner curve ends, so the inner border only
        // begins further along than the outer one where its curve has been reduced past the inset
        // that moved it inwards.
        let start_shrink = (inset_of(frame.joined_edge) - outer_along_start).max(0);
        let end_shrink = (inset_of(frame.opposite_joined_edge) - outer_along_end).max(0);
        if is_horizontal_edge {
            let rightwards = frame.along.x > 0.0;
            let (right_shrink, left_shrink) = if rightwards {
                (end_shrink, start_shrink)
            } else {
                (start_shrink, end_shrink)
            };
            modified_rect.x += left_shrink;
            modified_rect.width -= left_shrink + right_shrink;
        } else {
            let downwards = frame.along.y > 0.0;
            let (top_shrink, bottom_shrink) = if downwards {
                (start_shrink, end_shrink)
            } else {
                (end_shrink, start_shrink)
            };
            modified_rect.y += top_shrink;
            modified_rect.height -= top_shrink + bottom_shrink;
        }

        modified_borders_data.for_edge_mut(edge).line_style = inner_style;
        paint_border(
            painter,
            edge,
            modified_rect,
            modified_radius,
            modified_opposite_radius,
            &modified_borders_data,
            path,
            true,
        );
    };

    // The only difference between Inset/Outset and Solid is the color, and we already handled that above.
    let gfx_line_style = match border_style {
        line_style::NONE | line_style::HIDDEN => return,
        line_style::DOTTED => LineStyle::Dotted,
        line_style::DASHED => LineStyle::Dashed,
        line_style::SOLID | line_style::INSET | line_style::OUTSET => LineStyle::Solid,
        line_style::DOUBLE => {
            // Treat this as two solid borders, each 1/3 of the width.
            paint_double_border(painter, path, 1.0 / 3.0, line_style::SOLID, line_style::SOLID);
            return;
        }
        line_style::GROOVE | line_style::RIDGE => {
            // Two half-width borders
            let (outer, inner) = if border_style == line_style::GROOVE {
                (line_style::INSET, line_style::OUTSET)
            } else {
                (line_style::OUTSET, line_style::INSET)
            };
            paint_double_border(painter, path, 0.5, outer, inner);
            return;
        }
        _ => LineStyle::Solid,
    };

    //   0 /-------------\ 7
    //    / /-----------\ \
    //   /-/ 3         4 \-\
    //  1  2             5  6
    // For each border edge, need to compute 8 points at most, then paint them as closed path. 8 points are the most
    // complicated case; it happens when the joined border width is not 0 and the border radius is larger than the
    // border width on both sides. If the border radius is smaller than the border width, then the inner corner of the
    // border corner is a right angle.
    let width_into = border_data.width as f32;
    let radius_along = |corner: CornerRadius| -> f32 {
        (if is_horizontal_edge {
            corner.horizontal_radius
        } else {
            corner.vertical_radius
        }) as f32
    };
    let radius_into = |corner: CornerRadius| -> f32 {
        (if is_horizontal_edge {
            corner.vertical_radius
        } else {
            corner.horizontal_radius
        }) as f32
    };

    // compute_midpoint() works in page axes, so both its arguments and its result pass through the frame.
    let midpoint = |along: f32, into: f32, width_along: f32| -> FloatPoint {
        let point = if is_horizontal_edge {
            compute_midpoint(along, into, width_into, width_along)
        } else {
            compute_midpoint(into, along, width_along, width_into)
        };
        if is_horizontal_edge {
            point
        } else {
            FloatPoint { x: point.y, y: point.x }
        }
    };

    let point_at = |corner: IntPoint, offset: FloatPoint| -> FloatPoint {
        FloatPoint {
            x: corner.x as f32 + (frame.along.x * offset.x + frame.into.x * offset.y),
            y: corner.y as f32 + (frame.along.y * offset.x + frame.into.y * offset.y),
        }
    };

    let joined_width = joined_border_width as f32;
    let opposite_joined_width = opposite_joined_border_width as f32;

    let joined_corner_has_inner_corner = width_into < radius_into(radius) && joined_width < radius_along(radius);
    let opposite_joined_corner_has_inner_corner =
        width_into < radius_into(opposite_radius) && opposite_joined_width < radius_along(opposite_radius);

    // An edge only gives up part of a corner where it meets a neighbour that has width, or where it follows a curve
    // into one. Without either it covers exactly its own rect.
    let shares_a_corner = joined_width > 0.0
        || opposite_joined_width > 0.0
        || radius.horizontal_radius > 0
        || radius.vertical_radius > 0
        || opposite_radius.horizontal_radius > 0
        || opposite_radius.vertical_radius > 0;

    let joined_corner_endpoint = midpoint(radius_along(radius), radius_into(radius), joined_width);
    let opposite_joined_corner_endpoint = midpoint(
        radius_along(opposite_radius),
        radius_into(opposite_radius),
        opposite_joined_width,
    );

    let mut points: Vec<FloatPoint> = Vec::with_capacity(8);
    points.push(point_at(frame.start_outer, FloatPoint::default()));
    points.push(point_at(
        frame.start_outer,
        point(
            -joined_corner_endpoint.x,
            radius_into(radius) - joined_corner_endpoint.y,
        ),
    ));

    if joined_corner_has_inner_corner {
        let inner = midpoint(
            radius_along(radius) - joined_width,
            radius_into(radius) - width_into,
            joined_width,
        );
        points.push(point_at(
            frame.start_inner,
            point(-inner.x, radius_into(radius) - width_into - inner.y),
        ));
        points.push(point_at(frame.start_inner, FloatPoint::default()));
    } else {
        points.push(point_at(
            frame.start_inner,
            point(joined_width - radius_along(radius), 0.0),
        ));
    }

    if opposite_joined_corner_has_inner_corner {
        let inner = midpoint(
            radius_along(opposite_radius) - opposite_joined_width,
            radius_into(opposite_radius) - width_into,
            opposite_joined_width,
        );
        points.push(point_at(frame.end_inner, FloatPoint::default()));
        points.push(point_at(
            frame.end_inner,
            point(inner.x, radius_into(opposite_radius) - width_into - inner.y),
        ));
    } else {
        points.push(point_at(
            frame.end_inner,
            point(radius_along(opposite_radius) - opposite_joined_width, 0.0),
        ));
    }

    points.push(point_at(
        frame.end_outer,
        point(
            opposite_joined_corner_endpoint.x,
            radius_into(opposite_radius) - opposite_joined_corner_endpoint.y,
        ),
    ));
    points.push(point_at(frame.end_outer, FloatPoint::default()));

    let inner_corner_offset = |width_along: f32| -> FloatSize {
        if is_horizontal_edge {
            FloatSize {
                width: width_along,
                height: width_into,
            }
        } else {
            FloatSize {
                width: width_into,
                height: width_along,
            }
        }
    };
    let joined_inner_corner_offset = inner_corner_offset(joined_width);
    let opposite_joined_inner_corner_offset = inner_corner_offset(opposite_joined_width);

    let append_edge_region = |target: &mut PathBuilder| {
        let mut current = 0;
        target.move_to(points[current].x, points[current].y);
        current += 1;
        target.elliptical_arc_to(
            points[current].x,
            points[current].y,
            radius.horizontal_radius as f32,
            radius.vertical_radius as f32,
            0.0,
            false,
            false,
        );
        current += 1;
        target.line_to(points[current].x, points[current].y);
        current += 1;
        if joined_corner_has_inner_corner {
            target.elliptical_arc_to(
                points[current].x,
                points[current].y,
                radius.horizontal_radius as f32 - joined_inner_corner_offset.width,
                radius.vertical_radius as f32 - joined_inner_corner_offset.height,
                0.0,
                false,
                true,
            );
            current += 1;
        }
        target.line_to(points[current].x, points[current].y);
        current += 1;
        if opposite_joined_corner_has_inner_corner {
            target.elliptical_arc_to(
                points[current].x,
                points[current].y,
                opposite_radius.horizontal_radius as f32 - opposite_joined_inner_corner_offset.width,
                opposite_radius.vertical_radius as f32 - opposite_joined_inner_corner_offset.height,
                0.0,
                false,
                true,
            );
            current += 1;
        }
        target.line_to(points[current].x, points[current].y);
        current += 1;
        target.elliptical_arc_to(
            points[current].x,
            points[current].y,
            opposite_radius.horizontal_radius as f32,
            opposite_radius.vertical_radius as f32,
            0.0,
            false,
            false,
        );
    };

    // Fails for an edge too short to hold the pattern, which is painted solid instead.
    if gfx_line_style != LineStyle::Solid {
        let centerline = patterned_border_centerline(edge, rect, radius, opposite_radius, borders_data).build();
        if is_long_enough_for_pattern(centerline.length(), width_into, gfx_line_style == LineStyle::Dotted) {
            flush_queued_edges(painter, path);
            // The stroke is as wide as the border, so at a shared corner it would spill across the
            // split into its neighbour. Clipping it to the region a solid edge would have filled
            // cuts it back there.
            if shares_a_corner {
                let mut edge_region = PathBuilder::new();
                append_edge_region(&mut edge_region);
                painter.save();
                painter.add_clip_path(&edge_region.build(), WindingRule::EvenOdd);
            }
            stroke_patterned_path(painter, &centerline, gfx_line_style, width_into, color, false);
            if shares_a_corner {
                painter.restore();
            }
            return;
        }
    }

    append_edge_region(path);

    // If joined borders have the same color, combine them to draw together.
    if last || color != border_color(frame.opposite_joined_edge, borders_data) {
        flush_queued_edges(painter, path);
    }
}

// When every edge shares a width, color and style there is nothing to attribute to one edge or the other, so the whole
// border is stroked as a single closed centerline. Splitting it per edge instead would cut each corner dash in two, and
// two separately flattened and antialiased strokes never join cleanly.
fn paint_uniform_patterned_border(
    painter: &mut DisplayListRecorder,
    border_rect: IntRect,
    corner_radii: CornerRadii,
    borders_data: &BordersDataDevicePixels,
) {
    let width = borders_data.top.width as f32;
    let half_width = width / 2.0;

    let left = border_rect.x as f32 + half_width;
    let top = border_rect.y as f32 + half_width;
    let right = border_rect.right() as f32 - half_width;
    let bottom = border_rect.bottom() as f32 - half_width;

    let centerline_radii = |corner: CornerRadius| -> FloatSize {
        let radii = FloatSize {
            width: (corner.horizontal_radius as f32 - half_width).max(0.0),
            height: (corner.vertical_radius as f32 - half_width).max(0.0),
        };
        if radii.width <= 0.0 || radii.height <= 0.0 {
            return FloatSize {
                width: 0.0,
                height: 0.0,
            };
        }
        radii
    };
    let mut top_left = centerline_radii(corner_radii.top_left);
    let mut top_right = centerline_radii(corner_radii.top_right);
    let mut bottom_right = centerline_radii(corner_radii.bottom_right);
    let mut bottom_left = centerline_radii(corner_radii.bottom_left);

    // https://drafts.csswg.org/css-backgrounds-3/#overlapping-curves
    // Corner curves must not overlap: When the sum of any two adjacent border radii exceeds the size of the border
    // box, UAs must proportionally reduce the used values of all border radii until none of them overlap.
    // AD-HOC: The reduction has to run again for the centerline, because a radius narrower than half the border
    //         collapses to nothing there instead of shrinking along with the rest, which lets two curves that do fit
    //         the border box overlap.

    // The algorithm for reducing radii is as follows:
    // Let f = min(Li/Si), where i ∈ {top, right, bottom, left}, Si is the sum of the two corresponding radii of the
    // corners on side i, and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box.
    let mut f = 1.0f32;
    let mut ratio_for_side = |sum_of_radii: f32, side_length: f32| {
        if sum_of_radii > side_length {
            f = f.min(side_length / sum_of_radii);
        }
    };
    ratio_for_side(top_left.width + top_right.width, right - left);
    ratio_for_side(bottom_left.width + bottom_right.width, right - left);
    ratio_for_side(top_right.height + bottom_right.height, bottom - top);
    ratio_for_side(top_left.height + bottom_left.height, bottom - top);
    // If f < 1, then all corner radii are reduced by multiplying them by f.
    if f < 1.0 {
        for corner in [&mut top_left, &mut top_right, &mut bottom_right, &mut bottom_left] {
            corner.width *= f;
            corner.height *= f;
        }
    }

    let is_empty = |size: FloatSize| size.width <= 0.0 || size.height <= 0.0;

    // Starting halfway along the top edge puts the seam on an axis of symmetry.
    let start_x = ((left + right) / 2.0).clamp(left + top_left.width, right - top_right.width);

    let mut centerline = PathBuilder::new();
    centerline.move_to(start_x, top);
    centerline.line_to(right - top_right.width, top);
    if !is_empty(top_right) {
        centerline.elliptical_arc_to(
            right,
            top + top_right.height,
            top_right.width,
            top_right.height,
            0.0,
            false,
            true,
        );
    }
    centerline.line_to(right, bottom - bottom_right.height);
    if !is_empty(bottom_right) {
        centerline.elliptical_arc_to(
            right - bottom_right.width,
            bottom,
            bottom_right.width,
            bottom_right.height,
            0.0,
            false,
            true,
        );
    }
    centerline.line_to(left + bottom_left.width, bottom);
    if !is_empty(bottom_left) {
        centerline.elliptical_arc_to(
            left,
            bottom - bottom_left.height,
            bottom_left.width,
            bottom_left.height,
            0.0,
            false,
            true,
        );
    }
    centerline.line_to(left, top + top_left.height);
    if !is_empty(top_left) {
        centerline.elliptical_arc_to(
            left + top_left.width,
            top,
            top_left.width,
            top_left.height,
            0.0,
            false,
            true,
        );
    }
    centerline.line_to(start_x, top);
    centerline.close();

    let style = if borders_data.top.line_style == line_style::DOTTED {
        LineStyle::Dotted
    } else {
        LineStyle::Dashed
    };
    stroke_patterned_path(painter, &centerline.build(), style, width, borders_data.top.color, true);
}

pub fn paint_all_borders(
    painter: &mut DisplayListRecorder,
    border_rect: IntRect,
    corner_radii: CornerRadii,
    borders_data: &BordersDataDevicePixels,
) {
    if borders_data.top.width <= 0
        && borders_data.right.width <= 0
        && borders_data.left.width <= 0
        && borders_data.bottom.width <= 0
    {
        return;
    }

    let style = borders_data.top.line_style;
    if (style == line_style::DASHED || style == line_style::DOTTED) && borders_data.all_are_equal() {
        let width = borders_data.top.width as f32;
        let dotted = style == line_style::DOTTED;
        // A box with a side too short for the pattern has to go through the per-edge painting below, which is where
        // that side falls back to a solid line.
        if is_long_enough_for_pattern(border_rect.width as f32, width, dotted)
            && is_long_enough_for_pattern(border_rect.height as f32, width, dotted)
        {
            paint_uniform_patterned_border(painter, border_rect, corner_radii, borders_data);
            return;
        }
    }

    let mut top_left = corner_radii.top_left;
    let mut top_right = corner_radii.top_right;
    let mut bottom_right = corner_radii.bottom_right;
    let mut bottom_left = corner_radii.bottom_left;
    let zero = CornerRadius {
        horizontal_radius: 0,
        vertical_radius: 0,
    };

    // Disable border radii if the corresponding borders don't exist:
    if borders_data.bottom.width <= 0 && borders_data.left.width <= 0 {
        bottom_left = zero;
    }
    if borders_data.bottom.width <= 0 && borders_data.right.width <= 0 {
        bottom_right = zero;
    }
    if borders_data.top.width <= 0 && borders_data.left.width <= 0 {
        top_left = zero;
    }
    if borders_data.top.width <= 0 && borders_data.right.width <= 0 {
        top_right = zero;
    }

    let top_border_rect = IntRect::new(
        border_rect.x + top_left.horizontal_radius,
        border_rect.y,
        border_rect.width - top_left.horizontal_radius - top_right.horizontal_radius,
        borders_data.top.width,
    );
    let right_border_rect = IntRect::new(
        border_rect.x + (border_rect.width - borders_data.right.width),
        border_rect.y + top_right.vertical_radius,
        borders_data.right.width,
        border_rect.height - top_right.vertical_radius - bottom_right.vertical_radius,
    );
    let bottom_border_rect = IntRect::new(
        border_rect.x + bottom_left.horizontal_radius,
        border_rect.y + (border_rect.height - borders_data.bottom.width),
        border_rect.width - bottom_left.horizontal_radius - bottom_right.horizontal_radius,
        borders_data.bottom.width,
    );
    let left_border_rect = IntRect::new(
        border_rect.x,
        border_rect.y + top_left.vertical_radius,
        borders_data.left.width,
        border_rect.height - top_left.vertical_radius - bottom_left.vertical_radius,
    );

    const EDGES: [BorderEdge; 4] = [BorderEdge::Top, BorderEdge::Right, BorderEdge::Bottom, BorderEdge::Left];

    // Find the first border that has a different color than the previous one, then start painting
    // from that border.
    let mut start_index = 0;
    for i in 0..EDGES.len() {
        if border_color(EDGES[i], borders_data) != border_color(EDGES[(i + 1) % EDGES.len()], borders_data) {
            start_index = (i + 1) % EDGES.len();
            break;
        }
    }

    let mut path = PathBuilder::new();
    for i in 0..EDGES.len() {
        let last = i == EDGES.len() - 1;
        match EDGES[(start_index + i) % EDGES.len()] {
            BorderEdge::Top => paint_border(
                painter,
                BorderEdge::Top,
                top_border_rect,
                top_left,
                top_right,
                borders_data,
                &mut path,
                last,
            ),
            BorderEdge::Right => paint_border(
                painter,
                BorderEdge::Right,
                right_border_rect,
                top_right,
                bottom_right,
                borders_data,
                &mut path,
                last,
            ),
            BorderEdge::Bottom => paint_border(
                painter,
                BorderEdge::Bottom,
                bottom_border_rect,
                bottom_right,
                bottom_left,
                borders_data,
                &mut path,
                last,
            ),
            BorderEdge::Left => paint_border(
                painter,
                BorderEdge::Left,
                left_border_rect,
                bottom_left,
                top_left,
                borders_data,
                &mut path,
                last,
            ),
        }
    }
}

pub(crate) fn paint_box_borders_from_style(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
    facts: &BasePaintFacts,
) {
    let data = recorder.data(paintable);
    let converter = recorder.converter;
    let device_side = |color: u32, style: u8, width: CssPixels| BorderDataDevicePixels {
        color: Color(color),
        line_style: style,
        width: converter.enclosing_device_pixels(width),
    };
    let default_side = BorderDataDevicePixels {
        color: Color::TRANSPARENT,
        line_style: line_style::NONE,
        width: 0,
    };
    let Some(style) = recorder.layout_arena.node_style_if_live(data.layout_node) else {
        return;
    };
    let zero = CssPixels::from_raw(0);
    let borders_data = BordersDataDevicePixels {
        top: if data.border.top == zero {
            default_side
        } else {
            device_side(
                style.border_top_color(),
                style.border_top_style(),
                style.border_top_width(),
            )
        },
        right: if data.border.right == zero {
            default_side
        } else {
            device_side(
                style.border_right_color(),
                style.border_right_style(),
                style.border_right_width(),
            )
        },
        bottom: if data.border.bottom == zero {
            default_side
        } else {
            device_side(
                style.border_bottom_color(),
                style.border_bottom_style(),
                style.border_bottom_width(),
            )
        },
        left: if data.border.left == zero {
            default_side
        } else {
            device_side(
                style.border_left_color(),
                style.border_left_style(),
                style.border_left_width(),
            )
        },
    };
    let css_border_widths = [
        if data.border.top == zero {
            zero
        } else {
            style.border_top_width()
        },
        if data.border.right == zero {
            zero
        } else {
            style.border_right_width()
        },
        if data.border.bottom == zero {
            zero
        } else {
            style.border_bottom_width()
        },
        if data.border.left == zero {
            zero
        } else {
            style.border_left_width()
        },
    ];
    let border_box_rect = crate::painting::paintable_geometry::absolute_border_box_rect(recorder.paintables, paintable);
    let border_radii =
        crate::painting::border_radii::BorderRadii::from_raw(recorder.hit_test_facts(paintable).border_radii);
    paint_box_borders(
        recorder,
        paintable,
        facts,
        border_box_rect,
        css_border_widths,
        &borders_data,
        border_radii,
    );
}

pub(crate) fn paint_box_borders(
    recorder: &mut PaintRecorder<'_>,
    paintable: PaintableSlotId,
    facts: &BasePaintFacts,
    border_box_rect: CssPixelRect,
    css_border_widths: [CssPixels; 4],
    borders_data: &BordersDataDevicePixels,
    border_radii: crate::painting::border_radii::BorderRadii,
) {
    if facts.paints_border_image
        && crate::painting::record::paint::border_image::paint_border_image(
            recorder,
            paintable,
            css_border_widths,
            border_box_rect,
        )
    {
        return;
    }
    let converter = recorder.converter;
    paint_all_borders(
        &mut recorder.recorder,
        converter.rounded_device_rect(border_box_rect),
        border_radii.as_corners(&converter),
        borders_data,
    );
}
