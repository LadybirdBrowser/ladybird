/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The outlines that masks of layers clip by, as CoreGraphics paths: that of a path a sublayer is clipped by, and that
//! of a rect whose corners are rounded by quarters of ellipses.

use super::frameworks::{
    CGMutablePathRef, CGPathAddCurveToPoint, CGPathAddLineToPoint, CGPathAddQuadCurveToPoint, CGPathCloseSubpath,
    CGPathCreateMutable, CGPathMoveToPoint, CGPathRef, CGSize, CfRetained,
};
use libgfx_rust::path::{Path, PathVerb};
use libgfx_rust::{CornerRadii, CornerRadius, FloatPoint};

/// A CoreGraphics path of which this holds one reference.
pub(super) struct CoreGraphicsPath(CfRetained);

impl CoreGraphicsPath {
    fn new() -> Self {
        // SAFETY: The answer is a new path the caller owns.
        Self(unsafe { CfRetained::adopt(CGPathCreateMutable().cast_const()) }.expect("a new path"))
    }

    pub(super) fn as_ptr(&self) -> CGPathRef {
        self.0.as_ptr()
    }

    fn as_mut_ptr(&self) -> CGMutablePathRef {
        self.0.as_mut_ptr()
    }

    fn move_to(&self, x: f64, y: f64) {
        // SAFETY: The path is live, and no transform is applied.
        unsafe { CGPathMoveToPoint(self.as_mut_ptr(), std::ptr::null(), x, y) };
    }

    fn line_to(&self, x: f64, y: f64) {
        // SAFETY: As above.
        unsafe { CGPathAddLineToPoint(self.as_mut_ptr(), std::ptr::null(), x, y) };
    }

