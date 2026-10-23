/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Points, tangents and subdivisions of the segments of a path, computed in double precision.

use super::geometry::Segment;
use crate::geometry::FloatPoint;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}

impl Point {
    pub fn new(x: f64, y: f64) -> Self {
        Self { x, y }
    }

    pub fn from_float(point: FloatPoint) -> Self {
        Self::new(f64::from(point.x), f64::from(point.y))
    }

    pub fn to_float(self) -> FloatPoint {
        FloatPoint {
            x: self.x as f32,
            y: self.y as f32,
        }
    }

    pub fn plus(self, other: Self) -> Self {
        Self::new(self.x + other.x, self.y + other.y)
    }

    pub fn minus(self, other: Self) -> Self {
        Self::new(self.x - other.x, self.y - other.y)
    }

    pub fn scaled(self, factor: f64) -> Self {
        Self::new(self.x * factor, self.y * factor)
    }

    pub fn length(self) -> f64 {
        self.x.hypot(self.y)
    }

    pub fn distance_to(self, other: Self) -> f64 {
        self.minus(other).length()
    }

    pub fn lerp(self, other: Self, t: f64) -> Self {
        Self::new(self.x + (other.x - self.x) * t, self.y + (other.y - self.y) * t)
    }
}

/// A segment with its points in double precision. A line is a curve of degree one.
#[derive(Clone, Copy, Debug)]
pub enum Curve {
    Line([Point; 2]),
    Quad([Point; 3]),
    Conic([Point; 3], f64),
    Cubic([Point; 4]),
}

impl Curve {
    pub fn from_segment(segment: &Segment) -> Self {
        let convert = Point::from_float;
        match segment {
            Segment::Line(points) => Self::Line(points.map(convert)),
            Segment::Quad(points) => Self::Quad(points.map(convert)),
            Segment::Conic(points, weight) => Self::Conic(points.map(convert), f64::from(*weight)),
            Segment::Cubic(points) => Self::Cubic(points.map(convert)),
        }
    }

    pub fn control_points(&self) -> &[Point] {
        match self {
            Self::Line(points) => points,
            Self::Quad(points) | Self::Conic(points, _) => points,
            Self::Cubic(points) => points,
        }
    }

    pub fn start(&self) -> Point {
        self.control_points()[0]
    }

    pub fn end(&self) -> Point {
        *self.control_points().last().expect("a curve has points")
    }

    pub fn point_at(&self, t: f64) -> Point {
        let u = 1.0 - t;
        match *self {
            Self::Line([p0, p1]) => p0.lerp(p1, t),
            Self::Quad([p0, p1, p2]) => {
                let (a, b, c) = (u * u, 2.0 * u * t, t * t);
                Point::new(a * p0.x + b * p1.x + c * p2.x, a * p0.y + b * p1.y + c * p2.y)
            }
            Self::Conic([p0, p1, p2], weight) => {
                let (a, b, c) = (u * u, 2.0 * weight * u * t, t * t);
                let denominator = a + b + c;
                Point::new(
                    (a * p0.x + b * p1.x + c * p2.x) / denominator,
                    (a * p0.y + b * p1.y + c * p2.y) / denominator,
                )
            }
            Self::Cubic([p0, p1, p2, p3]) => {
                let (a, b, c, d) = (u * u * u, 3.0 * u * u * t, 3.0 * u * t * t, t * t * t);
                Point::new(
                    a * p0.x + b * p1.x + c * p2.x + d * p3.x,
                    a * p0.y + b * p1.y + c * p2.y + d * p3.y,
                )
            }
        }
    }

