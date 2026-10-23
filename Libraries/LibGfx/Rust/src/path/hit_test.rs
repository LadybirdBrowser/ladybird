/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Whether a point is inside a path, as the fill of the path covers it.
//!
//! https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ispointinpath
//!
//! Every subpath counts as closed. The winding number of the path around the point decides: the edges
//! that cross the horizontal ray from the point to the right add one when they go down and subtract one
//! when they go up. An edge includes the point where it starts in y and excludes the one where it ends,
//! so that a ray through a vertex counts it once where the path passes through and not at all where it
//! turns back.
//!
//! A point on the path itself is inside. A point where one edge ends and the next starts is on the next
//! edge only. Where the edges through a point cancel out, as where the path goes back over itself and
//! so bounds nothing, the winding number decides after all.

use super::curves::{Curve, Point};
use super::geometry::{PathElement, PathGeometry, Segment};
use crate::geometry::FloatPoint;
use crate::paint_enums::WindingRule;

/// The furthest that a point may be off a curve, relative to the size of its coordinates, and still
/// lie on it. The position of a curve at a given y is solved for numerically.
const ON_CURVE_TOLERANCE: f64 = 1e-6;

/// The number of bisection steps that find the parameter of a monotonic curve at a given y. Each step
/// halves the interval, so the parameter ends up as precise as a double allows.
const BISECTION_STEPS: u32 = 64;

/// How close the sum of the unit tangents of the edges through a point has to come to zero for them to
/// cancel out.
const CANCELLED_TANGENTS_TOLERANCE: f64 = 1e-6;

pub fn contains(geometry: &PathGeometry, x: f32, y: f32, fill_rule: WindingRule) -> bool {
    if !geometry.has_geometry() || !x.is_finite() || !y.is_finite() {
        return false;
    }
    let point = Point::new(f64::from(x), f64::from(y));
    let mut winding = 0i32;
    let mut tangents_through_point = Vec::new();
    for_each_filled_segment(geometry, |segment| {
        if !segment.is_degenerate() {
            add_crossings_of_segment(
                &Curve::from_segment(&segment),
                point,
                &mut winding,
                &mut tangents_through_point,
            );
        }
    });

    if !tangents_through_point.is_empty() {
        if tangents_through_point.len() % 2 == 1 {
            return true;
        }
        let tangent_sum = tangents_through_point
            .iter()
            .fold(Point::default(), |sum, tangent| sum.plus(*tangent));
        if tangent_sum.length() > CANCELLED_TANGENTS_TOLERANCE {
            return true;
        }
    }
    match fill_rule {
        WindingRule::Nonzero => winding != 0,
        WindingRule::EvenOdd => winding % 2 != 0,
    }
}

/// Calls the callback with the segments that bound the fill: those of the path, and a line back
/// to the start of every subpath that does not end there.
fn for_each_filled_segment(geometry: &PathGeometry, mut callback: impl FnMut(Segment)) {
    // The start of the subpath that is being walked, and where the walk is in it.
    let mut open_subpath: Option<(FloatPoint, FloatPoint)> = None;
    let close_open_subpath = |open_subpath: Option<(FloatPoint, FloatPoint)>, callback: &mut dyn FnMut(Segment)| {
        if let Some((start, current)) = open_subpath
            && start != current
        {
            callback(Segment::Line([current, start]));
        }
    };
    for element in geometry.elements() {
        if let PathElement::MoveTo(point) = element {
            close_open_subpath(open_subpath, &mut callback);
            open_subpath = Some((point, point));
        } else if let Some(segment) = element.drawn_segment() {
            callback(segment);
            if let Some((_, current)) = &mut open_subpath {
                *current = segment.end();
            }
        }
    }
    close_open_subpath(open_subpath, &mut callback);
}

fn unit(vector: Point) -> Point {
    vector.scaled(1.0 / vector.length())
}

