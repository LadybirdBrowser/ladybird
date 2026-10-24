/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The data of a `Gfx::Path`: its verbs, the points that they use and the weights of its conics,
//! and the rules by which the operations of `Gfx::Path` add to them.

use super::arcs;
use crate::geometry::FloatPoint;
use crate::matrix::AffineTransform;
use crate::paint_enums::WindingRule;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PathVerb {
    Move = 0,
    Line = 1,
    Quad = 2,
    Conic = 3,
    Cubic = 4,
    Close = 5,
}

impl PathVerb {
    pub fn from_u8(value: u8) -> Option<Self> {
        Some(match value {
            0 => Self::Move,
            1 => Self::Line,
            2 => Self::Quad,
            3 => Self::Conic,
            4 => Self::Cubic,
            5 => Self::Close,
            _ => return None,
        })
    }

    /// The number of points that the verb stores.
    pub fn stored_point_count(self) -> usize {
        match self {
            Self::Move | Self::Line => 1,
            Self::Quad | Self::Conic => 2,
            Self::Cubic => 3,
            Self::Close => 0,
        }
    }
}

/// A segment of a path, with the point where it starts as its first point.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum Segment {
    Line([FloatPoint; 2]),
    Quad([FloatPoint; 3]),
    Conic([FloatPoint; 3], f32),
    Cubic([FloatPoint; 4]),
}

impl Segment {
    pub fn points(&self) -> &[FloatPoint] {
        match self {
            Self::Line(points) => points,
            Self::Quad(points) | Self::Conic(points, _) => points,
            Self::Cubic(points) => points,
        }
    }

    pub fn start(&self) -> FloatPoint {
        self.points()[0]
    }

    pub fn end(&self) -> FloatPoint {
        *self.points().last().expect("a segment has points")
    }

    /// Whether every point of the segment is the same, so that it goes nowhere.
    pub fn is_degenerate(&self) -> bool {
        let start = self.start();
        self.points().iter().all(|point| *point == start)
    }
}

/// What a walk over the verbs of a path meets: the start of a contour, a segment, or a close with
/// the line back to the start of its contour.
#[derive(Clone, Copy, Debug, PartialEq)]
pub enum PathElement {
    MoveTo(FloatPoint),
    Segment(Segment),
    Close([FloatPoint; 2]),
}

impl PathElement {
    /// The segment that the element draws, if any. A close draws the line back to the start.
    pub fn drawn_segment(&self) -> Option<Segment> {
        match self {
            Self::MoveTo(_) => None,
            Self::Segment(segment) => Some(*segment),
            Self::Close(points) => Some(Segment::Line(*points)),
        }
    }
}

#[derive(Clone, Debug)]
pub struct PathGeometry {
    verbs: Vec<PathVerb>,
    points: Vec<FloatPoint>,
    conic_weights: Vec<f32>,
    fill_rule: WindingRule,
    has_only_finite_points: bool,

    // What the operations of Gfx::Path remember between calls: whether there is a point to draw from,
    // and where the last subpath started.
    has_current_point: bool,
    last_move_to: FloatPoint,
}

fn is_finite_point(point: FloatPoint) -> bool {
    point.x.is_finite() && point.y.is_finite()
}

impl Default for PathGeometry {
    fn default() -> Self {
        Self::new()
    }
}

impl PathGeometry {
    pub fn new() -> Self {
        Self {
            verbs: Vec::new(),
            points: Vec::new(),
            conic_weights: Vec::new(),
            fill_rule: WindingRule::Nonzero,
            has_only_finite_points: true,
            has_current_point: false,
            last_move_to: FloatPoint::default(),
        }
    }

    pub fn verbs(&self) -> &[PathVerb] {
        &self.verbs
    }

    pub fn points(&self) -> &[FloatPoint] {
        &self.points
    }

    pub fn conic_weights(&self) -> &[f32] {
        &self.conic_weights
    }

    pub fn fill_rule(&self) -> WindingRule {
        self.fill_rule
    }

