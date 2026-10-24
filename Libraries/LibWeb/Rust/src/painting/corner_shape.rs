/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Contours of boxes whose corners are shaped by `corner-shape`.
//!
//! https://drafts.csswg.org/css-borders-4/#corner-shape-rendering

use crate::css::computed_value_views::ComputedValuesView;
use libgfx_rust::path::Path;
use libgfx_rust::{CornerRadii, CornerRadius, FloatPoint, FloatRect};

/// The superellipse parameter of each corner, in the order top-left, top-right, bottom-right,
/// bottom-left.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct CornerShapes(pub [f64; 4]);

// How many line segments approximate each curved corner.
const SEGMENTS_PER_CURVED_CORNER: u32 = 32;

impl CornerShapes {
    pub(crate) fn of(style: ComputedValuesView<'_>) -> Self {
        let border = style.border();
        Self([
            border.corner_top_left_shape,
            border.corner_top_right_shape,
            border.corner_bottom_right_shape,
            border.corner_bottom_left_shape,
        ])
    }

    /// Whether the corners with a radius need a contour other than the elliptical arcs that rounded
    /// rectangles are drawn with.
    pub(crate) fn needs_contour(&self, radii: CornerRadii) -> bool {
        [radii.top_left, radii.top_right, radii.bottom_right, radii.bottom_left]
            .into_iter()
            .zip(self.0)
            .any(|(radius, shape)| radius.is_present() && shape != 1.0)
    }
}

/// Insets from the border box to the edge a contour traces, positive inward.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct Insets {
    pub top: f64,
    pub right: f64,
    pub bottom: f64,
    pub left: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Point {
    x: f64,
    y: f64,
}

#[derive(Clone, Copy, Debug, PartialEq)]
struct Vector {
    x: f64,
    y: f64,
}

impl Point {
    fn extended_by(self, vector: Vector) -> Point {
        Point {
            x: self.x + vector.x,
            y: self.y + vector.y,
        }
    }

    fn vector_to(self, other: Point) -> Vector {
        Vector {
            x: other.x - self.x,
            y: other.y - self.y,
        }
    }
}

impl Vector {
    fn scaled(self, factor: f64) -> Vector {
        Vector {
            x: self.x * factor,
            y: self.y * factor,
        }
    }

    fn plus(self, other: Vector) -> Vector {
        Vector {
            x: self.x + other.x,
            y: self.y + other.y,
        }
    }

    fn normalized(self) -> Vector {
        let length = self.x.hypot(self.y);
        if length == 0.0 {
            return self;
        }
        self.scaled(1.0 / length)
    }

    fn perpendicular(self) -> Vector {
        Vector { x: -self.y, y: self.x }
    }

    fn dot(self, other: Vector) -> f64 {
        self.x * other.x + self.y * other.y
    }
}

/// The intersection between the lines through `a1` and `a2` and through `b1` and `b2`, if they are
/// not parallel.
fn line_intersection(a1: Point, a2: Point, b1: Point, b2: Point) -> Option<Point> {
    let a = a1.vector_to(a2);
    let b = b1.vector_to(b2);
    let denominator = a.x * b.y - a.y * b.x;
    if denominator.abs() < 1e-12 {
        return None;
    }
    let t = ((b1.x - a1.x) * b.y - (b1.y - a1.y) * b.x) / denominator;
    Some(a1.extended_by(a.scaled(t)))
}

// https://drafts.csswg.org/css-borders-4/#normalized-superellipse-half-corner
fn normalized_superellipse_half_corner(k: f64) -> f64 {
    if k == f64::NEG_INFINITY {
        return 0.0;
    }
    if k == f64::INFINITY {
        return 1.0;
    }
    let n = 2f64.powf(k.abs());
    let convex_half_corner = 0.5f64.powf(1.0 / n);
    if k < 0.0 {
        1.0 - convex_half_corner
    } else {
        convex_half_corner
    }
}

struct CornerInputs {
    start_radius: f64,
    end_radius: f64,
    start_inset: f64,
    end_inset: f64,
    target_corner_outer: Point,
    normalized_v3: Vector,
    normalized_v2: Vector,
    k: f64,
}