/// Adds the crossings of the segment with the ray from the point to the winding number, and the unit
/// tangent of the segment where it passes through the point, if it does, to the tangents.
fn add_crossings_of_segment(curve: &Curve, point: Point, winding: &mut i32, tangents_through_point: &mut Vec<Point>) {
    if let Curve::Line([start, end]) = *curve {
        add_crossing_of_line(start, end, point, winding, tangents_through_point);
        return;
    }

    // A curve stays within the bounds of its control points.
    let control_points = curve.control_points();
    if control_points.iter().all(|control| control.y < point.y)
        || control_points.iter().all(|control| control.y > point.y)
    {
        return;
    }

    let tolerance = ON_CURVE_TOLERANCE * point.x.abs().max(point.y.abs()).max(1.0);
    // The end of a piece belongs to the next one, and the end of the segment to the next segment.
    let mut parameters_through_point: Vec<f64> = Vec::new();

    // The curve in pieces that go only one way in y.
    for (from, to) in monotonic_pieces(curve, 0.0, 1.0, |point| point.y) {
        let from_point = curve_point(curve, from);
        let to_point = curve_point(curve, to);
        let (low_y, high_y) = (from_point.y.min(to_point.y), from_point.y.max(to_point.y));
        if point.y < low_y || point.y > high_y {
            continue;
        }

        if low_y == high_y {
            // A piece that stays at one y can only have the point on it, at the parameters where it
            // reaches the x of the point, and crosses no ray.
            for (from, to) in monotonic_pieces(curve, from, to, |point| point.x) {
                let (from_x, to_x) = (curve_point(curve, from).x, curve_point(curve, to).x);
                if point.x < from_x.min(to_x) || point.x > from_x.max(to_x) {
                    continue;
                }
                let t = parameter_at(curve, from, to, from_x, to_x, point.x, |point| point.x);
                if t < to {
                    parameters_through_point.push(t);
                }
            }
            continue;
        }

        let t = parameter_at(curve, from, to, from_point.y, to_point.y, point.y, |point| point.y);
        let x_at_point = curve.point_at(t).x;
        if (x_at_point - point.x).abs() <= tolerance {
            if t < to {
                parameters_through_point.push(t);
            }
            continue;
        }
        if point.y == high_y || x_at_point < point.x {
            continue;
        }
        *winding += if to_point.y > from_point.y { 1 } else { -1 };
    }

    for t in parameters_through_point {
        if let Some(direction) = curve.direction_at(t) {
            tangents_through_point.push(unit(direction));
        }
    }
}

/// The ranges of the parameter between from and to in which the curve goes only one way in the coordinate.
fn monotonic_pieces(curve: &Curve, from: f64, to: f64, coordinate: impl Fn(Point) -> f64) -> Vec<(f64, f64)> {
    let mut parameters = vec![from];
    parameters.extend(
        curve
            .turning_parameters(coordinate)
            .into_iter()
            .filter(|t| *t > from && *t < to),
    );
    parameters.push(to);
    parameters.windows(2).map(|range| (range[0], range[1])).collect()
}

fn curve_point(curve: &Curve, t: f64) -> Point {
    if t == 0.0 {
        curve.start()
    } else if t == 1.0 {
        curve.end()
    } else {
        curve.point_at(t)
    }
}

fn add_crossing_of_line(
    start: Point,
    end: Point,
    point: Point,
    winding: &mut i32,
    tangents_through_point: &mut Vec<Point>,
) {
    let (low_y, high_y) = (start.y.min(end.y), start.y.max(end.y));
    if point.y < low_y || point.y > high_y {
        return;
    }
    let (low_x, high_x) = (start.x.min(end.x), start.x.max(end.x));
    // The sign of the cross product says on which side of the line the point is.
    let cross = (end.x - start.x) * (point.y - start.y) - (end.y - start.y) * (point.x - start.x);
    if cross == 0.0 && point.x >= low_x && point.x <= high_x {
        // The end of a line belongs to the next segment.
        if point != end {
            tangents_through_point.push(unit(end.minus(start)));
        }
        return;
    }
    if start.y == end.y || point.y == high_y {
        return;
    }
    // The point is left of a line that goes down when the cross product is positive, and left of one
    // that goes up when it is negative.
    let goes_down = end.y > start.y;
    if (goes_down && cross > 0.0) || (!goes_down && cross < 0.0) {
        *winding += if goes_down { 1 } else { -1 };
    }
}

/// The parameter in the range where the curve goes one way in the coordinate at which it reaches the given
/// value.
fn parameter_at(
    curve: &Curve,
    mut from: f64,
    mut to: f64,
    from_value: f64,
    to_value: f64,
    value: f64,
    coordinate: impl Fn(Point) -> f64,
) -> f64 {
    if value == from_value {
        return from;
    }
    if value == to_value {
        return to;
    }
    let increasing = to_value > from_value;
    for _ in 0..BISECTION_STEPS {
        let middle = (from + to) / 2.0;
        if (coordinate(curve.point_at(middle)) < value) == increasing {
            from = middle;
        } else {
            to = middle;
        }
    }
    (from + to) / 2.0
}