    pub fn set_fill_rule(&mut self, fill_rule: WindingRule) {
        self.fill_rule = fill_rule;
    }

    /// A path with a point that is not finite has no geometry: it draws nothing, has no length and
    /// contains no point. It still remembers its operations, so that drawing can continue from it.
    pub fn has_geometry(&self) -> bool {
        self.has_only_finite_points && !self.verbs.is_empty()
    }

    /// Whether the path has no current point, which Gfx::Path::is_empty() calls empty.
    pub fn is_empty(&self) -> bool {
        !self.has_current_point
    }

    pub fn last_point(&self) -> FloatPoint {
        self.points.last().copied().unwrap_or_default()
    }

    pub fn clear(&mut self) {
        *self = Self::new();
    }

    /// The elements of the path. A move that ends the path starts nothing, and is left out.
    pub fn elements(&self) -> PathElements<'_> {
        PathElements {
            geometry: self,
            verb_index: 0,
            point_index: 0,
            conic_weight_index: 0,
            current_point: FloatPoint::default(),
            contour_start: FloatPoint::default(),
        }
    }

    /// The bounds of the points of the path, as x, y, width and height. A move that ends the path,
    /// and that so starts nothing, does not count, unless it is the only point.
    pub fn bounding_box(&self) -> [f32; 4] {
        if self.points.is_empty() || !self.has_only_finite_points {
            return [0.0; 4];
        }
        let mut counted_points = self.points.as_slice();
        if counted_points.len() > 1 && self.verbs.last() == Some(&PathVerb::Move) {
            counted_points = &counted_points[..counted_points.len() - 1];
        }
        let first = counted_points[0];
        let (mut left, mut top, mut right, mut bottom) = (first.x, first.y, first.x, first.y);
        for point in &counted_points[1..] {
            left = left.min(point.x);
            top = top.min(point.y);
            right = right.max(point.x);
            bottom = bottom.max(point.y);
        }
        [left, top, right - left, bottom - top]
    }

    // Contour building, without the rules of Gfx::Path for a path that has no current point.

    fn push_point(&mut self, point: FloatPoint) {
        self.has_only_finite_points &= is_finite_point(point);
        self.points.push(point);
    }

    fn pop_point(&mut self) {
        if let Some(point) = self.points.pop()
            && !is_finite_point(point)
        {
            self.recompute_finiteness();
        }
    }

    /// Starts a contour. A move that follows a move replaces it.
    pub fn contour_move_to(&mut self, point: FloatPoint) {
        if self.verbs.last() == Some(&PathVerb::Move) {
            self.pop_point();
            self.push_point(point);
            return;
        }
        self.verbs.push(PathVerb::Move);
        self.push_point(point);
    }

    /// A segment needs a contour to belong to. After a close, the next contour starts where the
    /// closed one did.
    fn start_contour_for_segment(&mut self) {
        match self.verbs.last() {
            None => self.contour_move_to(FloatPoint::default()),
            Some(PathVerb::Close) => self.contour_move_to(self.start_of_last_contour()),
            Some(_) => {}
        }
    }

    fn start_of_last_contour(&self) -> FloatPoint {
        let mut point_index = 0;
        let mut start = FloatPoint::default();
        for verb in &self.verbs {
            if *verb == PathVerb::Move {
                start = self.points[point_index];
            }
            point_index += verb.stored_point_count();
        }
        start
    }

    pub fn contour_line_to(&mut self, point: FloatPoint) {
        self.start_contour_for_segment();
        self.verbs.push(PathVerb::Line);
        self.push_point(point);
    }

    pub fn contour_quad_to(&mut self, control: FloatPoint, point: FloatPoint) {
        self.start_contour_for_segment();
        self.verbs.push(PathVerb::Quad);
        self.push_point(control);
        self.push_point(point);
    }

    /// A conic with a weight of 1 is a quadratic curve, and is stored as one.
    pub fn contour_conic_to(&mut self, control: FloatPoint, point: FloatPoint, weight: f32) {
        if weight == 1.0 {
            self.contour_quad_to(control, point);
            return;
        }
        self.start_contour_for_segment();
        self.verbs.push(PathVerb::Conic);
        self.push_point(control);
        self.push_point(point);
        self.conic_weights.push(weight);
    }

    pub fn contour_cubic_to(&mut self, control1: FloatPoint, control2: FloatPoint, point: FloatPoint) {
        self.start_contour_for_segment();
        self.verbs.push(PathVerb::Cubic);
        self.push_point(control1);
        self.push_point(control2);
        self.push_point(point);
    }

    pub fn contour_close(&mut self) {
        if matches!(self.verbs.last(), None | Some(PathVerb::Close)) {
            return;
        }
        self.verbs.push(PathVerb::Close);
    }

    // The operations of Gfx::Path.

    pub fn move_to(&mut self, point: FloatPoint) {
        self.last_move_to = point;
        self.has_current_point = true;
        self.contour_move_to(point);
    }

    pub fn line_to(&mut self, point: FloatPoint) {
        if !self.has_current_point {
            self.move_to(point);
            return;
        }
        self.contour_line_to(point);
    }

    pub fn quadratic_bezier_curve_to(&mut self, control: FloatPoint, point: FloatPoint) {
        if !self.has_current_point {
            self.move_to(control);
        }
        self.contour_quad_to(control, point);
    }

    pub fn cubic_bezier_curve_to(&mut self, control1: FloatPoint, control2: FloatPoint, point: FloatPoint) {
        if !self.has_current_point {
            self.move_to(control1);
        }
        self.contour_cubic_to(control1, control2, point);
    }

    /// An arc of an ellipse, with the parameters of the SVG path command. The rotation is in radians.
    pub fn elliptical_arc_to(
        &mut self,
        point: FloatPoint,
        radii: (f32, f32),
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
    ) {
        if !self.has_current_point {
            self.move_to(point);
            return;
        }
        arcs::append_elliptical_arc(self, point, radii, x_axis_rotation, large_arc, sweep);
    }

    pub fn arc_to(&mut self, point: FloatPoint, radius: f32, large_arc: bool, sweep: bool) {
        self.elliptical_arc_to(point, (radius, radius), 0.0, large_arc, sweep);
    }

    /// Closes the subpath, and starts the next one where it started.
    pub fn close(&mut self) {
        if !self.has_current_point {
            return;
        }
        self.contour_close();
        self.contour_move_to(self.last_move_to);
    }

    /// Adds the subpaths of the other path. A move that ends this path starts nothing, so it does
    /// not stay in front of the other path's first move. A path without geometry adds no subpaths,
    /// but what it remembers for the operations that follow carries over.
    pub fn append_path(&mut self, other: &PathGeometry) {
        if other.has_only_finite_points {
            self.append_mapped(other, |point| point);
        }
        if other.has_current_point {
            self.has_current_point = true;
            self.last_move_to = other.last_move_to;
        }
    }

    /// Adds the contours of the other path with its points mapped, as for glyph outlines. The state of
    /// Gfx::Path is left as it was; update_state_from_contours() sets it.
    pub fn append_mapped(&mut self, other: &PathGeometry, map: impl Fn(FloatPoint) -> FloatPoint) {
        self.append_mapped_contours(&other.verbs, &other.points, &other.conic_weights, map);
    }

    /// Adds contours given as the verbs, points and conic weights of a path, with their points mapped, as
    /// append_mapped() does.
    pub fn append_mapped_contours(
        &mut self,
        verbs: &[PathVerb],
        points: &[FloatPoint],
        conic_weights: &[f32],
        map: impl Fn(FloatPoint) -> FloatPoint,
    ) {
        if verbs.is_empty() {
            return;
        }
        if self.verbs.last() == Some(&PathVerb::Move) {
            self.verbs.pop();
            self.pop_point();
        }
        let first_appended_point = self.points.len();
        self.verbs.extend_from_slice(verbs);
        self.points.extend(points.iter().map(|point| map(*point)));
        self.conic_weights.extend_from_slice(conic_weights);
        self.has_only_finite_points &= self.points[first_appended_point..]
            .iter()
            .all(|point| is_finite_point(*point));
    }

    /// Sets the state of Gfx::Path from the contours, for a path that was not built by its operations.
    pub fn update_state_from_contours(&mut self) {
        self.has_current_point = !self.verbs.is_empty();
        self.last_move_to = self.start_of_last_contour();
    }

    /// A copy with every point mapped by the transform. Conics keep their weights.
    pub fn copy_transformed(&self, transform: AffineTransform) -> PathGeometry {
        let mut copy = self.clone();
        copy.transform(transform);
        copy
    }

    pub fn transform(&mut self, transform: AffineTransform) {
        for point in &mut self.points {
            *point = transform.map_point(*point);
        }
        if self.has_current_point {
            self.last_move_to = transform.map_point(self.last_move_to);
        }
        self.recompute_finiteness();
    }

    fn recompute_finiteness(&mut self) {
        self.has_only_finite_points = self.points.iter().all(|point| is_finite_point(*point));
    }

    /// Makes a path from contours that a decoder checked: each verb has its points and weights, and
    /// every point is finite.
    pub(super) fn from_checked_contours(
        verbs: Vec<PathVerb>,
        points: Vec<FloatPoint>,
        conic_weights: Vec<f32>,
        fill_rule: WindingRule,
    ) -> Self {
        let mut geometry = Self {
            verbs,
            points,
            conic_weights,
            fill_rule,
            has_only_finite_points: true,
            has_current_point: false,
            last_move_to: FloatPoint::default(),
        };
        geometry.update_state_from_contours();
        geometry
    }

    /// Whether both paths have the same fill rule and the same contours, point for point.
    pub fn has_same_contours(&self, other: &PathGeometry) -> bool {
        let same_bits = |a: f32, b: f32| a.to_bits() == b.to_bits();
        self.fill_rule == other.fill_rule
            && self.verbs == other.verbs
            && self.points.len() == other.points.len()
            && self
                .points
                .iter()
                .zip(&other.points)
                .all(|(a, b)| same_bits(a.x, b.x) && same_bits(a.y, b.y))
            && self.conic_weights.len() == other.conic_weights.len()
            && self
                .conic_weights
                .iter()
                .zip(&other.conic_weights)
                .all(|(a, b)| same_bits(*a, *b))
    }
}

