/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lengths of paths, and positions and directions along them.
//!
//! A curve is measured in pieces between points at equal steps of its parameter. The number of steps
//! comes from Wang's formula, which keeps the line between the ends of each piece within
//! MEASURE_TOLERANCE of the curve. The length of each piece is the integral of the speed of the curve,
//! by Gauss-Legendre quadrature, which is exact to well within float precision for pieces this flat.
//! A position within a piece is at the parameter that is as far between its ends as the distance is.

use super::curves::{Curve, Point};
use super::geometry::{PathElement, PathElements, PathGeometry, Segment};
use crate::geometry::FloatPoint;

/// The furthest that the lines that measure a curve are from it.
const MEASURE_TOLERANCE: f64 = 1.0 / 32.0;

/// The most lines with which to follow a single curve, which bounds the work for a huge curve.
const MAXIMUM_LINE_COUNT_ALONG_CURVE: u32 = 4096;

/// The number of lines between points at equal parameter steps that follow the curve within the
/// tolerance, by Wang's formula: the square root of the largest second difference of its control
/// points, scaled by its degree, over the tolerance.
pub(super) fn line_count_to_follow(curve: &Curve) -> u32 {
    let second_difference = |points: &[Point], index: usize| {
        points[index]
            .minus(points[index + 1].scaled(2.0))
            .plus(points[index + 2])
            .length()
    };
    let scaled_second_difference = match curve {
        Curve::Line(_) => return 1,
        Curve::Quad(points) => second_difference(points, 0) / 4.0,
        // A conic with a weight of at most 1, like the arcs of SVG paths, bends no more than the quadratic curve
        // with the same control points.
        Curve::Conic(points, weight) => weight.max(1.0) * second_difference(points, 0) / 4.0,
        Curve::Cubic(points) => 3.0 * second_difference(points, 0).max(second_difference(points, 1)) / 4.0,
    };
    let line_count = (scaled_second_difference / MEASURE_TOLERANCE).sqrt().ceil();
    if line_count.is_nan() {
        return 1;
    }
    line_count.clamp(1.0, f64::from(MAXIMUM_LINE_COUNT_ALONG_CURVE)) as u32
}

/// The length of the curve between two parameters: the integral of its speed, by five-point
/// Gauss-Legendre quadrature.
fn length_between(curve: &Curve, from: f64, to: f64) -> f64 {
    const NODES_AND_WEIGHTS: [(f64, f64); 5] = [
        (0.0, 0.568_888_888_888_888_9),
        (-0.538_469_310_105_683_1, 0.478_628_670_499_366_5),
        (0.538_469_310_105_683_1, 0.478_628_670_499_366_5),
        (-0.906_179_845_938_664, 0.236_926_885_056_189_1),
        (0.906_179_845_938_664, 0.236_926_885_056_189_1),
    ];
    let half_width = (to - from) / 2.0;
    let middle = (from + to) / 2.0;
    let integral: f64 = NODES_AND_WEIGHTS
        .iter()
        .map(|(node, weight)| weight * curve.derivative_at(middle + half_width * node).length())
        .sum();
    integral * half_width
}

#[derive(Clone, Copy, Debug)]
struct MeasuredPiece {
    segment_index: usize,
    from_parameter: f64,
    to_parameter: f64,
    distance_at_end: f64,
}

/// The measure of one contour of a path that has length.
#[derive(Clone, Debug)]
pub struct ContourMeasure {
    curves: Vec<Curve>,
    pieces: Vec<MeasuredPiece>,
}

impl ContourMeasure {
    fn new() -> Self {
        Self {
            curves: Vec::new(),
            pieces: Vec::new(),
        }
    }

    fn distance(&self) -> f64 {
        self.pieces.last().map_or(0.0, |piece| piece.distance_at_end)
    }

    fn add_segment(&mut self, segment: &Segment) {
        let curve = Curve::from_segment(segment);
        let line_count = line_count_to_follow(&curve);
        self.add_curve(curve, line_count);
    }

    fn add_curve(&mut self, curve: Curve, line_count: u32) {
        let segment_index = self.curves.len();
        let mut distance = self.distance();
        let mut previous_point = curve.start();
        let mut previous_parameter = 0.0;
        for line in 1..=line_count {
            let parameter = f64::from(line) / f64::from(line_count);
            let point = if line == line_count {
                curve.end()
            } else {
                curve.point_at(parameter)
            };
            let length = match curve {
                Curve::Line(_) => previous_point.distance_to(point),
                _ => length_between(&curve, previous_parameter, parameter),
            };
            if length > 0.0 {
                distance += length;
                self.pieces.push(MeasuredPiece {
                    segment_index,
                    from_parameter: previous_parameter,
                    to_parameter: parameter,
                    distance_at_end: distance,
                });
            }
            previous_point = point;
            previous_parameter = parameter;
        }
        self.curves.push(curve);
    }