// https://drafts.csswg.org/css-borders-4/#border-aligned-corner-clip-out-path
// NB: The clip-out path runs from the corner's start edge to its end edge, clockwise around the box, so
//     the points it extends to replace the corner of the contour. `None` clips nothing out, which leaves
//     the corner square.
fn border_aligned_corner_clip_out_path(inputs: CornerInputs) -> Option<Vec<Point>> {
    let CornerInputs {
        start_radius,
        end_radius,
        start_inset,
        end_inset,
        target_corner_outer,
        normalized_v3,
        normalized_v2,
        k,
    } = inputs;

    // 1. If startRadius or endRadius is zero, then return an empty path.
    if start_radius == 0.0 || end_radius == 0.0 {
        return None;
    }
    // 2. If K is ∞, then return an empty path.
    if k == f64::INFINITY {
        return None;
    }

    // 3. Let vectorToOriginalCornerOuterV3 be normalizedV3, scaled by -endInset.
    let vector_to_original_corner_outer_v3 = normalized_v3.scaled(-end_inset);
    // 4. Let vectorToOriginalCornerOuterV2 be normalizedV2, scaled by -startInset.
    let vector_to_original_corner_outer_v2 = normalized_v2.scaled(-start_inset);
    // 5. Let originalCornerOuter be targetCornerOuter, extended by « vectorToOriginalCornerOuterV3,
    //    vectorToOriginalCornerOuterV2 ».
    let original_corner_outer = target_corner_outer
        .extended_by(vector_to_original_corner_outer_v3)
        .extended_by(vector_to_original_corner_outer_v2);

    // 6. Let originalCornerStart be originalCornerOuter, extended by « normalizedV3 scaled by endRadius ».
    let original_corner_start = original_corner_outer.extended_by(normalized_v3.scaled(end_radius));
    // 7. Let originalCornerEnd be originalCornerOuter, extended by « normalizedV2 scaled by startRadius ».
    let original_corner_end = original_corner_outer.extended_by(normalized_v2.scaled(start_radius));

    // 8. Let halfCornerX be the normalized superellipse half corner given K.
    let half_corner_x = normalized_superellipse_half_corner(k);
    // 9. Let controlPointX be clamp(0, 1/(sqrt(2) - 1) ⋅ halfCornerX - 1/sqrt(2), 1).
    let control_point_x =
        (1.0 / (std::f64::consts::SQRT_2 - 1.0) * half_corner_x - 1.0 / std::f64::consts::SQRT_2).clamp(0.0, 1.0);

    // 10. Let insetDiff be clamp(-startRadius, endInset - startInset, endRadius).
    let inset_diff = (end_inset - start_inset).clamp(-start_radius, end_radius);
    // 11. If K is smaller than or equal to zero, and insetDiff is equal to -startRadius or endRadius, then return
    //     an empty path.
    if k <= 0.0 && (inset_diff == -start_radius || inset_diff == end_radius) {
        return None;
    }

    // 12. Let startControlPointX be controlPointX.
    let mut start_control_point_x = control_point_x;
    // 13. Let endControlPointX be controlPointX.
    let mut end_control_point_x = control_point_x;
    // 14. If insetDiff is not zero:
    if inset_diff != 0.0 {
        let root = (start_radius * start_radius + end_radius * end_radius - inset_diff * inset_diff).sqrt();
        // 1. Let bevelNormalVectorX be endRadius ⋅ insetDiff + startRadius ⋅ sqrt(startRadius² + endRadius² -
        //    insetDiff²).
        let bevel_normal_vector_x = end_radius * inset_diff + start_radius * root;
        // 2. Let bevelNormalVectorY be -startRadius ⋅ insetDiff + endRadius ⋅ sqrt(startRadius² + endRadius² -
        //    insetDiff²).
        let bevel_normal_vector_y = -start_radius * inset_diff + end_radius * root;
        // 3. Let bevelControlPointX be (startRadius ⋅ bevelNormalVectorY)/(startRadius ⋅ bevelNormalVectorY +
        //    endRadius ⋅ bevelNormalVectorX).
        let bevel_control_point_x = (start_radius * bevel_normal_vector_y)
            / (start_radius * bevel_normal_vector_y + end_radius * bevel_normal_vector_x);
        // 4. Set startControlPointX to bevelControlPointX · (2 · controlPointX) if K is less than zero,
        //    1 - (1 - bevelControlPointX) · (2 · (1 - controlPointX)) otherwise.
        start_control_point_x = if k < 0.0 {
            bevel_control_point_x * (2.0 * control_point_x)
        } else {
            1.0 - (1.0 - bevel_control_point_x) * (2.0 * (1.0 - control_point_x))
        };
        // 5. Set endControlPointX to 2 ⋅ controlPointX - startControlPointX.
        end_control_point_x = 2.0 * control_point_x - start_control_point_x;
    }

    // 15. Let unmappedStartNormalVector be the two-dimensional vector ((1 - startControlPointX) ⋅ startRadius,
    //     startControlPointX ⋅ endRadius), normalized.
    let unmapped_start_normal_vector = Vector {
        x: (1.0 - start_control_point_x) * start_radius,
        y: start_control_point_x * end_radius,
    }
    .normalized();
    // 16. Let unmappedEndNormalVector be the two-dimensional vector (endControlPointX ⋅ startRadius,
    //     (1 - endControlPointX) ⋅ endRadius), normalized.
    let unmapped_end_normal_vector = Vector {
        x: end_control_point_x * start_radius,
        y: (1.0 - end_control_point_x) * end_radius,
    }
    .normalized();

    // 17. Let startNormalVector be the sum of « normalizedV3 scaled by unmappedStartNormalVector[0], normalizedV2
    //     scaled by unmappedStartNormalVector[1] ».
    let start_normal_vector = normalized_v3
        .scaled(unmapped_start_normal_vector.x)
        .plus(normalized_v2.scaled(unmapped_start_normal_vector.y));
    // 18. Let endNormalVector be the sum of « normalizedV3 scaled by unmappedEndNormalVector[0], normalizedV2
    //     scaled by unmappedEndNormalVector[1] ».
    let end_normal_vector = normalized_v3
        .scaled(unmapped_end_normal_vector.x)
        .plus(normalized_v2.scaled(unmapped_end_normal_vector.y));

    // 19. Let adjustedCornerStart be originalCornerStart, extended by « startNormalVector scaled by startInset ».
    let mut adjusted_corner_start = original_corner_start.extended_by(start_normal_vector.scaled(start_inset));
    // 20. Let adjustedCornerEnd be originalCornerEnd, extended by « endNormalVector scaled by endInset ».
    let mut adjusted_corner_end = original_corner_end.extended_by(end_normal_vector.scaled(end_inset));

    // 21. Let startTangentVector be the perpendicular of startNormalVector, scaled by -1.
    let start_tangent_vector = start_normal_vector.perpendicular().scaled(-1.0);
    // 22. Let endTangentVector be the perpendicular of endNormalVector.
    let end_tangent_vector = end_normal_vector.perpendicular();

    // 23. Let miterStart be adjustedCornerStart.
    let mut miter_start = adjusted_corner_start;
    // 24. Let miterEnd be adjustedCornerEnd.
    let mut miter_end = adjusted_corner_end;

    // 25. If startInset is less than zero:
    if start_inset < 0.0 {
        // 1. Let clipStart be targetCornerOuter, extended by « normalizedV3 ».
        let clip_start = target_corner_outer.extended_by(normalized_v3);
        // 2. Set miterStart to the intersection between the lines (adjustedCornerStart, adjustedCornerStart
        //    extended by « startTangentVector ») and (clipStart, targetCornerOuter). If the lines are parallel,
        //    let it be adjustedCornerStart.
        miter_start = line_intersection(
            adjusted_corner_start,
            adjusted_corner_start.extended_by(start_tangent_vector),
            clip_start,
            target_corner_outer,
        )
        .unwrap_or(adjusted_corner_start);
        // 3. If K is greater than or equal to zero, set adjustedCornerStart to miterStart.
        if k >= 0.0 {
            adjusted_corner_start = miter_start;
        }
    }

    // 26. If endInset is less than zero:
    if end_inset < 0.0 {
        // 1. Let clipEnd be targetCornerOuter, extended by « normalizedV2 ».
        let clip_end = target_corner_outer.extended_by(normalized_v2);
        // 2. Set miterEnd to the intersection between the lines (adjustedCornerEnd, adjustedCornerEnd extended by
        //    « endTangentVector ») and (clipEnd, targetCornerOuter). If the lines are parallel, let it be
        //    adjustedCornerEnd.
        miter_end = line_intersection(
            adjusted_corner_end,
            adjusted_corner_end.extended_by(end_tangent_vector),
            clip_end,
            target_corner_outer,
        )
        .unwrap_or(adjusted_corner_end);
        // 3. If K is greater than or equal to zero, set adjustedCornerEnd to miterEnd.
        if k >= 0.0 {
            adjusted_corner_end = miter_end;
        }
    }

    // 27. Let adjustedCornerDiagonalVector be the vector between adjustedCornerStart and adjustedCornerEnd.
    let adjusted_corner_diagonal_vector = adjusted_corner_start.vector_to(adjusted_corner_end);
    // 28. Let adjustedCornerHeight be the dot product of adjustedCornerDiagonalVector and normalizedV2.
    let adjusted_corner_height = adjusted_corner_diagonal_vector.dot(normalized_v2);

    // 29. Let adjustedCornerOuter be adjustedCornerEnd, extended by « normalizedV2 scaled by
    //     -adjustedCornerHeight ».
    let adjusted_corner_outer = adjusted_corner_end.extended_by(normalized_v2.scaled(-adjusted_corner_height));
    // 30. Let adjustedCornerCenter be adjustedCornerStart, extended by « normalizedV2 scaled by
    //     adjustedCornerHeight ».
    let adjusted_corner_center = adjusted_corner_start.extended_by(normalized_v2.scaled(adjusted_corner_height));

    // 31. Let mapPointToCorner be the following steps, given numbers x and y, DOMPointReadOnly start, end, and
    //     center:
    let map_point_to_corner = |x: f64, y: f64, start: Point, end: Point, center: Point| {
        // 1. Let v1 be the vector between center and end, scaled by x.
        let v1 = center.vector_to(end).scaled(x);
        // 2. Let v4 be the vector between center and start, scaled by y.
        let v4 = center.vector_to(start).scaled(y);
        // 3. Return center, extended by « v1, v4 ».
        center.extended_by(v1).extended_by(v4)
    };

    // 32. Let selfIntersection be undefined.
    let mut self_intersection = None;
    // 33. If K, startInset, and endInset are less than zero:
    if k < 0.0 && start_inset < 0.0 && end_inset < 0.0 {
        // 1. If -endInset is greater than or equal to startRadius:
        if -end_inset >= start_radius {
            // 1. Set selfIntersection to the intersection between the segment (miterStart, adjustedCornerStart) and
            //    the line (miterEnd, adjustedCornerEnd).
            self_intersection = line_intersection(miter_start, adjusted_corner_start, miter_end, adjusted_corner_end);
        }
        // 2. If -startInset is greater than or equal to endRadius, and selfIntersection is undefined:
        if -start_inset >= end_radius && self_intersection.is_none() {
            // 1. Set selfIntersection to the intersection between the segment (miterEnd, adjustedCornerEnd) and
            //    the line (miterStart, adjustedCornerStart).
            self_intersection = line_intersection(miter_end, adjusted_corner_end, miter_start, adjusted_corner_start);
        }
    }

    // 34. Let path be a path, starting at miterStart.
    let mut path = vec![miter_start];

    // 35. If selfIntersection is a defined point:
    if let Some(self_intersection) = self_intersection {
        // 1. Extend path to selfIntersection.
        path.push(self_intersection);
    }
    // 36. Otherwise, if K is -∞:
    else if k == f64::NEG_INFINITY {
        // 1. Extend path to adjustedCornerCenter.
        path.push(adjusted_corner_center);
    }
    // 37. Otherwise, if K is greater than zero or less than or equal to -1:
    else if k > 0.0 || k <= -1.0 {
        // 1. Let n be 2^abs(K).
        let n = 2f64.powf(k.abs());
        // 2. Let curveCenter be adjustedCornerOuter if K is less than zero, adjustedCornerCenter otherwise.
        let curve_center = if k < 0.0 {
            adjusted_corner_outer
        } else {
            adjusted_corner_center
        };
        // 3. For every T between 0 and 1, in an implementation-approximated manner, extend path to the result of
        //    calling mapPointToCorner given T^(1/n) and (1 - T)^(1/n), adjustedCornerStart, adjustedCornerEnd,
        //    and curveCenter.
        for step in 0..=SEGMENTS_PER_CURVED_CORNER {
            let t = f64::from(step) / f64::from(SEGMENTS_PER_CURVED_CORNER);
            path.push(map_point_to_corner(
                t.powf(1.0 / n),
                (1.0 - t).powf(1.0 / n),
                adjusted_corner_start,
                adjusted_corner_end,
                curve_center,
            ));
        }
    }
    // 38. Otherwise, if K is greater than -1 and less than zero:
    else if k > -1.0 && k < 0.0 {
        // 1. Let tangentIntersection be the intersection between the lines (adjustedCornerStart,
        //    adjustedCornerStart extended by « startTangentVector ») and (adjustedCornerEnd, adjustedCornerEnd
        //    extended by « endTangentVector »). If the lines are parallel, let it be adjustedCornerStart.
        let tangent_intersection = line_intersection(
            adjusted_corner_start,
            adjusted_corner_start.extended_by(start_tangent_vector),
            adjusted_corner_end,
            adjusted_corner_end.extended_by(end_tangent_vector),
        )
        .unwrap_or(adjusted_corner_start);
        // 2. For every T between 0 and 1, in an implementation-approximated manner, extend path to the result of
        //    calling mapPointToCorner given 1 - (1 - T)^(1/2) and 1 - T^(1/2), adjustedCornerStart,
        //    adjustedCornerEnd, and tangentIntersection.
        for step in 0..=SEGMENTS_PER_CURVED_CORNER {
            let t = f64::from(step) / f64::from(SEGMENTS_PER_CURVED_CORNER);
            path.push(map_point_to_corner(
                1.0 - (1.0 - t).sqrt(),
                1.0 - t.sqrt(),
                adjusted_corner_start,
                adjusted_corner_end,
                tangent_intersection,
            ));
        }
    }

    // 39. Extend path to miterEnd.
    path.push(miter_end);
    // 40. Return path.
    Some(path)
}

