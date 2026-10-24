/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The operations of the CanvasPath mixin, https://html.spec.whatwg.org/multipage/canvas.html#canvaspath, which
//! Path2D and the 2D contexts share. They build on a Path, with its rules for a path without a current point.
//!
//! The arithmetic is that of the C++ it replaces, in the same precision: the arguments of moveTo(), lineTo(),
//! quadraticCurveTo(), arc() and ellipse() are floats, and those of the rest are doubles that become floats where
//! they become points.

use libgfx_rust::{FloatPoint, FloatSize};

use crate::ak_format::ak_formatted;

use crate::geometry::dictionaries::DOMPointInit;

/// The union (unrestricted double or DOMPointInit) of HTML/Canvas/CanvasPath.idl, a radius of roundRect(), whose
/// members the bindings convert.
pub(crate) enum UnrestrictedDoubleOrDOMPointInit {
    UnrestrictedDouble(f64),
    DOMPointInit(DOMPointInit),
}

/// The union (unrestricted double or DOMPointInit or sequence<(unrestricted double or DOMPointInit)>) of
/// HTML/Canvas/CanvasPath.idl, the radii of roundRect(), whose members the bindings convert.
pub(crate) enum UnrestrictedDoubleOrDOMPointInitOrUnrestrictedDoubleOrDOMPointInitSequence {
    UnrestrictedDouble(f64),
    DOMPointInit(DOMPointInit),
    Sequence(Vec<UnrestrictedDoubleOrDOMPointInit>),
}

impl From<UnrestrictedDoubleOrDOMPointInit> for RoundRectRadius {
    fn from(radius: UnrestrictedDoubleOrDOMPointInit) -> Self {
        match radius {
            UnrestrictedDoubleOrDOMPointInit::UnrestrictedDouble(radius) => Self { x: radius, y: radius },
            UnrestrictedDoubleOrDOMPointInit::DOMPointInit(point) => Self { x: point.x, y: point.y },
        }
    }
}

/// The radii of roundRect() as a list, which step 2 of its steps makes of a single radius.
pub(crate) fn round_rect_radii(
    radii: UnrestrictedDoubleOrDOMPointInitOrUnrestrictedDoubleOrDOMPointInitSequence,
) -> Vec<RoundRectRadius> {
    use UnrestrictedDoubleOrDOMPointInitOrUnrestrictedDoubleOrDOMPointInitSequence as Radii;
    match radii {
        Radii::UnrestrictedDouble(radius) => vec![UnrestrictedDoubleOrDOMPointInit::UnrestrictedDouble(radius).into()],
        Radii::DOMPointInit(point) => vec![UnrestrictedDoubleOrDOMPointInit::DOMPointInit(point).into()],
        Radii::Sequence(radii) => radii.into_iter().map(RoundRectRadius::from).collect(),
    }
}

/// What the operations build on, which is a Path, or what the crate tests record.
pub(crate) trait PathSink {
    /// Whether the path has no current point.
    fn is_empty(&self) -> bool;
    fn last_point(&self) -> FloatPoint;
    fn move_to(&mut self, point: FloatPoint);
    fn line_to(&mut self, point: FloatPoint);
    fn quadratic_bezier_curve_to(&mut self, through: FloatPoint, point: FloatPoint);
    fn cubic_bezier_curve_to(&mut self, c1: FloatPoint, c2: FloatPoint, point: FloatPoint);
    fn elliptical_arc_to(
        &mut self,
        point: FloatPoint,
        radii: FloatSize,
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
    );
    fn arc_to(&mut self, point: FloatPoint, radius: f32, large_arc: bool, sweep: bool);
    fn close(&mut self);
}

impl PathSink for libgfx_rust::path::Path {
    fn is_empty(&self) -> bool {
        self.is_empty()
    }

    fn last_point(&self) -> FloatPoint {
        self.last_point()
    }

    fn move_to(&mut self, point: FloatPoint) {
        self.move_to(point);
    }

    fn line_to(&mut self, point: FloatPoint) {
        self.line_to(point);
    }

    fn quadratic_bezier_curve_to(&mut self, through: FloatPoint, point: FloatPoint) {
        self.quadratic_bezier_curve_to(through, point);
    }

    fn cubic_bezier_curve_to(&mut self, c1: FloatPoint, c2: FloatPoint, point: FloatPoint) {
        self.cubic_bezier_curve_to(c1, c2, point);
    }

    fn elliptical_arc_to(
        &mut self,
        point: FloatPoint,
        radii: FloatSize,
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
    ) {
        self.elliptical_arc_to(point, radii, x_axis_rotation, large_arc, sweep);
    }

    fn arc_to(&mut self, point: FloatPoint, radius: f32, large_arc: bool, sweep: bool) {
        self.arc_to(point, radius, large_arc, sweep);
    }