pub struct PathElements<'a> {
    geometry: &'a PathGeometry,
    verb_index: usize,
    point_index: usize,
    conic_weight_index: usize,
    current_point: FloatPoint,
    contour_start: FloatPoint,
}

impl Iterator for PathElements<'_> {
    type Item = PathElement;

    fn next(&mut self) -> Option<PathElement> {
        let verbs = &self.geometry.verbs;
        let verb = *verbs.get(self.verb_index)?;
        if verb == PathVerb::Move && self.verb_index + 1 == verbs.len() {
            return None;
        }
        self.verb_index += 1;
        let points = &self.geometry.points[self.point_index..self.point_index + verb.stored_point_count()];
        self.point_index += verb.stored_point_count();
        let start = self.current_point;
        let element = match verb {
            PathVerb::Move => {
                self.contour_start = points[0];
                PathElement::MoveTo(points[0])
            }
            PathVerb::Line => PathElement::Segment(Segment::Line([start, points[0]])),
            PathVerb::Quad => PathElement::Segment(Segment::Quad([start, points[0], points[1]])),
            PathVerb::Conic => {
                let weight = self.geometry.conic_weights[self.conic_weight_index];
                self.conic_weight_index += 1;
                PathElement::Segment(Segment::Conic([start, points[0], points[1]], weight))
            }
            PathVerb::Cubic => PathElement::Segment(Segment::Cubic([start, points[0], points[1], points[2]])),
            PathVerb::Close => PathElement::Close([start, self.contour_start]),
        };
        self.current_point = match element {
            PathElement::MoveTo(point) => point,
            PathElement::Segment(segment) => segment.end(),
            PathElement::Close(_) => self.contour_start,
        };
        Some(element)
    }
}