    /// The derivative of the curve with respect to its parameter.
    pub fn derivative_at(&self, t: f64) -> Point {
        let u = 1.0 - t;
        match *self {
            Self::Line([p0, p1]) => p1.minus(p0),
            Self::Quad([p0, p1, p2]) => p1.minus(p0).scaled(2.0 * u).plus(p2.minus(p1).scaled(2.0 * t)),
            Self::Conic([p0, p1, p2], weight) => {
                // The quotient rule on numerator / denominator.
                let (a, b, c) = (u * u, 2.0 * weight * u * t, t * t);
                let (da, db, dc) = (-2.0 * u, 2.0 * weight * (u - t), 2.0 * t);
                let denominator = a + b + c;
                let denominator_derivative = da + db + dc;
                let numerator = Point::new(a * p0.x + b * p1.x + c * p2.x, a * p0.y + b * p1.y + c * p2.y);
                let numerator_derivative =
                    Point::new(da * p0.x + db * p1.x + dc * p2.x, da * p0.y + db * p1.y + dc * p2.y);
                numerator_derivative
                    .scaled(denominator)
                    .minus(numerator.scaled(denominator_derivative))
                    .scaled(1.0 / (denominator * denominator))
            }
            Self::Cubic([p0, p1, p2, p3]) => p1
                .minus(p0)
                .scaled(3.0 * u * u)
                .plus(p2.minus(p1).scaled(6.0 * u * t))
                .plus(p3.minus(p2).scaled(3.0 * t * t)),
        }
    }

    /// The direction in which the curve goes at the parameter. Where the derivative vanishes, as at
    /// an end whose control point coincides with it, the direction comes from the next points.
    pub fn direction_at(&self, t: f64) -> Option<Point> {
        let derivative = self.derivative_at(t);
        if derivative.length() > 0.0 {
            return Some(derivative);
        }
        let points = self.control_points();
        if t <= 0.5 {
            let start = points[0];
            points[1..]
                .iter()
                .map(|point| point.minus(start))
                .find(|vector| vector.length() > 0.0)
        } else {
            let end = points[points.len() - 1];
            points[..points.len() - 1]
                .iter()
                .rev()
                .map(|point| end.minus(*point))
                .find(|vector| vector.length() > 0.0)
        }
    }

    /// The parameters strictly between 0 and 1 where the curve turns in the given coordinate, in increasing
    /// order.
    pub fn turning_parameters(&self, coordinate: impl Fn(Point) -> f64) -> Vec<f64> {
        let mut roots = match *self {
            Self::Line(_) => Vec::new(),
            Self::Quad([p0, p1, p2]) => {
                let (v0, v1, v2) = (coordinate(p0), coordinate(p1), coordinate(p2));
                // The derivative is linear: (v1 - v0) + t (v0 - 2 v1 + v2), up to a factor of 2.
                solve_quadratic(0.0, v0 - 2.0 * v1 + v2, v1 - v0)
            }
            Self::Conic([p0, p1, p2], weight) => {
                // The derivative has the sign of N'(t) D(t) - N(t) D'(t), whose cubic terms cancel. With the
                // coordinate measured from p0, it is (w (v1 - v0)) + t (v2 - v0 - 2 w (v1 - v0))
                // + t^2 (w - 1) (v2 - v0) times 2.
                let (b, c) = (coordinate(p1) - coordinate(p0), coordinate(p2) - coordinate(p0));
                solve_quadratic((weight - 1.0) * c, c - 2.0 * weight * b, weight * b)
            }
            Self::Cubic([p0, p1, p2, p3]) => {
                let (v0, v1, v2, v3) = (coordinate(p0), coordinate(p1), coordinate(p2), coordinate(p3));
                // The derivative over 3 is a t^2 + b t + c.
                let a = v3 - 3.0 * v2 + 3.0 * v1 - v0;
                let b = 2.0 * (v2 - 2.0 * v1 + v0);
                let c = v1 - v0;
                solve_quadratic(a, b, c)
            }
        };
        roots.retain(|t| *t > 0.0 && *t < 1.0);
        roots.sort_by(f64::total_cmp);
        roots.dedup();
        roots
    }
}

/// The real roots of a t^2 + b t + c.
fn solve_quadratic(a: f64, b: f64, c: f64) -> Vec<f64> {
    if a == 0.0 {
        if b == 0.0 {
            return Vec::new();
        }
        return vec![-c / b];
    }
    let discriminant = b * b - 4.0 * a * c;
    if discriminant < 0.0 {
        return Vec::new();
    }
    // The form that avoids subtracting nearly equal values.
    let root = discriminant.sqrt();
    let q = -0.5 * (b + if b < 0.0 { -root } else { root });
    if q == 0.0 {
        return vec![0.0];
    }
    vec![q / a, c / q]
}