    pub fn length(&self) -> f32 {
        self.distance() as f32
    }

    /// The point at the distance along the contour, clamped to its ends, and the unit tangent there.
    pub fn position_and_tangent(&self, distance: f32) -> (FloatPoint, FloatPoint) {
        let distance = f64::from(distance).clamp(0.0, self.distance());
        let piece_index = self
            .pieces
            .partition_point(|piece| piece.distance_at_end < distance)
            .min(self.pieces.len() - 1);
        let piece = &self.pieces[piece_index];
        let distance_at_start = if piece_index == 0 {
            0.0
        } else {
            self.pieces[piece_index - 1].distance_at_end
        };
        let fraction = (distance - distance_at_start) / (piece.distance_at_end - distance_at_start);
        let curve = &self.curves[piece.segment_index];
        let parameter = piece.from_parameter + (piece.to_parameter - piece.from_parameter) * fraction;
        let position = match *curve {
            Curve::Line([start, end]) => start.lerp(end, parameter),
            _ => curve.point_at(parameter),
        };
        let direction = curve.direction_at(parameter).unwrap_or_default();
        let direction_length = direction.length();
        let tangent = if direction_length > 0.0 {
            direction.scaled(1.0 / direction_length)
        } else {
            direction
        };
        (position.to_float(), tangent.to_float())
    }

    /// The measures of the contours of the path that have length, in order, each measured when it is
    /// asked for. A close adds the line back to the start of its contour; an open contour gets no such
    /// line.
    pub fn contours_of(geometry: &PathGeometry) -> ContourMeasures<'_> {
        ContourMeasures {
            elements: geometry.has_geometry().then(|| geometry.elements()),
        }
    }

    fn of_curve(curve: Curve, line_count: u32) -> ContourMeasure {
        let mut contour = ContourMeasure::new();
        contour.add_curve(curve, line_count);
        contour
    }
}

pub struct ContourMeasures<'a> {
    elements: Option<PathElements<'a>>,
}

impl Iterator for ContourMeasures<'_> {
    type Item = ContourMeasure;

    fn next(&mut self) -> Option<ContourMeasure> {
        let elements = self.elements.as_mut()?;
        let mut contour = ContourMeasure::new();
        for element in elements.by_ref() {
            match element.drawn_segment() {
                Some(segment) => contour.add_segment(&segment),
                None if !contour.pieces.is_empty() => return Some(contour),
                None => contour.curves.clear(),
            }
        }
        self.elements = None;
        (!contour.pieces.is_empty()).then_some(contour)
    }
}

/// The length of the path: the sum of the lengths of its contours.
pub fn length(geometry: &PathGeometry) -> f32 {
    // Summing floats starts from -0, which a path without length must not give.
    ContourMeasure::contours_of(geometry).fold(0.0, |length, contour| length + contour.length())
}