    fn close(&mut self) {
        self.close();
    }
}

/// What an operation throws.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum CanvasPathError {
    /// The radius of arc(), an IndexSizeError.
    NegativeArcRadius(f32),
    /// The radius of arcTo(), an IndexSizeError.
    NegativeArcToRadius(f64),
    /// The radiusX of ellipse(), an IndexSizeError.
    NegativeMajorAxisRadius(f32),
    /// The radiusY of ellipse(), an IndexSizeError.
    NegativeMinorAxisRadius(f32),
    /// More than four radii, or none, for roundRect(), a RangeError.
    RoundRectRadiusCount,
    /// A negative radius for roundRect(), a RangeError.
    NegativeRoundRectRadius,
}

impl CanvasPathError {
    /// Whether the exception is a RangeError rather than an "IndexSizeError" DOMException.
    pub(crate) fn is_range_error(self) -> bool {
        matches!(self, Self::RoundRectRadiusCount | Self::NegativeRoundRectRadius)
    }

    pub(crate) fn message(self) -> String {
        match self {
            Self::NegativeArcRadius(radius) => format!("The radius provided ({}) is negative.", ak_formatted(radius)),
            Self::NegativeArcToRadius(radius) => format!("The radius provided ({}) is negative.", ak_formatted(radius)),
            Self::NegativeMajorAxisRadius(radius) => {
                format!("The major-axis radius provided ({}) is negative.", ak_formatted(radius))
            }
            Self::NegativeMinorAxisRadius(radius) => {
                format!("The minor-axis radius provided ({}) is negative.", ak_formatted(radius))
            }
            Self::RoundRectRadiusCount => "roundRect: Can have between 1 and 4 radii".to_owned(),
            Self::NegativeRoundRectRadius => "roundRect: Radius can't be negative".to_owned(),
        }
    }
}

#[cfg(not(test))]
impl From<CanvasPathError> for crate::bindings::Exception {
    fn from(error: CanvasPathError) -> Self {
        let message = ak::Utf16String::from_utf8(&error.message());
        if error.is_range_error() {
            Self::RangeError(message)
        } else {
            Self::index_size_error(message)
        }
    }
}

fn point(x: f32, y: f32) -> FloatPoint {
    FloatPoint { x, y }
}

#[allow(
    clippy::cast_possible_truncation,
    reason = "a point of a Gfx::Path is in floats, as the C++ made it"
)]
fn point_from_doubles(x: f64, y: f64) -> FloatPoint {
    point(x as f32, y as f32)
}

fn ensure_subpath(path: &mut impl PathSink, x: f32, y: f32) {
    if path.is_empty() {
        path.move_to(point(x, y));
    }
}

