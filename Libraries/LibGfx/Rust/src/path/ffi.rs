/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions through which `Gfx::Path` in C++ owns, builds and queries a path.
//!
//! Only LibGfx's copy of this crate makes and frees the paths of `Gfx::Path`. Rust code in other
//! libraries reaches them through the `ladybird_gfx_path_*` functions of LibGfx.

use super::encoding;
use super::geometry::PathGeometry;
use super::hit_test;
use super::measure::{self, ContourMeasure, PathVertex};
use super::stroke::{self, StrokeStyle};
use super::svg_text;
use crate::geometry::FloatPoint;
use crate::matrix::AffineTransform;
use crate::paint_enums::{CapStyle, JoinStyle, WindingRule};
use std::ffi::c_void;

/// The contours of a path, valid until the path changes or is destroyed. A path without geometry has
/// none.
#[repr(C)]
pub struct PathGeometryData {
    pub verbs: *const u8,
    pub verb_count: usize,
    pub points: *const FloatPoint,
    pub point_count: usize,
    pub conic_weights: *const f32,
    pub conic_weight_count: usize,
    pub fill_rule: u8,
}

fn point(x: f32, y: f32) -> FloatPoint {
    FloatPoint { x, y }
}

/// # Safety
/// `transform` must point to the six values a, b, c, d, e and f of an affine transform.
unsafe fn read_transform(transform: *const f32) -> AffineTransform {
    AffineTransform {
        // SAFETY: The caller passes six floats.
        values: unsafe { std::ptr::read(transform.cast::<[f32; 6]>()) },
    }
}

/// # Safety
/// `out_point_and_tangent` must point to four floats.
unsafe fn write_point_and_tangent(out_point_and_tangent: *mut f32, (position, tangent): (FloatPoint, FloatPoint)) {
    // SAFETY: The caller passes room for four floats.
    unsafe {
        std::ptr::write(
            out_point_and_tangent.cast::<[f32; 4]>(),
            [position.x, position.y, tangent.x, tangent.y],
        );
    }
}

/// Calls the function with the path and the other path. A path appended to itself is copied first.
///
/// # Safety
/// `path` and `other` must be live paths from this module; nothing else may use `path` for the call.
unsafe fn with_other_path(
    path: *mut PathGeometry,
    other: *const PathGeometry,
    function: impl FnOnce(&mut PathGeometry, &PathGeometry),
) {
    if std::ptr::eq(path, other) {
        // SAFETY: Forwarded from the caller.
        let copy = unsafe { &*other }.clone();
        // SAFETY: Forwarded from the caller.
        function(unsafe { &mut *path }, &copy);
    } else {
        // SAFETY: Forwarded from the caller; the paths are distinct.
        function(unsafe { &mut *path }, unsafe { &*other });
    }
}

fn into_raw(geometry: PathGeometry) -> *mut PathGeometry {
    Box::into_raw(Box::new(geometry))
}

#[unsafe(no_mangle)]
pub extern "C" fn path_geometry_create() -> *mut PathGeometry {
    into_raw(PathGeometry::new())
}

/// # Safety
/// `path` must be a live path from this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_clone(path: *const PathGeometry) -> *mut PathGeometry {
    // SAFETY: Forwarded from the caller.
    into_raw(unsafe { &*path }.clone())
}