/// The point at a distance along the path, clamped to its ends, and the unit tangent of the path there.
/// The gaps between the subpaths do not add to the distance. A path without length is only its start
/// point, where it has no tangent.
pub fn point_at_length(geometry: &PathGeometry, distance: f32) -> Option<(FloatPoint, FloatPoint)> {
    let mut remaining_distance = distance.max(0.0);
    let mut end_of_path = None;
    for contour in ContourMeasure::contours_of(geometry) {
        let contour_length = contour.length();
        end_of_path = Some(contour.position_and_tangent(remaining_distance.min(contour_length)));
        if remaining_distance <= contour_length {
            return end_of_path;
        }
        remaining_distance -= contour_length;
    }
    if end_of_path.is_some() {
        return end_of_path;
    }
    if !geometry.has_geometry() {
        return None;
    }
    Some((geometry.points()[0], FloatPoint::default()))
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct PathVertex {
    pub point: FloatPoint,
    pub distance: f32,
    pub direction: FloatPoint,
}

/// The vertices of polylines through the ends of the segments of the path, with their distance along
/// it. Along a curved segment, the polylines pass through points along the curve that keep them within
/// MEASURE_TOLERANCE of it. The gaps between the subpaths do not add to the distance, so a subpath
/// starts at the distance its predecessor ends at. A path without length is only its start point.
///
/// Each vertex has the unit tangent of the path there as its direction. Where the path changes
/// direction at once, the vertex appears twice: with the direction in which the path arrives, and with
/// the one in which it leaves. A subpath start that no segment with length leaves from has no
/// direction.
pub fn vertices(geometry: &PathGeometry) -> Vec<PathVertex> {
    let mut vertices: Vec<PathVertex> = Vec::new();
    if !geometry.has_geometry() {
        return vertices;
    }
    let mut distance = 0.0f32;
    for element in geometry.elements() {
        let Some(segment) = element.drawn_segment() else {
            if let PathElement::MoveTo(point) = element {
                vertices.push(PathVertex {
                    point,
                    distance,
                    direction: FloatPoint::default(),
                });
            }
            continue;
        };

        // A segment without length has no direction, and leaves the path where it was.
        let curve = Curve::from_segment(&segment);
        let line_count = line_count_to_follow(&curve);
        let measure = ContourMeasure::of_curve(curve, line_count);
        let segment_length = measure.length();
        if segment_length <= 0.0 {
            continue;
        }
        let direction_at = |distance_in_segment: f32| measure.position_and_tangent(distance_in_segment).1;

        let start_direction = direction_at(0.0);
        if let Some(last) = vertices.last_mut()
            && last.direction == FloatPoint::default()
        {
            last.direction = start_direction;
        } else if vertices.last().map(|vertex| vertex.direction) != Some(start_direction) {
            vertices.push(PathVertex {
                point: segment.start(),
                distance,
                direction: start_direction,
            });
        }

        let mut curve = CurveToFollow {
            measure: &measure,
            distance_to_curve: distance,
            vertices: &mut vertices,
            remaining_line_count: MAXIMUM_LINE_COUNT_ALONG_CURVE - line_count,
        };
        let mut previous_distance = 0.0f32;
        let mut previous_point = segment.start();
        for line in 1..=line_count {
            let distance_in_segment = segment_length * line as f32 / line_count as f32;
            let (position, tangent) = if line == line_count {
                (segment.end(), FloatPoint::default())
            } else {
                measure.position_and_tangent(distance_in_segment)
            };
            curve.follow_between(previous_distance, previous_point, distance_in_segment, position);
            if line < line_count {
                curve.vertices.push(PathVertex {
                    point: position,
                    distance: distance + distance_in_segment,
                    direction: tangent,
                });
            }
            previous_distance = distance_in_segment;
            previous_point = position;
        }
        distance += segment_length;
        vertices.push(PathVertex {
            point: segment.end(),
            distance,
            direction: direction_at(segment_length),
        });
    }

    if vertices.is_empty() {
        vertices.push(PathVertex {
            point: geometry.points()[0],
            distance: 0.0,
            direction: FloatPoint::default(),
        });
    }
    vertices
}

struct CurveToFollow<'a> {
    measure: &'a ContourMeasure,
    distance_to_curve: f32,
    vertices: &'a mut Vec<PathVertex>,
    remaining_line_count: u32,
}

impl CurveToFollow<'_> {
    /// Appends the vertices between two points along the curve, splitting the line between them in half
    /// for as long as the curve passes further than the tolerance from it halfway, or until the curve has
    /// as many lines as it may have.
    fn follow_between(
        &mut self,
        start_distance: f32,
        start_point: FloatPoint,
        end_distance: f32,
        end_point: FloatPoint,
    ) {
        if self.remaining_line_count == 0 {
            return;
        }
        let middle_distance = (start_distance + end_distance) / 2.0;
        let (middle_point, tangent) = self.measure.position_and_tangent(middle_distance);
        let chord_middle = FloatPoint {
            x: (start_point.x + end_point.x) / 2.0,
            y: (start_point.y + end_point.y) / 2.0,
        };
        if Point::from_float(middle_point).distance_to(Point::from_float(chord_middle)) <= MEASURE_TOLERANCE {
            return;
        }

        self.remaining_line_count -= 1;
        self.follow_between(start_distance, start_point, middle_distance, middle_point);
        self.vertices.push(PathVertex {
            point: middle_point,
            distance: self.distance_to_curve + middle_distance,
            direction: tangent,
        });
        self.follow_between(middle_distance, middle_point, end_distance, end_point);
    }
}