    fn quad_to(&self, control: FloatPoint, end: FloatPoint) {
        // SAFETY: As above.
        unsafe {
            CGPathAddQuadCurveToPoint(
                self.as_mut_ptr(),
                std::ptr::null(),
                f64::from(control.x),
                f64::from(control.y),
                f64::from(end.x),
                f64::from(end.y),
            );
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn cubic_to(&self, x1: f64, y1: f64, x2: f64, y2: f64, x: f64, y: f64) {
        // SAFETY: As above.
        unsafe { CGPathAddCurveToPoint(self.as_mut_ptr(), std::ptr::null(), x1, y1, x2, y2, x, y) };
    }

    fn close(&self) {
        // SAFETY: As above.
        unsafe { CGPathCloseSubpath(self.as_mut_ptr()) };
    }
}

// CoreGraphics has no conic curves, so a conic is drawn as quadratic curves that stray from it by at most this, in the
// units of the space of its path.
const CONIC_TOLERANCE_IN_UNITS_OF_PATH: f32 = 0.1;
const MOST_HALVINGS_OF_CONIC_INTO_QUADRATIC_CURVES: u32 = 5;

/// How many times a conic is halved into quadratic curves for them to stay within the tolerance of it, by the bound on
/// how far a single quadratic curve strays from it. Each halving quarters how far the curves stray.
fn halvings_of_conic_into_quadratic_curves(
    start: FloatPoint,
    control: FloatPoint,
    end: FloatPoint,
    weight: f32,
) -> u32 {
    let weight_past_parabola = weight - 1.0;
    let factor = weight_past_parabola / (4.0 * (2.0 + weight_past_parabola));
    let x = factor * (start.x - 2.0 * control.x + end.x);
    let y = factor * (start.y - 2.0 * control.y + end.y);
    let mut distance_strayed = (x * x + y * y).sqrt();
    let mut halvings = 0;
    while halvings < MOST_HALVINGS_OF_CONIC_INTO_QUADRATIC_CURVES && distance_strayed > CONIC_TOLERANCE_IN_UNITS_OF_PATH
    {
        distance_strayed *= 0.25;
        halvings += 1;
    }
    halvings
}

#[derive(Clone, Copy)]
struct Conic {
    points: [FloatPoint; 3],
    weight: f32,
}

fn point(x: f32, y: f32) -> FloatPoint {
    FloatPoint { x, y }
}

fn is_finite(point: FloatPoint) -> bool {
    point.x.is_finite() && point.y.is_finite()
}

impl Conic {
    /// The two halves of the conic, split where its parameter is a half.
    fn halved(&self) -> [Conic; 2] {
        let [p0, p1, p2] = self.points;
        let weight = self.weight;
        let scale = 1.0 / (1.0 + weight);
        let weighted_p1 = point(weight * p1.x, weight * p1.y);
        let mut middle = point(
            (p0.x + 2.0 * weighted_p1.x + p2.x) * scale * 0.5,
            (p0.y + 2.0 * weighted_p1.y + p2.y) * scale * 0.5,
        );
        if !is_finite(middle) {
            let weight = f64::from(weight);
            let weight_twice = weight * 2.0;
            let scale_half = 1.0 / (1.0 + weight) * 0.5;
            let coordinate = |a: f32, b: f32, c: f32| {
                ((f64::from(a) + weight_twice * f64::from(b) + f64::from(c)) * scale_half) as f32
            };
            middle = point(coordinate(p0.x, p1.x, p2.x), coordinate(p0.y, p1.y, p2.y));
        }
        let halved_weight = (0.5 + weight * 0.5).sqrt();
        [
            Conic {
                points: [
                    p0,
                    point((p0.x + weighted_p1.x) * scale, (p0.y + weighted_p1.y) * scale),
                    middle,
                ],
                weight: halved_weight,
            },
            Conic {
                points: [
                    middle,
                    point((weighted_p1.x + p2.x) * scale, (weighted_p1.y + p2.y) * scale),
                    p2,
                ],
                weight: halved_weight,
            },
        ]
    }

    /// Appends the control and end points of the quadratic curves of `2^level` parts of the conic.
    fn append_quadratic_curves(&self, level: u32, points: &mut Vec<FloatPoint>) {
        if level == 0 {
            points.push(self.points[1]);
            points.push(self.points[2]);
            return;
        }
        let mut halves = self.halved();
        let start_y = self.points[0].y;
        let end_y = self.points[2].y;
        let between = |a: f32, b: f32, c: f32| (a - b) * (c - b) <= 0.0;
        // Halves of a conic that is monotonic in y stay so.
        if between(start_y, self.points[1].y, end_y) {
            let middle_y = halves[0].points[2].y;
            if !between(start_y, middle_y, end_y) {
                let closer_y = if (middle_y - start_y).abs() < (middle_y - end_y).abs() {
                    start_y
                } else {
                    end_y
                };
                halves[0].points[2].y = closer_y;
                halves[1].points[0].y = closer_y;
            }
            if !between(start_y, halves[0].points[1].y, halves[0].points[2].y) {
                halves[0].points[1].y = start_y;
            }
            if !between(halves[1].points[0].y, halves[1].points[1].y, end_y) {
                halves[1].points[1].y = end_y;
            }
        }
        halves[0].append_quadratic_curves(level - 1, points);
        halves[1].append_quadratic_curves(level - 1, points);
    }

    /// The control and end points of the quadratic curves that `2^halvings` parts of the conic come closest to, as
    /// Skia's `SkConic::chopIntoQuadsPOW2()` makes them.
    fn quadratic_curves(&self, mut halvings: u32) -> Vec<FloatPoint> {
        let mut points = Vec::with_capacity(2 << halvings);
        let coincide = |a: FloatPoint, b: FloatPoint| {
            let (dx, dy) = (a.x - b.x, a.y - b.y);
            !(dx.is_finite() && dy.is_finite() && (dx != 0.0 || dy != 0.0))
        };
        let mut halved_into_lines = false;
        if halvings == MOST_HALVINGS_OF_CONIC_INTO_QUADRATIC_CURVES {
            let halves = self.halved();
            if coincide(halves[0].points[1], halves[0].points[2]) && coincide(halves[1].points[0], halves[1].points[1])
            {
                let control = halves[0].points[1];
                points.extend([control, control, control, halves[1].points[2]]);
                halvings = 1;
                halved_into_lines = true;
            }
        }
        if !halved_into_lines {
            self.append_quadratic_curves(halvings, &mut points);
        }
        // Points that are not finite are pinned to the control point of the conic; its ends stay where they are.
        if !points.iter().all(|point| is_finite(*point)) {
            let last = points.len() - 1;
            for point in &mut points[..last] {
                *point = self.points[1];
            }
        }
        points
    }
}

/// The outline of the path in its own space.
pub(super) fn outline_of_path(path: &Path) -> CoreGraphicsPath {
    let outline = CoreGraphicsPath::new();
    let mut points = path.points().iter().copied();
    let mut conic_weights = path.conic_weights().iter().copied();
    let mut next_point = || points.next().unwrap_or_default();
    let mut current_point = FloatPoint::default();
    let mut start_of_subpath = FloatPoint::default();
    for verb in path.verbs() {
        match verb {
            PathVerb::Move => {
                let point = next_point();
                outline.move_to(f64::from(point.x), f64::from(point.y));
                current_point = point;
                start_of_subpath = point;
            }
            PathVerb::Line => {
                let point = next_point();
                outline.line_to(f64::from(point.x), f64::from(point.y));
                current_point = point;
            }
            PathVerb::Quad => {
                let control = next_point();
                let end = next_point();
                outline.quad_to(control, end);
                current_point = end;
            }
            PathVerb::Conic => {
                let control = next_point();
                let end = next_point();
                let weight = conic_weights.next().unwrap_or(1.0);
                let conic = Conic {
                    points: [current_point, control, end],
                    weight,
                };
                let halvings = halvings_of_conic_into_quadratic_curves(current_point, control, end, weight);
                for [control, end] in conic.quadratic_curves(halvings).as_chunks::<2>().0 {
                    outline.quad_to(*control, *end);
                }
                current_point = end;
            }
            PathVerb::Cubic => {
                let first_control = next_point();
                let second_control = next_point();
                let end = next_point();
                outline.cubic_to(
                    f64::from(first_control.x),
                    f64::from(first_control.y),
                    f64::from(second_control.x),
                    f64::from(second_control.y),
                    f64::from(end.x),
                    f64::from(end.y),
                );
                current_point = end;
            }
            PathVerb::Close => {
                outline.close();
                current_point = start_of_subpath;
            }
        }
    }
    outline
}

/// The outline of a rect of the size whose corners are each rounded by a quarter of an ellipse, in the coordinates of a
/// layer whose y-axis points down.
pub(super) fn outline_of_rect_with_rounded_corners(
    size: CGSize,
    corner_radii: &CornerRadii,
    device_pixel_ratio: f64,
) -> CoreGraphicsPath {
    // How far along a radius the control points of the cubic curve nearest a quarter of an ellipse are.
    const CONTROL_POINT_DISTANCE_ALONG_RADIUS: f64 = 0.552_284_749_8;
    let radii_in_points = |corner: CornerRadius| {
        if !corner.is_present() {
            return CGSize::default();
        }
        CGSize {
            width: (f64::from(corner.horizontal_radius) / device_pixel_ratio).min(size.width),
            height: (f64::from(corner.vertical_radius) / device_pixel_ratio).min(size.height),
        }
    };
    let top_left = radii_in_points(corner_radii.top_left);
    let top_right = radii_in_points(corner_radii.top_right);
    let bottom_right = radii_in_points(corner_radii.bottom_right);
    let bottom_left = radii_in_points(corner_radii.bottom_left);
    let to_control_point = |radius: f64| radius * (1.0 - CONTROL_POINT_DISTANCE_ALONG_RADIUS);
    let (width, height) = (size.width, size.height);
    let outline = CoreGraphicsPath::new();
    outline.move_to(top_left.width, 0.0);
    outline.line_to(width - top_right.width, 0.0);
    outline.cubic_to(
        width - to_control_point(top_right.width),
        0.0,
        width,
        to_control_point(top_right.height),
        width,
        top_right.height,
    );
    outline.line_to(width, height - bottom_right.height);
    outline.cubic_to(
        width,
        height - to_control_point(bottom_right.height),
        width - to_control_point(bottom_right.width),
        height,
        width - bottom_right.width,
        height,
    );
    outline.line_to(bottom_left.width, height);
    outline.cubic_to(
        to_control_point(bottom_left.width),
        height,
        0.0,
        height - to_control_point(bottom_left.height),
        0.0,
        height - bottom_left.height,
    );
    outline.line_to(0.0, top_left.height);
    outline.cubic_to(
        0.0,
        to_control_point(top_left.height),
        to_control_point(top_left.width),
        0.0,
        top_left.width,
        0.0,
    );
    outline.close();
    outline
}
