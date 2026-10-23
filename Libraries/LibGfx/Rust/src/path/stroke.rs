/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The area that stroking a path covers, as a path to fill with the nonzero winding rule.
//!
//! Each subpath is followed as a polyline through points along its curves, each with the directions in
//! which the path arrives at it and leaves it, and is cut into dashes when the stroke has a dash pattern.
//! Every line of a polyline adds the band between the normals of the path at its ends, every corner of the
//! path the piece that its join adds, and every open end its cap. All pieces wind the same way, so the
//! nonzero fill of them all is the area that they cover together.

use super::curves::{Curve, Point};
use super::geometry::{PathElement, PathGeometry};
use super::measure::line_count_to_follow;
use crate::geometry::FloatPoint;
use crate::paint_enums::{CapStyle, JoinStyle, WindingRule};

/// The smallest cosine of half a turn that one conic covers as a round join. A turn that comes closer to a
/// reversal gets a whole circle.
const SMALLEST_HALF_TURN_COSINE_OF_A_SECTOR: f64 = 0.1;

pub struct StrokeStyle<'a> {
    pub thickness: f32,
    pub cap_style: CapStyle,
    pub join_style: JoinStyle,
    pub miter_limit: f32,
    pub dash_array: &'a [f32],
    pub dash_offset: f32,
}

#[derive(Clone, Copy)]
struct PolylinePoint {
    point: Point,
    // The unit directions in which the path arrives at the point and leaves it. They differ only at the
    // corners of the path.
    incoming: Point,
    outgoing: Point,
}

struct Polyline {
    points: Vec<PolylinePoint>,
    closed: bool,
}

pub fn stroke_to_fill(geometry: &PathGeometry, style: &StrokeStyle) -> PathGeometry {
    let mut output = PathGeometry::new();
    let half_thickness = f64::from(style.thickness) / 2.0;
    if !geometry.has_geometry() || !half_thickness.is_finite() || half_thickness <= 0.0 {
        return output;
    }
    let mut stroker = Stroker {
        output: &mut output,
        half_thickness,
        style,
    };
    let dash_pattern = dash_pattern(style);
    for subpath in subpaths_of(geometry) {
        match &dash_pattern {
            Some(pattern) => {
                for dash in dashes_of(&subpath, pattern, f64::from(style.dash_offset)) {
                    stroker.stroke(&dash);
                }
            }
            None => stroker.stroke(&subpath),
        }
    }
    output.set_fill_rule(WindingRule::Nonzero);
    output.update_state_from_contours();
    output
}

fn unit(vector: Point) -> Point {
    vector.scaled(1.0 / vector.length())
}

fn normal(direction: Point) -> Point {
    Point::new(-direction.y, direction.x)
}

/// The subpaths of the path as polylines. Only a subpath that a segment follows its move into is stroked,
/// even if the segment goes nowhere, so that round and square caps can draw it as a dot.
fn subpaths_of(geometry: &PathGeometry) -> Vec<Polyline> {
    let mut subpaths = Vec::new();
    // The subpath that is being followed, and whether a segment has followed its move.
    let mut current: Option<(Polyline, bool)> = None;
    for element in geometry.elements() {
        if let PathElement::MoveTo(point) = element {
            if let Some((subpath, true)) = current.take() {
                subpaths.push(subpath);
            }
            // The directions at the start are those of the first segment that goes somewhere.
            let start = PolylinePoint {
                point: Point::from_float(point),
                incoming: Point::default(),
                outgoing: Point::default(),
            };
            current = Some((
                Polyline {
                    points: vec![start],
                    closed: false,
                },
                false,
            ));
            continue;
        }
        let (subpath, has_segment) = current.as_mut().expect("a segment follows a move");
        *has_segment = true;
        let Some(segment) = element.drawn_segment() else {
            continue;
        };
        if segment.is_degenerate() {
            if let PathElement::Close(_) = element {
                subpath.closed = true;
            }
            continue;
        }
        let curve = Curve::from_segment(&segment);
        let direction_at = |t: f64| {
            unit(
                curve
                    .direction_at(t)
                    .expect("a segment that goes somewhere has directions"),
            )
        };
        leave_in_direction(subpath, direction_at(0.0));
        let line_count = line_count_to_follow(&curve);
        for line in 1..=line_count {
            let t = f64::from(line) / f64::from(line_count);
            let point = if line == line_count {
                curve.end()
            } else {
                curve.point_at(t)
            };
            push_point(subpath, point, direction_at(t));
        }
        if let PathElement::Close(_) = element {
            subpath.closed = true;
        }
    }
    if let Some((subpath, true)) = current {
        subpaths.push(subpath);
    }
    subpaths
}