fn add_line_to_start_point(path: &mut impl PathSink, start_point: FloatPoint) {
    if path.is_empty() {
        path.move_to(start_point);
    } else {
        path.line_to(start_point);
    }
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-closepath
pub(crate) fn close_path(path: &mut impl PathSink) {
    // The closePath() method, when invoked, must do nothing if the object's path has no subpaths. Otherwise, it must
    // mark the last subpath as closed, create a new subpath whose first point is the same as the previous subpath's
    // first point, and finally add this new subpath to the path.
    if path.is_empty() {
        return;
    }
    path.close();
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-moveto
pub(crate) fn move_to(path: &mut impl PathSink, x: f32, y: f32) {
    // 1. If either of the arguments are infinite or NaN, then return.
    if !x.is_finite() || !y.is_finite() {
        return;
    }

    // 2. Create a new subpath with the specified point as its first (and only) point.
    path.move_to(point(x, y));
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-lineto
pub(crate) fn line_to(path: &mut impl PathSink, x: f32, y: f32) {
    // 1. If either of the arguments are infinite or NaN, then return.
    if !x.is_finite() || !y.is_finite() {
        return;
    }

    if path.is_empty() {
        // 2. If the object's path has no subpaths, then ensure there is a subpath for (x, y).
        ensure_subpath(path, x, y);
    } else {
        // 3. Otherwise, connect the last point in the subpath to the given point (x, y) using a straight line, and
        //    then add the given point (x, y) to the subpath.
        path.line_to(point(x, y));
    }
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-quadraticcurveto
pub(crate) fn quadratic_curve_to(path: &mut impl PathSink, cpx: f32, cpy: f32, x: f32, y: f32) {
    // 1. If any of the arguments are infinite or NaN, then return.
    if !cpx.is_finite() || !cpy.is_finite() || !x.is_finite() || !y.is_finite() {
        return;
    }

    // 2. Ensure there is a subpath for (cpx, cpy).
    ensure_subpath(path, cpx, cpy);

    // 3. Connect the last point in the subpath to the given point (x, y) using a quadratic Bézier curve with control
    //    point (cpx, cpy).
    // 4. Add the given point (x, y) to the subpath.
    path.quadratic_bezier_curve_to(point(cpx, cpy), point(x, y));
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-beziercurveto
pub(crate) fn bezier_curve_to(path: &mut impl PathSink, cp1x: f64, cp1y: f64, cp2x: f64, cp2y: f64, x: f64, y: f64) {
    // 1. If any of the arguments are infinite or NaN, then return.
    if [cp1x, cp1y, cp2x, cp2y, x, y]
        .iter()
        .any(|argument| !argument.is_finite())
    {
        return;
    }

    // 2. Ensure there is a subpath for (cp1x, cp1y).
    let control_point_1 = point_from_doubles(cp1x, cp1y);
    ensure_subpath(path, control_point_1.x, control_point_1.y);

    // 3. Connect the last point in the subpath to the given point (x, y) using a cubic Bézier curve with control
    //    points (cp1x, cp1y) and (cp2x, cp2y).
    // 4. Add the point (x, y) to the subpath.
    path.cubic_bezier_curve_to(
        control_point_1,
        point_from_doubles(cp2x, cp2y),
        point_from_doubles(x, y),
    );
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arc
#[allow(clippy::too_many_arguments)]
pub(crate) fn arc(
    path: &mut impl PathSink,
    x: f32,
    y: f32,
    radius: f32,
    start_angle: f32,
    end_angle: f32,
    counterclockwise: bool,
) -> Result<(), CanvasPathError> {
    if radius < 0.0 && radius.is_finite() {
        return Err(CanvasPathError::NegativeArcRadius(radius));
    }
    ellipse(
        path,
        x,
        y,
        radius,
        radius,
        0.0,
        start_angle,
        end_angle,
        counterclockwise,
    )
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-ellipse
#[allow(clippy::too_many_arguments)]
pub(crate) fn ellipse(
    path: &mut impl PathSink,
    x: f32,
    y: f32,
    radius_x: f32,
    radius_y: f32,
    rotation: f32,
    mut start_angle: f32,
    mut end_angle: f32,
    mut counterclockwise: bool,
) -> Result<(), CanvasPathError> {
    // 1. If any of the arguments are infinite or NaN, then return.
    if [x, y, radius_x, radius_y, rotation, start_angle, end_angle]
        .iter()
        .any(|argument| !argument.is_finite())
    {
        return Ok(());
    }

    // 2. If either radiusX or radiusY are negative, then throw an "IndexSizeError" DOMException.
    if radius_x < 0.0 {
        return Err(CanvasPathError::NegativeMajorAxisRadius(radius_x));
    }
    if radius_y < 0.0 {
        return Err(CanvasPathError::NegativeMinorAxisRadius(radius_y));
    }

    if radius_x == 0.0 || radius_y == 0.0 {
        add_line_to_start_point(path, point(x, y));
        return Ok(());
    }

    // "If counterclockwise is false and endAngle − startAngle is greater than or equal to 2π, or, if counterclockwise
    // is true and startAngle − endAngle is greater than or equal to 2π, then the arc is the whole circumference of
    // this ellipse"
    // Also draw the full ellipse if making a non-zero whole number of turns.
    let tau = core::f32::consts::TAU;
    if (!counterclockwise && (end_angle - start_angle) >= tau)
        || (counterclockwise && (start_angle - end_angle) >= tau)
        || (start_angle != end_angle && (start_angle - end_angle) % tau == 0.0)
    {
        start_angle = 0.0;
        // FIXME: elliptical_arc_to() incorrectly handles the case where the start/end points are very close.
        //        So we slightly fudge the numbers here to correct for that.
        end_angle = tau * 0.9999;
        counterclockwise = false;
    } else {
        start_angle %= tau;
        end_angle %= tau;
    }

    // Then, figure out where the ends of the arc are.
    // To do so, we can pretend that the center of this ellipse is at (0, 0), and the whole coordinate system is
    // rotated `rotation` radians around the x axis, centered on `center`. The sign of the resulting relative
    // positions is just whether our angle is on one of the left quadrants.
    // NB: The C++ took the sine and the cosine of the rotation with the x87 instruction, in extended precision. Taken
    //     in double precision, they round to the same floats but in rare cases.
    let (sin_rotation, cos_rotation) = f64::from(rotation).sin_cos();
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the C++ kept the sine and the cosine in floats"
    )]
    let (sin_rotation, cos_rotation) = (sin_rotation as f32, cos_rotation as f32);

    let resolve_point_with_angle = |angle: f32| {
        let tan_relative = angle.tan();
        let tan2 = tan_relative * tan_relative;

        let ab = radius_x * radius_y;
        let a2 = radius_x * radius_x;
        let b2 = radius_y * radius_y;
        let sqrt = (b2 + a2 * tan2).sqrt();

        let mut relative_x_position = ab / sqrt;
        let mut relative_y_position = ab * tan_relative / sqrt;

        // Make sure to set the correct sign
        // -1 if 0 ≤ θ < 90° or 270°< θ ≤ 360°
        //  1 if 90° < θ< 270°
        let sn = if angle.cos() >= 0.0 { 1.0 } else { -1.0 };
        relative_x_position *= sn;
        relative_y_position *= sn;

        // Now rotate it (back) around the center point by 'rotation' radians, then move it back to our actual origin.
        let relative_rotated_x_position = relative_x_position * cos_rotation - relative_y_position * sin_rotation;
        let relative_rotated_y_position = relative_x_position * sin_rotation + relative_y_position * cos_rotation;
        point(relative_rotated_x_position + x, relative_rotated_y_position + y)
    };

    let start_point = resolve_point_with_angle(start_angle);
    let end_point = resolve_point_with_angle(end_angle);

    let mut delta_theta = if counterclockwise {
        start_angle - end_angle
    } else {
        end_angle - start_angle
    };

    if delta_theta < 0.0 {
        delta_theta += core::f32::consts::PI * 2.0;
    }

    // 3. If canvasPath's path has any subpaths, then add a straight line from the last point in the subpath to the
    //    start point of the arc.
    add_line_to_start_point(path, start_point);

    // 4. Add the start and end points of the arc to the subpath, and connect them with an arc.
    path.elliptical_arc_to(
        end_point,
        FloatSize {
            width: radius_x,
            height: radius_y,
        },
        rotation,
        delta_theta > core::f32::consts::PI,
        !counterclockwise,
    );

    Ok(())
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-arcto
pub(crate) fn arc_to(
    path: &mut impl PathSink,
    x1: f64,
    y1: f64,
    x2: f64,
    y2: f64,
    radius: f64,
) -> Result<(), CanvasPathError> {
    // 1. If any of the arguments are infinite or NaN, then return.
    if [x1, y1, x2, y2, radius].iter().any(|argument| !argument.is_finite()) {
        return Ok(());
    }

    // Point (x1, y1)
    let p1 = point_from_doubles(x1, y1);
    // Point (x2, y2)
    let p2 = point_from_doubles(x2, y2);

    // 2. Ensure there is a subpath for (x1, y1).
    ensure_subpath(path, p1.x, p1.y);

    // 3. If radius is negative, then throw an "IndexSizeError" DOMException.
    if radius < 0.0 {
        return Err(CanvasPathError::NegativeArcToRadius(radius));
    }

    // 4. Let the point (x0, y0) be the last point in the subpath, transformed by the inverse of the current
    //    transformation matrix (so that it is in the same coordinate system as the points passed to the method).
    // NB: A Path2D has no transformation matrix, and a context keeps its current default path in the coordinates of
    //     its current transformation matrix, so the last point is in that coordinate system already.
    // Point (x0, y0)
    let p0 = path.last_point();

    // 5. If the point (x0, y0) is equal to the point (x1, y1), or if the point (x1, y1) is equal to the point
    //    (x2, y2), or if radius is zero, then add the point (x1, y1) to the subpath, and connect that point to the
    //    previous point (x0, y0) by a straight line.
    if p0 == p1 || p1 == p2 || radius == 0.0 {
        path.line_to(p1);
        return Ok(());
    }

    let v1 = (p0.x - p1.x, p0.y - p1.y);
    let v2 = (p2.x - p1.x, p2.y - p1.y);
    let length = |vector: (f32, f32)| (vector.0 * vector.0 + vector.1 * vector.1).sqrt();
    let cos_theta = (v1.0 * v2.0 + v1.1 * v2.1) / (length(v1) * length(v2));
    // 6. Otherwise, if the points (x0, y0), (x1, y1), and (x2, y2) all lie on a single straight line, then add the
    //    point (x1, y1) to the subpath, and connect that point to the previous point (x0, y0) by a straight line.
    if cos_theta == -1.0 || cos_theta == 1.0 {
        path.line_to(p1);
        return Ok(());
    }

    // 7. Otherwise, let The Arc be the shortest arc given by circumference of the circle that has radius radius, and
    //    that has one point tangent to the half-infinite line that crosses the point (x0, y0) and ends at the point
    //    (x1, y1), and that has a different point tangent to the half-infinite line that ends at the point (x1, y1)
    //    and crosses the point (x2, y2). The points at which this circle touches these two lines are called the start
    //    and end tangent points respectively.
    let adjacent = radius / f64::from((cos_theta.acos() / 2.0).tan());
    let factor1 = adjacent / f64::from(length(v1));
    let x3 = f64::from(p1.x) + factor1 * f64::from(p0.x - p1.x);
    let y3 = f64::from(p1.y) + factor1 * f64::from(p0.y - p1.y);
    let start_tangent = point_from_doubles(x3, y3);

    let factor2 = adjacent / f64::from(length(v2));
    let x4 = f64::from(p1.x) + factor2 * f64::from(p2.x - p1.x);
    let y4 = f64::from(p1.y) + factor2 * f64::from(p2.y - p1.y);
    let end_tangent = point_from_doubles(x4, y4);

    // Connect the point (x0, y0) to the start tangent point by a straight line, adding the start tangent point to the
    // subpath.
    path.line_to(start_tangent);

    // NB: The arc is always small, since the tangent points are its end points and the lines meet at (x1, y1).
    let large_arc = false;
    let cross_product = v1.0 * v2.1 - v1.1 * v2.0;
    // NB: By the right-hand rule, true means clockwise.
    let sweep = cross_product < 0.0;

    // and then connect the start tangent point to the end tangent point by The Arc, adding the end tangent point to
    // the subpath.
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the radius of an arc of a Gfx::Path is a float"
    )]
    path.arc_to(end_tangent, radius as f32, large_arc, sweep);
    Ok(())
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-rect
pub(crate) fn rect(path: &mut impl PathSink, x: f64, y: f64, w: f64, h: f64) {
    // 1. If any of the arguments are infinite or NaN, then return.
    if [x, y, w, h].iter().any(|argument| !argument.is_finite()) {
        return;
    }

    // 2. Create a new subpath containing just the four points (x, y), (x+w, y), (x+w, y+h), (x, y+h), in that order,
    //    with those four points connected by straight lines.
    path.move_to(point_from_doubles(x, y));
    path.line_to(point_from_doubles(x + w, y));
    path.line_to(point_from_doubles(x + w, y + h));
    path.line_to(point_from_doubles(x, y + h));

    // 3. Mark the subpath as closed.
    path.close();

    // 4. Create a new subpath with the point (x, y) as the only point in the subpath.
    path.move_to(point_from_doubles(x, y));
}

/// A radius of roundRect(): both of a DOMPointInit, or a number twice. `CanvasRoundRectRadius` of
/// Libraries/LibWeb/HTML/Canvas/CanvasPath.h mirrors it.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub(crate) struct RoundRectRadius {
    pub(crate) x: f64,
    pub(crate) y: f64,
}

/// https://html.spec.whatwg.org/multipage/canvas.html#dom-context-2d-roundrect, with the radii as a list.
pub(crate) fn round_rect(
    path: &mut impl PathSink,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
    radii: &[RoundRectRadius],
) -> Result<(), CanvasPathError> {
    // 1. If any of x, y, w, or h are infinite or NaN, then return.
    if [x, y, w, h].iter().any(|argument| !argument.is_finite()) {
        return Ok(());
    }

    // 2. If radii is an unrestricted double or DOMPointInit, then set radii to « radii ».
    // NB: The caller passes a list.

    // 3. If radii is not a list of size one, two, three, or four, then throw a RangeError.
    if radii.is_empty() || radii.len() > 4 {
        return Err(CanvasPathError::RoundRectRadiusCount);
    }

    // 4. Let normalizedRadii be an empty list.
    let mut normalized_radii = Vec::with_capacity(radii.len());

    // 5. For each radius of radii:
    for radius in radii {
        // 5.1.1. If radius["x"] or radius["y"] is infinite or NaN, then return.
        if !radius.x.is_finite() || !radius.y.is_finite() {
            return Ok(());
        }

        // 5.1.2. If radius["x"] or radius["y"] is negative, then throw a RangeError.
        if radius.x < 0.0 || radius.y < 0.0 {
            return Err(CanvasPathError::NegativeRoundRectRadius);
        }

        // 5.1.3. Otherwise, append radius to normalizedRadii.
        normalized_radii.push(*radius);

        // 5.2. If radius is a unrestricted double:
        // NB: The caller passes a number as a radius with x and y.
    }

    // 6. Let upperLeft, upperRight, lowerRight, and lowerLeft be null.
    // 7. If normalizedRadii's size is 4, then set upperLeft to normalizedRadii[0], set upperRight to
    //    normalizedRadii[1], set lowerRight to normalizedRadii[2], and set lowerLeft to normalizedRadii[3].
    // 8. If normalizedRadii's size is 3, then set upperLeft to normalizedRadii[0], set upperRight and lowerLeft to
    //    normalizedRadii[1], and set lowerRight to normalizedRadii[2].
    // 9. If normalizedRadii's size is 2, then set upperLeft and lowerRight to normalizedRadii[0] and set upperRight
    //    and lowerLeft to normalizedRadii[1].
    // 10. If normalizedRadii's size is 1, then set upperLeft, upperRight, lowerRight, and lowerLeft to
    //     normalizedRadii[0].
    let (mut upper_left, mut upper_right, mut lower_right, mut lower_left) = match *normalized_radii.as_slice() {
        [upper_left, upper_right, lower_right, lower_left] => (upper_left, upper_right, lower_right, lower_left),
        [upper_left, upper_right_and_lower_left, lower_right] => (
            upper_left,
            upper_right_and_lower_left,
            lower_right,
            upper_right_and_lower_left,
        ),
        [upper_left_and_lower_right, upper_right_and_lower_left] => (
            upper_left_and_lower_right,
            upper_right_and_lower_left,
            upper_left_and_lower_right,
            upper_right_and_lower_left,
        ),
        [all] => (all, all, all, all),
        _ => unreachable!("there are one to four radii"),
    };

    // 11. Corner curves must not overlap. Scale all radii to prevent this:
    // 11.1. Let top be upperLeft["x"] + upperRight["x"].
    let top = upper_left.x + upper_right.x;

    // 11.2. Let right be upperRight["y"] + lowerRight["y"].
    let right = upper_right.y + lower_right.y;

    // 11.3. Let bottom be lowerRight["x"] + lowerLeft["x"].
    let bottom = lower_right.x + lower_left.x;

    // 11.4. Let left be upperLeft["y"] + lowerLeft["y"].
    let left = upper_left.y + lower_left.y;

    // 11.5. Let scale be the minimum value of the ratios w / top, h / right, w / bottom, h / left.
    // NB: As AK::min(), which the C++ took it with, this keeps the first of two ratios unless the second is less.
    let min = |a: f64, b: f64| if b < a { b } else { a };
    let scale = min(min(w / top, h / right), min(w / bottom, h / left));

    // 11.6. If scale is less than 1, then set the x and y members of upperLeft, upperRight, lowerLeft, and
    //       lowerRight to their current values multiplied by scale.
    if scale < 1.0 {
        for radius in [&mut upper_left, &mut upper_right, &mut lower_left, &mut lower_right] {
            radius.x *= scale;
            radius.y *= scale;
        }
    }

    // 12. Create a new subpath:
    let large_arc = false;
    let sweep = true;
    #[allow(
        clippy::cast_possible_truncation,
        reason = "the radii of an arc of a Gfx::Path are floats"
    )]
    let radii_of = |radius: RoundRectRadius| FloatSize {
        width: radius.x as f32,
        height: radius.y as f32,
    };

    // 12.1. Move to the point (x + upperLeft["x"], y).
    path.move_to(point_from_doubles(x + upper_left.x, y));

    // 12.2. Draw a straight line to the point (x + w − upperRight["x"], y).
    path.line_to(point_from_doubles(x + w - upper_right.x, y));

    // 12.3. Draw an arc to the point (x + w, y + upperRight["y"]).
    path.elliptical_arc_to(
        point_from_doubles(x + w, y + upper_right.y),
        radii_of(upper_right),
        0.0,
        large_arc,
        sweep,
    );

    // 12.4. Draw a straight line to the point (x + w, y + h − lowerRight["y"]).
    path.line_to(point_from_doubles(x + w, y + h - lower_right.y));

    // 12.5. Draw an arc to the point (x + w − lowerRight["x"], y + h).
    path.elliptical_arc_to(
        point_from_doubles(x + w - lower_right.x, y + h),
        radii_of(lower_right),
        0.0,
        large_arc,
        sweep,
    );

    // 12.6. Draw a straight line to the point (x + lowerLeft["x"], y + h).
    path.line_to(point_from_doubles(x + lower_left.x, y + h));

    // 12.7. Draw an arc to the point (x, y + h − lowerLeft["y"]).
    path.elliptical_arc_to(
        point_from_doubles(x, y + h - lower_left.y),
        radii_of(lower_left),
        0.0,
        large_arc,
        sweep,
    );

    // 12.8. Draw a straight line to the point (x, y + upperLeft["y"]).
    path.line_to(point_from_doubles(x, y + upper_left.y));

    // 12.9. Draw an arc to the point (x + upperLeft["x"], y).
    path.elliptical_arc_to(
        point_from_doubles(x + upper_left.x, y),
        radii_of(upper_left),
        0.0,
        large_arc,
        sweep,
    );

    // 13. Mark the subpath as closed.
    path.close();

    // 14. Create a new subpath with the point (x, y) as the only point in the subpath.
    path.move_to(point_from_doubles(x, y));
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::{
        CanvasPathError, PathSink, RoundRectRadius, arc, arc_to, close_path, ellipse, line_to, move_to, rect,
        round_rect,
    };
    use libgfx_rust::{FloatPoint, FloatSize};

    #[derive(Debug, PartialEq)]
    enum Op {
        MoveTo(f32, f32),
        LineTo(f32, f32),
        Quadratic,
        Cubic,
        EllipticalArcTo {
            x: f32,
            y: f32,
            radii: (f32, f32),
            large_arc: bool,
            sweep: bool,
        },
        ArcTo {
            x: f32,
            y: f32,
            radius: f32,
            sweep: bool,
        },
        Close,
    }

    /// The operations that a path was given, with the rules of a Gfx::Path for its current point.
    #[derive(Default)]
    struct RecordingPath {
        ops: Vec<Op>,
        last_point: Option<FloatPoint>,
        subpath_start: FloatPoint,
    }

    impl PathSink for RecordingPath {
        fn is_empty(&self) -> bool {
            self.last_point.is_none()
        }

        fn last_point(&self) -> FloatPoint {
            self.last_point.unwrap_or_default()
        }

        fn move_to(&mut self, point: FloatPoint) {
            self.ops.push(Op::MoveTo(point.x, point.y));
            self.last_point = Some(point);
            self.subpath_start = point;
        }

        fn line_to(&mut self, point: FloatPoint) {
            if self.is_empty() {
                return self.move_to(point);
            }
            self.ops.push(Op::LineTo(point.x, point.y));
            self.last_point = Some(point);
        }

        fn quadratic_bezier_curve_to(&mut self, _: FloatPoint, point: FloatPoint) {
            self.ops.push(Op::Quadratic);
            self.last_point = Some(point);
        }

        fn cubic_bezier_curve_to(&mut self, _: FloatPoint, _: FloatPoint, point: FloatPoint) {
            self.ops.push(Op::Cubic);
            self.last_point = Some(point);
        }

        fn elliptical_arc_to(&mut self, point: FloatPoint, radii: FloatSize, _: f32, large_arc: bool, sweep: bool) {
            self.ops.push(Op::EllipticalArcTo {
                x: point.x,
                y: point.y,
                radii: (radii.width, radii.height),
                large_arc,
                sweep,
            });
            self.last_point = Some(point);
        }

        fn arc_to(&mut self, point: FloatPoint, radius: f32, _: bool, sweep: bool) {
            self.ops.push(Op::ArcTo {
                x: point.x,
                y: point.y,
                radius,
                sweep,
            });
            self.last_point = Some(point);
        }

        fn close(&mut self) {
            if self.is_empty() {
                return;
            }
            self.ops.push(Op::Close);
            self.last_point = Some(self.subpath_start);
        }
    }

    #[test]
    fn points_that_are_not_finite_are_ignored() {
        let mut path = RecordingPath::default();
        move_to(&mut path, f32::NAN, 0.0);
        line_to(&mut path, 0.0, f32::INFINITY);
        close_path(&mut path);
        rect(&mut path, 0.0, 0.0, f64::NAN, 1.0);
        assert!(path.ops.is_empty());
    }

    #[test]
    fn a_line_without_a_subpath_starts_one() {
        let mut path = RecordingPath::default();
        line_to(&mut path, 1.0, 2.0);
        line_to(&mut path, 3.0, 4.0);
        assert_eq!(path.ops, [Op::MoveTo(1.0, 2.0), Op::LineTo(3.0, 4.0)]);
    }

    #[test]
    fn a_rect_is_a_closed_subpath_followed_by_a_new_one() {
        let mut path = RecordingPath::default();
        rect(&mut path, 1.0, 2.0, 3.0, 4.0);
        assert_eq!(
            path.ops,
            [
                Op::MoveTo(1.0, 2.0),
                Op::LineTo(4.0, 2.0),
                Op::LineTo(4.0, 6.0),
                Op::LineTo(1.0, 6.0),
                Op::Close,
                Op::MoveTo(1.0, 2.0),
            ]
        );
    }

    #[test]
    fn negative_radii_throw() {
        let mut path = RecordingPath::default();
        assert_eq!(
            arc(&mut path, 0.0, 0.0, -1.0, 0.0, 1.0, false),
            Err(CanvasPathError::NegativeArcRadius(-1.0))
        );
        assert_eq!(arc(&mut path, 0.0, 0.0, f32::NEG_INFINITY, 0.0, 1.0, false), Ok(()));
        assert_eq!(
            ellipse(&mut path, 0.0, 0.0, 1.0, -2.0, 0.0, 0.0, 1.0, false),
            Err(CanvasPathError::NegativeMinorAxisRadius(-2.0))
        );
        assert_eq!(
            arc_to(&mut path, 1.0, 1.0, 2.0, 2.0, -0.5),
            Err(CanvasPathError::NegativeArcToRadius(-0.5))
        );
        // NB: arcTo() ensures there is a subpath before it checks the radius.
        assert_eq!(path.ops, [Op::MoveTo(1.0, 1.0)]);
        assert_eq!(
            CanvasPathError::NegativeArcToRadius(-0.5).message(),
            "The radius provided (-0.5) is negative."
        );
    }

    #[test]
    fn an_ellipse_with_a_zero_radius_is_a_line_to_its_center() {
        let mut path = RecordingPath::default();
        move_to(&mut path, 1.0, 1.0);
        assert_eq!(ellipse(&mut path, 5.0, 6.0, 0.0, 2.0, 0.0, 0.0, 1.0, false), Ok(()));
        assert_eq!(path.ops, [Op::MoveTo(1.0, 1.0), Op::LineTo(5.0, 6.0)]);
    }

    #[test]
    fn a_whole_turn_is_the_whole_ellipse() {
        let mut path = RecordingPath::default();
        assert_eq!(
            arc(&mut path, 0.0, 0.0, 10.0, 0.0, core::f32::consts::TAU, false),
            Ok(())
        );
        let [
            Op::MoveTo(start_x, start_y),
            Op::EllipticalArcTo {
                large_arc,
                sweep,
                radii,
                ..
            },
        ] = path.ops.as_slice()
        else {
            panic!("the arc is one elliptical arc from its start: {:?}", path.ops);
        };
        assert_eq!((*start_x, *start_y), (10.0, 0.0));
        assert_eq!(*radii, (10.0, 10.0));
        assert!(*large_arc && *sweep);
    }

    #[test]
    fn arc_to_with_collinear_points_is_a_line() {
        let mut path = RecordingPath::default();
        move_to(&mut path, 0.0, 0.0);
        assert_eq!(arc_to(&mut path, 1.0, 0.0, 2.0, 0.0, 1.0), Ok(()));
        assert_eq!(arc_to(&mut path, 3.0, 3.0, 4.0, 3.0, 0.0), Ok(()));
        assert_eq!(
            path.ops,
            [Op::MoveTo(0.0, 0.0), Op::LineTo(1.0, 0.0), Op::LineTo(3.0, 3.0)]
        );
    }

    #[test]
    fn arc_to_a_corner_is_a_line_and_an_arc_between_the_tangent_points() {
        let mut path = RecordingPath::default();
        move_to(&mut path, 0.0, 0.0);
        assert_eq!(arc_to(&mut path, 10.0, 0.0, 10.0, 10.0, 5.0), Ok(()));
        let [_, Op::LineTo(x3, y3), Op::ArcTo { x, y, radius, sweep }] = path.ops.as_slice() else {
            panic!("the corner is a line and an arc: {:?}", path.ops);
        };
        assert!((x3 - 5.0).abs() < 1e-5 && *y3 == 0.0);
        assert!((*x - 10.0).abs() < 1e-5 && (y - 5.0).abs() < 1e-5);
        assert!(*sweep);
        assert!((radius - 5.0).abs() < 1e-5);
    }

    #[test]
    fn round_rect_takes_one_to_four_radii() {
        let radius = RoundRectRadius { x: 1.0, y: 1.0 };
        let mut path = RecordingPath::default();
        assert_eq!(
            round_rect(&mut path, 0.0, 0.0, 10.0, 10.0, &[]),
            Err(CanvasPathError::RoundRectRadiusCount)
        );
        assert_eq!(
            round_rect(&mut path, 0.0, 0.0, 10.0, 10.0, &[radius; 5]),
            Err(CanvasPathError::RoundRectRadiusCount)
        );
        assert_eq!(
            round_rect(
                &mut path,
                0.0,
                0.0,
                10.0,
                10.0,
                &[radius, RoundRectRadius { x: -1.0, y: 0.0 }]
            ),
            Err(CanvasPathError::NegativeRoundRectRadius)
        );
        assert_eq!(
            round_rect(
                &mut path,
                0.0,
                0.0,
                10.0,
                10.0,
                &[radius, RoundRectRadius { x: f64::NAN, y: -1.0 }]
            ),
            Ok(())
        );
        assert!(path.ops.is_empty());
    }

    #[test]
    fn round_rect_assigns_three_radii_to_the_corners_and_scales_them_down() {
        let mut path = RecordingPath::default();
        let radii = [
            RoundRectRadius { x: 1.0, y: 1.0 },
            RoundRectRadius { x: 2.0, y: 2.0 },
            RoundRectRadius { x: 30.0, y: 30.0 },
        ];
        assert_eq!(round_rect(&mut path, 0.0, 0.0, 16.0, 16.0, &radii), Ok(()));
        // NB: The upper right and the lower right corners are 32 tall together, so every radius is halved.
        let arc_radii: Vec<_> = path
            .ops
            .iter()
            .filter_map(|op| match op {
                Op::EllipticalArcTo { radii, .. } => Some(*radii),
                _ => None,
            })
            .collect();
        assert_eq!(arc_radii, [(1.0, 1.0), (15.0, 15.0), (1.0, 1.0), (0.5, 0.5)]);
        assert_eq!(path.ops.first(), Some(&Op::MoveTo(0.5, 0.0)));
        assert_eq!(path.ops.last(), Some(&Op::MoveTo(0.0, 0.0)));
    }
}