/// https://drafts.csswg.org/css-borders-4/#border-contour-path
/// `border_rect` and `radii` are the border box and its used radii, in the device space the path is built in.
pub(crate) fn border_contour_path(
    border_rect: FloatRect,
    radii: CornerRadii,
    shapes: CornerShapes,
    insets: Insets,
) -> Path {
    let mut builder = Path::new();
    append_border_contour_path(&mut builder, border_rect, radii, shapes, insets);
    builder
}

/// Appends the border contour path to `builder` as a closed subpath of its own.
pub(crate) fn append_border_contour_path(
    builder: &mut Path,
    border_rect: FloatRect,
    radii: CornerRadii,
    shapes: CornerShapes,
    insets: Insets,
) {
    // 1. Let borderRect be element's border box.
    // 2. Let targetRect be borderRect, inset by topInset, rightInset, bottomInset, leftInset.
    let left = f64::from(border_rect.x) + insets.left;
    let top = f64::from(border_rect.y) + insets.top;
    let right = f64::from(border_rect.x + border_rect.width) - insets.right;
    let bottom = f64::from(border_rect.y + border_rect.height) - insets.bottom;

    // 3. Let path be a path that traces targetRect.
    // 4-7. Let topRightRadius, bottomRightRadius, bottomLeftRadius, and topLeftRadius be element's used values of
    //      the border radii.
    let width = |radius: CornerRadius| f64::from(radius.horizontal_radius);
    let height = |radius: CornerRadius| f64::from(radius.vertical_radius);

    // 8. Let adjustedInset be the following steps, given a radius radius, and numbers insetX and insetY:
    //    1. Let adjustedRadius be the outset-adjusted border radius given borderRect's size, radius, and the
    //       outset (-insetX, -insetY).
    //    2. Return the inset (radius's width - adjustedRadius's width, radius's height - adjustedRadius's height).
    // NB: An inward outset reduces a radius by the outset, down to zero, so the inset a corner is adjusted by is
    //     at most its radius. Outward insets keep their size.
    let adjusted_inset = |radius: f64, inset: f64| if inset > 0.0 { inset.min(radius) } else { inset };

    // 9-12. Clip out from path the border-aligned corner clip-out path of each corner.
    // NB: Each clip-out path runs clockwise from the edge before its corner to the one after it, so the contour
    //     is those paths in clockwise order, with a corner that clips nothing out left square.
    let corners = [
        // Top-left.
        CornerInputs {
            start_radius: width(radii.top_left),
            end_radius: height(radii.top_left),
            start_inset: adjusted_inset(width(radii.top_left), insets.left),
            end_inset: adjusted_inset(height(radii.top_left), insets.top),
            target_corner_outer: Point { x: left, y: top },
            normalized_v3: Vector { x: 0.0, y: 1.0 },
            normalized_v2: Vector { x: 1.0, y: 0.0 },
            k: shapes.0[0],
        },
        // Top-right.
        CornerInputs {
            start_radius: height(radii.top_right),
            end_radius: width(radii.top_right),
            start_inset: adjusted_inset(height(radii.top_right), insets.top),
            end_inset: adjusted_inset(width(radii.top_right), insets.right),
            target_corner_outer: Point { x: right, y: top },
            normalized_v3: Vector { x: -1.0, y: 0.0 },
            normalized_v2: Vector { x: 0.0, y: 1.0 },
            k: shapes.0[1],
        },
        // Bottom-right.
        CornerInputs {
            start_radius: width(radii.bottom_right),
            end_radius: height(radii.bottom_right),
            start_inset: adjusted_inset(width(radii.bottom_right), insets.right),
            end_inset: adjusted_inset(height(radii.bottom_right), insets.bottom),
            target_corner_outer: Point { x: right, y: bottom },
            normalized_v3: Vector { x: 0.0, y: -1.0 },
            normalized_v2: Vector { x: -1.0, y: 0.0 },
            k: shapes.0[2],
        },
        // Bottom-left.
        CornerInputs {
            start_radius: height(radii.bottom_left),
            end_radius: width(radii.bottom_left),
            start_inset: adjusted_inset(height(radii.bottom_left), insets.bottom),
            end_inset: adjusted_inset(width(radii.bottom_left), insets.left),
            target_corner_outer: Point { x: left, y: bottom },
            normalized_v3: Vector { x: 1.0, y: 0.0 },
            normalized_v2: Vector { x: 0.0, y: -1.0 },
            k: shapes.0[3],
        },
    ];

    let mut first = true;
    let mut extend = |point: Point| {
        if first {
            builder.move_to(FloatPoint {
                x: point.x as f32,
                y: point.y as f32,
            });
            first = false;
        } else {
            builder.line_to(FloatPoint {
                x: point.x as f32,
                y: point.y as f32,
            });
        }
    };
    let target_rect = (left, top, right, bottom);
    for corner in corners {
        let corner_outer = corner.target_corner_outer;
        // NB: Clipping the corner out of targetRect leaves the part of the clip-out path inside targetRect as the
        //     contour, which also holds when an inset moves the clip-out path partly outside targetRect.
        let inside = border_aligned_corner_clip_out_path(corner)
            .map(|points| polyline_inside_rect(&points, target_rect))
            .unwrap_or_default();
        if inside.is_empty() {
            extend(corner_outer);
        } else {
            inside.into_iter().for_each(&mut extend);
        }
    }
    builder.close();
    // 13. Return path.
}