/// # Safety
/// `path` must be a path from this module that nothing uses afterwards, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_destroy(path: *mut PathGeometry) {
    if !path.is_null() {
        // SAFETY: The caller hands over the path, which into_raw() made from a Box.
        drop(unsafe { Box::from_raw(path) });
    }
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_clear(path: *mut PathGeometry) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.clear();
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_move_to(path: *mut PathGeometry, x: f32, y: f32) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.move_to(point(x, y));
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_line_to(path: *mut PathGeometry, x: f32, y: f32) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.line_to(point(x, y));
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_quadratic_bezier_curve_to(
    path: *mut PathGeometry,
    control_x: f32,
    control_y: f32,
    x: f32,
    y: f32,
) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.quadratic_bezier_curve_to(point(control_x, control_y), point(x, y));
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_cubic_bezier_curve_to(
    path: *mut PathGeometry,
    control1_x: f32,
    control1_y: f32,
    control2_x: f32,
    control2_y: f32,
    x: f32,
    y: f32,
) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.cubic_bezier_curve_to(
        point(control1_x, control1_y),
        point(control2_x, control2_y),
        point(x, y),
    );
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_elliptical_arc_to(
    path: *mut PathGeometry,
    x: f32,
    y: f32,
    radius_x: f32,
    radius_y: f32,
    x_axis_rotation: f32,
    large_arc: bool,
    sweep: bool,
) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.elliptical_arc_to(point(x, y), (radius_x, radius_y), x_axis_rotation, large_arc, sweep);
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_arc_to(
    path: *mut PathGeometry,
    x: f32,
    y: f32,
    radius: f32,
    large_arc: bool,
    sweep: bool,
) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.arc_to(point(x, y), radius, large_arc, sweep);
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_close(path: *mut PathGeometry) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.close();
}

// Contour building for outlines, without the rules of Gfx::Path for a path that has no current point.

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_contour_move_to(path: *mut PathGeometry, x: f32, y: f32) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.contour_move_to(point(x, y));
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_contour_line_to(path: *mut PathGeometry, x: f32, y: f32) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.contour_line_to(point(x, y));
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_contour_quad_to(
    path: *mut PathGeometry,
    control_x: f32,
    control_y: f32,
    x: f32,
    y: f32,
) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.contour_quad_to(point(control_x, control_y), point(x, y));
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_contour_cubic_to(
    path: *mut PathGeometry,
    control1_x: f32,
    control1_y: f32,
    control2_x: f32,
    control2_y: f32,
    x: f32,
    y: f32,
) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.contour_cubic_to(
        point(control1_x, control1_y),
        point(control2_x, control2_y),
        point(x, y),
    );
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_contour_close(path: *mut PathGeometry) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.contour_close();
}

/// # Safety
/// `path` and `other` must be live paths from this module; nothing else may use `path` for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_append_path(path: *mut PathGeometry, other: *const PathGeometry) {
    // SAFETY: Forwarded from the caller.
    unsafe { with_other_path(path, other, |path, other| path.append_path(other)) };
}

/// Adds the contours of the other path, mapped by the affine transform, without changing what the
/// operations of Gfx::Path remember.
///
/// # Safety
/// `path` and `other` must be live paths from this module; nothing else may use `path` for the call.
/// `transform` must point to the six values a, b, c, d, e and f of an affine transform.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_append_transformed_contours(
    path: *mut PathGeometry,
    other: *const PathGeometry,
    transform: *const f32,
) {
    // SAFETY: Forwarded from the caller.
    let transform = unsafe { read_transform(transform) };
    // SAFETY: Forwarded from the caller.
    unsafe {
        with_other_path(path, other, |path, other| {
            path.append_mapped(other, |point| transform.map_point(point))
        })
    };
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_update_state_from_contours(path: *mut PathGeometry) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.update_state_from_contours();
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_set_fill_rule(path: *mut PathGeometry, winding_rule: i32) {
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.set_fill_rule(WindingRule::from_raw(winding_rule));
}

/// # Safety
/// `path` must be a live path from this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_is_empty(path: *const PathGeometry) -> bool {
    // SAFETY: Forwarded from the caller.
    unsafe { &*path }.is_empty()
}

/// # Safety
/// `path` must be a live path from this module, and `out_x_y` must point to two floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_last_point(path: *const PathGeometry, out_x_y: *mut f32) {
    // SAFETY: Forwarded from the caller.
    let last = unsafe { &*path }.last_point();
    // SAFETY: The caller passes room for two floats.
    unsafe { std::ptr::write(out_x_y.cast::<[f32; 2]>(), [last.x, last.y]) };
}

/// # Safety
/// `path` must be a live path from this module, and `out_x_y_width_height` must point to four floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_bounding_box(path: *const PathGeometry, out_x_y_width_height: *mut f32) {
    // SAFETY: Forwarded from the caller.
    let bounds = unsafe { &*path }.bounding_box();
    // SAFETY: The caller passes room for four floats.
    unsafe { std::ptr::write(out_x_y_width_height.cast::<[f32; 4]>(), bounds) };
}