/// Sets the direction in which the path leaves the last point of the polyline, and the one in which it
/// arrives at the start, if nothing has arrived there.
fn leave_in_direction(polyline: &mut Polyline, direction: Point) {
    let last = polyline.points.last_mut().expect("a polyline has a start");
    last.outgoing = direction;
    if last.incoming == Point::default() {
        last.incoming = direction;
    }
}

/// Adds a point where the path goes in the direction. A point where the polyline already is only takes the
/// direction in which the path leaves it.
fn push_point(polyline: &mut Polyline, point: Point, direction: Point) {
    if let Some(last) = polyline.points.last_mut()
        && last.point == point
    {
        last.outgoing = direction;
        return;
    }
    polyline.points.push(PolylinePoint {
        point,
        incoming: direction,
        outgoing: direction,
    });
}

/// The dash pattern of the style, if it has one that dashes anything: an even number of lengths that are
/// not negative, and that do not all add up to nothing.
fn dash_pattern(style: &StrokeStyle) -> Option<Vec<f64>> {
    let pattern: Vec<f64> = style.dash_array.iter().map(|length| f64::from(*length)).collect();
    if pattern.is_empty()
        || !pattern.len().is_multiple_of(2)
        || !pattern.iter().all(|length| length.is_finite() && *length >= 0.0)
    {
        return None;
    }
    let total: f64 = pattern.iter().sum();
    (total > 0.0 && f64::from(style.dash_offset).is_finite()).then_some(pattern)
}

/// The dashes of a subpath. The pattern starts over at the start of each subpath, at the offset. A dash
/// that runs through the start of a closed subpath is one dash. A subpath that goes nowhere has no dashes.
fn dashes_of(subpath: &Polyline, pattern: &[f64], offset: f64) -> Vec<Polyline> {
    if subpath.points.len() < 2 {
        return Vec::new();
    }
    let total: f64 = pattern.iter().sum();
    let mut index = 0;
    let mut remaining = pattern[0];
    let mut phase = offset.rem_euclid(total);
    // A dash with no length at the offset still starts there, and draws a dot with round and square caps.
    while phase > remaining || (phase == remaining && remaining > 0.0) {
        phase -= remaining;
        index = (index + 1) % pattern.len();
        remaining = pattern[index];
    }
    remaining -= phase;

    // The lengths at even indices are dashes, and those at odd ones are gaps.
    let starts_in_dash = index % 2 == 0;
    let mut dashes = Vec::new();
    let mut current = starts_in_dash.then(|| vec![subpath.points[0]]);
    for pair in subpath.points.windows(2) {
        let (from, to) = (pair[0], pair[1]);
        let length = from.point.distance_to(to.point);
        let mut travelled = 0.0;
        while length - travelled > remaining {
            travelled += remaining;
            let fraction = travelled / length;
            let direction = unit(from.outgoing.lerp(to.incoming, fraction));
            let point = PolylinePoint {
                point: from.point.lerp(to.point, fraction),
                incoming: direction,
                outgoing: direction,
            };
            // The pattern alternates between dashes and gaps, so a dash ends here or one starts.
            match current.take() {
                Some(mut dash) => {
                    dash.push(point);
                    dashes.push(dash);
                }
                None => current = Some(vec![point]),
            }
            index = (index + 1) % pattern.len();
            remaining = pattern[index];
        }
        remaining -= length - travelled;
        if let Some(dash) = current.as_mut() {
            dash.push(to);
        }
    }
    let ends_in_dash = current.is_some();
    dashes.extend(current);

    if subpath.closed && starts_in_dash && ends_in_dash && dashes.len() > 1 {
        let first = dashes.remove(0);
        let last = dashes.last_mut().expect("there is more than one dash");
        last.last_mut().expect("a dash has points").outgoing = first[0].outgoing;
        last.extend(first.into_iter().skip(1));
    }
    dashes
        .into_iter()
        .map(|points| Polyline { points, closed: false })
        .collect()
}