/// The parts of `polyline` inside the rectangle `(left, top, right, bottom)`, in order.
fn polyline_inside_rect(polyline: &[Point], (left, top, right, bottom): (f64, f64, f64, f64)) -> Vec<Point> {
    const EPSILON: f64 = 1e-6;
    let mut inside: Vec<Point> = Vec::new();
    for segment in polyline.windows(2) {
        let (from, to) = (segment[0], segment[1]);
        let delta = from.vector_to(to);
        // Liang-Barsky: narrow the segment's parameter range to each side of the rectangle in turn.
        let (mut entry, mut exit) = (0.0f64, 1.0f64);
        let mut visible = true;
        for (step, room) in [
            (-delta.x, from.x - left + EPSILON),
            (delta.x, right - from.x + EPSILON),
            (-delta.y, from.y - top + EPSILON),
            (delta.y, bottom - from.y + EPSILON),
        ] {
            if step == 0.0 {
                if room < 0.0 {
                    visible = false;
                }
                continue;
            }
            let t = room / step;
            if step < 0.0 {
                entry = entry.max(t);
            } else {
                exit = exit.min(t);
            }
        }
        if !visible || entry > exit {
            continue;
        }
        for t in [entry, exit] {
            let point = from.extended_by(delta.scaled(t));
            if inside
                .last()
                .is_none_or(|last| (last.x - point.x).abs() > EPSILON || (last.y - point.y).abs() > EPSILON)
            {
                inside.push(point);
            }
        }
    }
    inside
}