/// # Safety
/// `path` must be a live path from this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_length(path: *const PathGeometry) -> f32 {
    // SAFETY: Forwarded from the caller.
    measure::length(unsafe { &*path })
}

/// Writes the point and the unit tangent at the distance along the path, and returns whether the path
/// has a point.
///
/// # Safety
/// `path` must be a live path from this module, and `out_point_and_tangent` must point to four floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_point_at_length(
    path: *const PathGeometry,
    distance: f32,
    out_point_and_tangent: *mut f32,
) -> bool {
    // SAFETY: Forwarded from the caller.
    let Some(point_and_tangent) = measure::point_at_length(unsafe { &*path }, distance) else {
        return false;
    };
    // SAFETY: Forwarded from the caller.
    unsafe { write_point_and_tangent(out_point_and_tangent, point_and_tangent) };
    true
}

/// Calls the callback once with the vertices of the polylines that follow the path.
///
/// # Safety
/// `path` must be a live path from this module, and the callback must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_vertices(
    path: *const PathGeometry,
    callback: unsafe extern "C" fn(*mut c_void, *const PathVertex, usize),
    context: *mut c_void,
) {
    // SAFETY: Forwarded from the caller.
    let vertices = measure::vertices(unsafe { &*path });
    // SAFETY: The vertices live for the synchronous call.
    unsafe { callback(context, vertices.as_ptr(), vertices.len()) };
}

/// # Safety
/// `path` must be a live path from this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_contains(path: *const PathGeometry, x: f32, y: f32, winding_rule: i32) -> bool {
    // SAFETY: Forwarded from the caller.
    hit_test::contains(unsafe { &*path }, x, y, WindingRule::from_raw(winding_rule))
}

/// The outline of the area that stroking the path covers, as a path to fill with the nonzero winding rule.
/// The cap and join styles have the values of `Gfx::Path::CapStyle` and `Gfx::Path::JoinStyle`.
///
/// # Safety
/// `path` must be a live path from this module, and `dash_array` must point to `dash_count` floats, or be
/// null with a count of zero.
#[allow(clippy::too_many_arguments)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_stroke_to_fill(
    path: *const PathGeometry,
    thickness: f32,
    cap_style: i32,
    join_style: i32,
    miter_limit: f32,
    dash_array: *const f32,
    dash_count: usize,
    dash_offset: f32,
) -> *mut PathGeometry {
    let style = StrokeStyle {
        thickness,
        cap_style: match cap_style {
            1 => CapStyle::Round,
            2 => CapStyle::Square,
            _ => CapStyle::Butt,
        },
        join_style: match join_style {
            1 => JoinStyle::Round,
            2 => JoinStyle::Bevel,
            _ => JoinStyle::Miter,
        },
        miter_limit,
        // SAFETY: Forwarded from the caller.
        dash_array: unsafe { ak::slice_from_raw_parts(dash_array, dash_count) },
        dash_offset,
    };
    // SAFETY: Forwarded from the caller.
    into_raw(stroke::stroke_to_fill(unsafe { &*path }, &style))
}

/// # Safety
/// `path` must be a live path from this module, and `transform` must point to the six values a, b, c, d,
/// e and f of an affine transform.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_copy_transformed(
    path: *const PathGeometry,
    transform: *const f32,
) -> *mut PathGeometry {
    // SAFETY: Forwarded from the caller.
    let transform = unsafe { read_transform(transform) };
    // SAFETY: Forwarded from the caller.
    into_raw(unsafe { &*path }.copy_transformed(transform))
}

/// # Safety
/// `path` must be a live path from this module, which nothing else uses for the call, and `transform`
/// must point to the six values a, b, c, d, e and f of an affine transform.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_transform(path: *mut PathGeometry, transform: *const f32) {
    // SAFETY: Forwarded from the caller.
    let transform = unsafe { read_transform(transform) };
    // SAFETY: Forwarded from the caller.
    unsafe { &mut *path }.transform(transform);
}