struct Stroker<'a> {
    output: &'a mut PathGeometry,
    half_thickness: f64,
    style: &'a StrokeStyle<'a>,
}

impl Stroker<'_> {
    fn stroke(&mut self, polyline: &Polyline) {
        let mut points: Vec<PolylinePoint> = Vec::with_capacity(polyline.points.len());
        for point in &polyline.points {
            match points.last_mut() {
                Some(last) if last.point == point.point => last.outgoing = point.outgoing,
                _ => points.push(*point),
            }
        }
        if points.len() < 2 {
            if let Some(point) = points.first() {
                self.add_dot(point.point);
            }
            return;
        }

        for pair in points.windows(2) {
            self.add_band(pair[0], pair[1]);
        }
        for point in &points[1..points.len() - 1] {
            self.add_join(point.point, point.incoming, point.outgoing);
        }
        let (first, last) = (points[0], points[points.len() - 1]);
        if polyline.closed {
            self.add_join(first.point, last.incoming, first.outgoing);
        } else {
            self.add_cap(first.point, first.outgoing.scaled(-1.0));
            self.add_cap(last.point, last.incoming);
        }
    }

    /// The band that a line of the polyline sweeps, between the normals of the path where the line starts
    /// and where it ends.
    fn add_band(&mut self, from: PolylinePoint, to: PolylinePoint) {
        let from_offset = normal(from.outgoing).scaled(self.half_thickness);
        let to_offset = normal(to.incoming).scaled(self.half_thickness);
        let corners = [
            from.point.plus(from_offset),
            to.point.plus(to_offset),
            to.point.minus(to_offset),
            from.point.minus(from_offset),
        ];
        if is_convex(&corners) {
            self.add_convex_polygon(&corners);
            return;
        }
        // Where the curve bends more tightly than the stroke is wide, the normals cross on the inner side.
        // Triangles fanned from the line cover the band without needing it to be convex.
        for triangle in [
            [from.point, corners[0], corners[1]],
            [from.point, corners[1], to.point],
            [from.point, to.point, corners[2]],
            [from.point, corners[2], corners[3]],
        ] {
            self.add_convex_polygon(&triangle);
        }
    }

    /// The piece that the join adds at a corner, on the outer side of the turn.
    fn add_join(&mut self, point: Point, incoming: Point, outgoing: Point) {
        let cross = incoming.x * outgoing.y - incoming.y * outgoing.x;
        let dot = incoming.x * outgoing.x + incoming.y * outgoing.y;
        if cross == 0.0 && dot > 0.0 {
            return;
        }
        let half_turn_cosine = ((1.0 + dot) / 2.0).max(0.0).sqrt();
        // The outer side of a turn is the one away from the side that the path turns to.
        let outer_side = if cross > 0.0 { -1.0 } else { 1.0 };
        let incoming_offset = point.plus(normal(incoming).scaled(outer_side * self.half_thickness));
        let outgoing_offset = point.plus(normal(outgoing).scaled(outer_side * self.half_thickness));
        let miter_tip = || {
            let bisector = unit(incoming_offset.plus(outgoing_offset).minus(point.scaled(2.0)));
            point.plus(bisector.scaled(self.half_thickness / half_turn_cosine))
        };
        match self.style.join_style {
            JoinStyle::Round if half_turn_cosine < SMALLEST_HALF_TURN_COSINE_OF_A_SECTOR => {
                self.add_circle(point, self.half_thickness);
            }
            JoinStyle::Round => {
                // An arc around the corner from one offset to the other is a conic with the miter tip as its
                // control point, and the cosine of half the turn as its weight.
                self.add_sector(
                    point,
                    incoming_offset,
                    miter_tip(),
                    outgoing_offset,
                    half_turn_cosine as f32,
                );
            }
            // The miter is as long as the stroke is wide times one over the cosine of half the turn. One longer
            // than the limit is a bevel.
            JoinStyle::Miter
                if half_turn_cosine > 0.0 && 1.0 / half_turn_cosine <= f64::from(self.style.miter_limit) =>
            {
                self.add_convex_polygon(&[point, incoming_offset, miter_tip(), outgoing_offset]);
            }
            JoinStyle::Miter | JoinStyle::Bevel => {
                self.add_convex_polygon(&[point, incoming_offset, outgoing_offset]);
            }
        }
    }

    /// The cap at an open end, where the path leaves in the given direction.
    fn add_cap(&mut self, end: Point, direction: Point) {
        match self.style.cap_style {
            CapStyle::Butt => {}
            CapStyle::Round => self.add_circle(end, self.half_thickness),
            CapStyle::Square => {
                let along = direction.scaled(self.half_thickness);
                let across = normal(direction).scaled(self.half_thickness);
                self.add_convex_polygon(&[
                    end.plus(across),
                    end.plus(across).plus(along),
                    end.minus(across).plus(along),
                    end.minus(across),
                ]);
            }
        }
    }

    /// A polyline that goes nowhere: round caps make it a circle, and square caps a square along the x axis.
    fn add_dot(&mut self, point: Point) {
        match self.style.cap_style {
            CapStyle::Butt => {}
            CapStyle::Round => self.add_circle(point, self.half_thickness),
            CapStyle::Square => {
                let half = self.half_thickness;
                self.add_convex_polygon(&[
                    Point::new(point.x - half, point.y - half),
                    Point::new(point.x + half, point.y - half),
                    Point::new(point.x + half, point.y + half),
                    Point::new(point.x - half, point.y + half),
                ]);
            }
        }
    }

    /// Adds a convex polygon that winds the same way as every other piece. One without area adds nothing.
    fn add_convex_polygon(&mut self, points: &[Point]) {
        let twice_area: f64 = (0..points.len())
            .map(|index| {
                let (a, b) = (points[index], points[(index + 1) % points.len()]);
                a.x * b.y - b.x * a.y
            })
            .sum();
        if !twice_area.is_normal() {
            return;
        }
        let mut ordered: Vec<Point> = points.to_vec();
        if twice_area < 0.0 {
            ordered.reverse();
        }
        self.output.contour_move_to(ordered[0].to_float());
        for point in &ordered[1..] {
            self.output.contour_line_to(point.to_float());
        }
        self.output.contour_close();
    }

    /// Adds the sector of a circle around the center from one point on it to another, along the conic with
    /// the control point and the weight, winding the same way as the polygons.
    fn add_sector(&mut self, center: Point, from: Point, control: Point, to: Point, weight: f32) {
        let twice_area = (from.x - center.x) * (to.y - center.y) - (to.x - center.x) * (from.y - center.y);
        if !twice_area.is_normal() {
            return;
        }
        let (from, to) = if twice_area > 0.0 { (from, to) } else { (to, from) };
        self.output.contour_move_to(center.to_float());
        self.output.contour_line_to(from.to_float());
        self.output.contour_conic_to(control.to_float(), to.to_float(), weight);
        self.output.contour_close();
    }

    /// Adds a circle as four quarter circles, winding the same way as the polygons.
    fn add_circle(&mut self, center: Point, radius: f64) {
        let at = |x: f64, y: f64| FloatPoint {
            x: (center.x + x * radius) as f32,
            y: (center.y + y * radius) as f32,
        };
        let weight = std::f32::consts::FRAC_1_SQRT_2;
        self.output.contour_move_to(at(1.0, 0.0));
        self.output.contour_conic_to(at(1.0, 1.0), at(0.0, 1.0), weight);
        self.output.contour_conic_to(at(-1.0, 1.0), at(-1.0, 0.0), weight);
        self.output.contour_conic_to(at(-1.0, -1.0), at(0.0, -1.0), weight);
        self.output.contour_conic_to(at(1.0, -1.0), at(1.0, 0.0), weight);
        self.output.contour_close();
    }
}

/// Whether the polygon turns the same way at each corner.
fn is_convex(points: &[Point]) -> bool {
    let count = points.len();
    let turns = (0..count).map(|index| {
        let (a, b, c) = (points[index], points[(index + 1) % count], points[(index + 2) % count]);
        (b.x - a.x) * (c.y - b.y) - (b.y - a.y) * (c.x - b.x)
    });
    let (mut has_left_turn, mut has_right_turn) = (false, false);
    for turn in turns {
        has_left_turn |= turn > 0.0;
        has_right_turn |= turn < 0.0;
    }
    !(has_left_turn && has_right_turn)
}