#[cfg(test)]
mod tests {
    use super::*;

    fn corner(k: f64, start_inset: f64, end_inset: f64) -> Vec<Point> {
        border_aligned_corner_clip_out_path(CornerInputs {
            start_radius: 10.0,
            end_radius: 10.0,
            start_inset,
            end_inset,
            target_corner_outer: Point { x: 100.0, y: 0.0 },
            normalized_v3: Vector { x: -1.0, y: 0.0 },
            normalized_v2: Vector { x: 0.0, y: 1.0 },
            k,
        })
        .unwrap()
    }

    #[test]
    fn a_bevel_cuts_the_corner_with_a_straight_line() {
        assert_eq!(
            corner(0.0, 0.0, 0.0),
            [Point { x: 90.0, y: 0.0 }, Point { x: 100.0, y: 10.0 }]
        );
    }

    #[test]
    fn a_notch_steps_in_to_the_corner_center() {
        assert_eq!(
            corner(f64::NEG_INFINITY, 0.0, 0.0),
            [
                Point { x: 90.0, y: 0.0 },
                Point { x: 90.0, y: 10.0 },
                Point { x: 100.0, y: 10.0 }
            ]
        );
    }

    #[test]
    fn a_round_corner_passes_through_the_arc_midpoint() {
        let points = corner(1.0, 0.0, 0.0);
        let middle = points[points.len() / 2];
        let expected = 90.0 + 10.0 * std::f64::consts::FRAC_1_SQRT_2;
        assert!(
            (middle.x - expected).abs() < 1e-9
                && (middle.y - (10.0 - 10.0 * std::f64::consts::FRAC_1_SQRT_2)).abs() < 1e-9
        );
    }