/// Calls the callback once with the byte form of the path.
///
/// # Safety
/// `path` must be a live path from this module, and the callback must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_encode(
    path: *const PathGeometry,
    callback: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    context: *mut c_void,
) {
    // SAFETY: Forwarded from the caller.
    let bytes = encoding::encode(unsafe { &*path });
    // SAFETY: The bytes live for the synchronous call.
    unsafe { callback(context, bytes.as_ptr(), bytes.len()) };
}

/// The path that the bytes describe. Bytes that describe no path give an empty path.
///
/// # Safety
/// `bytes` must point to `count` bytes, or be null with a count of zero.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_decode(bytes: *const u8, count: usize) -> *mut PathGeometry {
    // SAFETY: Forwarded from the caller.
    let bytes = unsafe { ak::slice_from_raw_parts(bytes, count) };
    into_raw(encoding::decode(bytes).unwrap_or_default())
}

/// Calls the callback once with the SVG path data of the path, as UTF-8.
///
/// # Safety
/// `path` must be a live path from this module, and the callback must accept `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_svg_string(
    path: *const PathGeometry,
    callback: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    context: *mut c_void,
) {
    // SAFETY: Forwarded from the caller.
    let string = svg_text::to_svg_string(unsafe { &*path });
    // SAFETY: The string lives for the synchronous call.
    unsafe { callback(context, string.as_ptr(), string.len()) };
}

/// # Safety
/// `a` and `b` must be live paths from this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_equals(a: *const PathGeometry, b: *const PathGeometry) -> bool {
    // SAFETY: Forwarded from the caller.
    unsafe { &*a }.has_same_contours(unsafe { &*b })
}

/// # Safety
/// `path` must be a live path from this module, and `out_data` must point to room for the data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_data(path: *const PathGeometry, out_data: *mut PathGeometryData) {
    // SAFETY: Forwarded from the caller.
    let geometry = unsafe { &*path };
    let (verbs, points, conic_weights) = if geometry.has_geometry() {
        (geometry.verbs(), geometry.points(), geometry.conic_weights())
    } else {
        (&[][..], &[][..], &[][..])
    };
    let data = PathGeometryData {
        verbs: verbs.as_ptr().cast::<u8>(),
        verb_count: verbs.len(),
        points: points.as_ptr(),
        point_count: points.len(),
        conic_weights: conic_weights.as_ptr(),
        conic_weight_count: conic_weights.len(),
        fill_rule: geometry.fill_rule() as u8,
    };
    // SAFETY: The caller passes room for the data.
    unsafe { std::ptr::write(out_data, data) };
}

/// The measure of the first contour of the path that has length, or null if none has.
///
/// # Safety
/// `path` must be a live path from this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_measure_first_contour(path: *const PathGeometry) -> *mut ContourMeasure {
    // SAFETY: Forwarded from the caller.
    ContourMeasure::contours_of(unsafe { &*path })
        .next()
        .map_or(std::ptr::null_mut(), |contour| Box::into_raw(Box::new(contour)))
}

/// # Safety
/// `measure` must be a live measure from path_geometry_measure_first_contour().
#[unsafe(no_mangle)]
pub unsafe extern "C" fn contour_measure_length(measure: *const ContourMeasure) -> f32 {
    // SAFETY: Forwarded from the caller.
    unsafe { &*measure }.length()
}

/// Writes the point at the distance along the contour, clamped to its ends, and the unit tangent there.
///
/// # Safety
/// `measure` must be a live measure from path_geometry_measure_first_contour(), and
/// `out_point_and_tangent` must point to four floats.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn contour_measure_position_and_tangent(
    measure: *const ContourMeasure,
    distance: f32,
    out_point_and_tangent: *mut f32,
) {
    // SAFETY: Forwarded from the caller.
    let point_and_tangent = unsafe { &*measure }.position_and_tangent(distance);
    // SAFETY: Forwarded from the caller.
    unsafe { write_point_and_tangent(out_point_and_tangent, point_and_tangent) };
}

/// # Safety
/// `measure` must be a measure from path_geometry_measure_first_contour() that nothing uses afterwards,
/// or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn contour_measure_destroy(measure: *mut ContourMeasure) {
    if !measure.is_null() {
        // SAFETY: The caller hands over the measure, which was made from a Box.
        drop(unsafe { Box::from_raw(measure) });
    }
}
