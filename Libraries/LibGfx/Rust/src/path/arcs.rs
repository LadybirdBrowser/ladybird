/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Elliptical arcs as conics.
//!
//! https://www.w3.org/TR/SVG2/implnote.html#ArcConversionEndpointToCenter
//!
//! The arc goes from the endpoint parameterization of the SVG path command to a center, radii and
//! a range of angles on the ellipse. A conic is exactly an arc of an ellipse: on the unit circle, the
//! arc from angle a to angle a + d has its ends as the end points, the point where the tangents at
//! the ends meet, (cos(a + d / 2), sin(a + d / 2)) / cos(d / 2), as the control point, and cos(d / 2)
//! as the weight. Mapping the three points onto the ellipse keeps the weight.

use super::geometry::PathGeometry;
use crate::geometry::FloatPoint;
use std::f64::consts::PI;

/// The largest angle of the ellipse that one conic covers.
const LARGEST_ANGLE_OF_ONE_CONIC: f64 = 2.0 * PI / 3.0;

pub(super) fn append_elliptical_arc(
    geometry: &mut PathGeometry,
    end: FloatPoint,
    radii: (f32, f32),
    x_axis_rotation: f32,
    large_arc: bool,
    sweep: bool,
) {
    if ![end.x, end.y, radii.0, radii.1, x_axis_rotation]
        .iter()
        .all(|value| value.is_finite())
    {
        return;
    }

    let start = geometry.last_point();
    let (mut radius_x, mut radius_y) = (f64::from(radii.0.abs()), f64::from(radii.1.abs()));
    if start == end || radius_x == 0.0 || radius_y == 0.0 {
        geometry.contour_line_to(end);
        return;
    }

    let (sin_rotation, cos_rotation) = f64::from(x_axis_rotation).sin_cos();
    let (start_x, start_y) = (f64::from(start.x), f64::from(start.y));
    let (end_x, end_y) = (f64::from(end.x), f64::from(end.y));

    // The start point in a frame centered between the end points, with the axes of the ellipse.
    let half_difference_x = (start_x - end_x) / 2.0;
    let half_difference_y = (start_y - end_y) / 2.0;
    let start_in_frame_x = cos_rotation * half_difference_x + sin_rotation * half_difference_y;
    let start_in_frame_y = -sin_rotation * half_difference_x + cos_rotation * half_difference_y;

    // Radii too small for the ellipse to reach both points grow until it just does.
    let radii_scale = (start_in_frame_x * start_in_frame_x) / (radius_x * radius_x)
        + (start_in_frame_y * start_in_frame_y) / (radius_y * radius_y);
    if radii_scale > 1.0 {
        let scale = radii_scale.sqrt();
        radius_x *= scale;
        radius_y *= scale;
    }

    let rx_squared = radius_x * radius_x;
    let ry_squared = radius_y * radius_y;
    let x_squared = start_in_frame_x * start_in_frame_x;
    let y_squared = start_in_frame_y * start_in_frame_y;
    let numerator = rx_squared * ry_squared - rx_squared * y_squared - ry_squared * x_squared;
    let denominator = rx_squared * y_squared + ry_squared * x_squared;
    let mut center_factor = (numerator / denominator).max(0.0).sqrt();
    if large_arc == sweep {
        center_factor = -center_factor;
    }
    let center_in_frame_x = center_factor * radius_x * start_in_frame_y / radius_y;
    let center_in_frame_y = -center_factor * radius_y * start_in_frame_x / radius_x;
    let center_x = cos_rotation * center_in_frame_x - sin_rotation * center_in_frame_y + (start_x + end_x) / 2.0;
    let center_y = sin_rotation * center_in_frame_x + cos_rotation * center_in_frame_y + (start_y + end_y) / 2.0;

    // The angles of both end points on the unit circle that the ellipse is a mapping of.
    let start_on_circle = (
        (start_in_frame_x - center_in_frame_x) / radius_x,
        (start_in_frame_y - center_in_frame_y) / radius_y,
    );
    let end_on_circle = (
        (-start_in_frame_x - center_in_frame_x) / radius_x,
        (-start_in_frame_y - center_in_frame_y) / radius_y,
    );
    let start_angle = start_on_circle.1.atan2(start_on_circle.0);
    let mut sweep_angle = (start_on_circle.0 * end_on_circle.1 - start_on_circle.1 * end_on_circle.0)
        .atan2(start_on_circle.0 * end_on_circle.0 + start_on_circle.1 * end_on_circle.1);
    if !sweep && sweep_angle > 0.0 {
        sweep_angle -= 2.0 * PI;
    } else if sweep && sweep_angle < 0.0 {
        sweep_angle += 2.0 * PI;
    }
    if !(start_angle.is_finite() && sweep_angle.is_finite() && center_x.is_finite() && center_y.is_finite()) {
        geometry.contour_line_to(end);
        return;
    }

    let to_ellipse = |unit_x: f64, unit_y: f64| {
        let x = radius_x * snap_to_axis(unit_x);
        let y = radius_y * snap_to_axis(unit_y);
        FloatPoint {
            x: (center_x + cos_rotation * x - sin_rotation * y) as f32,
            y: (center_y + sin_rotation * x + cos_rotation * y) as f32,
        }
    };

    let conic_count = ((sweep_angle.abs() / LARGEST_ANGLE_OF_ONE_CONIC) - 1e-9)
        .ceil()
        .max(1.0) as usize;
    let angle_per_conic = sweep_angle / conic_count as f64;
    let half_angle_cosine = (angle_per_conic / 2.0).cos();
    let weight = half_angle_cosine as f32;
    for index in 0..conic_count {
        let from_angle = start_angle + angle_per_conic * index as f64;
        let middle_angle = from_angle + angle_per_conic / 2.0;
        let to_angle = from_angle + angle_per_conic;
        let control = to_ellipse(
            middle_angle.cos() / half_angle_cosine,
            middle_angle.sin() / half_angle_cosine,
        );
        let conic_end = if index + 1 == conic_count {
            end
        } else {
            to_ellipse(to_angle.cos(), to_angle.sin())
        };
        geometry.contour_conic_to(control, conic_end, weight);
    }
}

/// A coordinate on the unit circle that is 0, 1 or -1 but for rounding, as at the ends of quarter
/// circles, is made exact, so that arcs along the axes have exact points.
fn snap_to_axis(value: f64) -> f64 {
    const ROUNDING_TOLERANCE: f64 = 1e-12;
    for exact in [0.0, 1.0, -1.0] {
        if (value - exact).abs() < ROUNDING_TOLERANCE {
            return exact;
        }
    }
    value
}