    #[test]
    fn an_inset_bevel_is_the_outer_bevel_moved_inward_along_its_normal() {
        // The border box corner is at (100, 0), and the target corner is 2px inside it.
        let points = border_aligned_corner_clip_out_path(CornerInputs {
            start_radius: 10.0,
            end_radius: 10.0,
            start_inset: 2.0,
            end_inset: 2.0,
            target_corner_outer: Point { x: 98.0, y: 2.0 },
            normalized_v3: Vector { x: -1.0, y: 0.0 },
            normalized_v2: Vector { x: 0.0, y: 1.0 },
            k: 0.0,
        })
        .unwrap();
        let offset = std::f64::consts::SQRT_2;
        let (start, end) = (points[0], points[points.len() - 1]);
        assert!((start.x - (90.0 - offset)).abs() < 1e-9 && (start.y - offset).abs() < 1e-9);
        assert!((end.x - (100.0 - offset)).abs() < 1e-9 && (end.y - (10.0 + offset)).abs() < 1e-9);
    }

    #[test]
    fn only_the_part_of_a_polyline_inside_the_rect_is_kept() {
        let inside = polyline_inside_rect(
            &[
                Point { x: -5.0, y: 5.0 },
                Point { x: 5.0, y: 5.0 },
                Point { x: 5.0, y: 15.0 },
            ],
            (0.0, 0.0, 10.0, 10.0),
        );
        assert_eq!(inside.len(), 3);
        assert!((inside[0].x - 0.0).abs() < 1e-5 && (inside[0].y - 5.0).abs() < 1e-9);
        assert_eq!(inside[1], Point { x: 5.0, y: 5.0 });
        assert!((inside[2].x - 5.0).abs() < 1e-9 && (inside[2].y - 10.0).abs() < 1e-5);
    }
}
