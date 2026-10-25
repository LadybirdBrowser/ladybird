/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The functions through which `Gfx::Path` in C++ owns, builds and reads a path.
//!
//! Only LibGfx's copy of this crate makes and frees the paths of `Gfx::Path`. Rust code builds and
//! queries its own paths with `Path`.

use super::encoding;
use super::geometry::{PathGeometry, PathVerb};
use crate::geometry::FloatPoint;

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

impl PathGeometryData {
    pub fn of(geometry: &PathGeometry) -> Self {
        let (verbs, points, conic_weights) = if geometry.has_geometry() {
            (geometry.verbs(), geometry.points(), geometry.conic_weights())
        } else {
            (&[][..], &[][..], &[][..])
        };
        Self {
            verbs: verbs.as_ptr().cast::<u8>(),
            verb_count: verbs.len(),
            points: points.as_ptr(),
            point_count: points.len(),
            conic_weights: conic_weights.as_ptr(),
            conic_weight_count: conic_weights.len(),
            fill_rule: geometry.fill_rule() as u8,
        }
    }

    /// Adds the contours to the path, with their points mapped.
    ///
    /// # Safety
    /// The data must be that of a live path, as of() gives it.
    pub(super) unsafe fn append_mapped_to(&self, geometry: &mut PathGeometry, map: impl Fn(FloatPoint) -> FloatPoint) {
        // SAFETY: The caller keeps the arrays of the path live, and each of its verbs is a PathVerb.
        let (verbs, points, conic_weights) = unsafe {
            (
                ak::slice_from_raw_parts(self.verbs.cast::<PathVerb>(), self.verb_count),
                ak::slice_from_raw_parts(self.points, self.point_count),
                ak::slice_from_raw_parts(self.conic_weights, self.conic_weight_count),
            )
        };
        geometry.append_mapped_contours(verbs, points, conic_weights, map);
    }
}

/// # Safety
/// `path` must be a live path from this module, and `out_data` must point to room for the data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn path_geometry_data(path: *const PathGeometry, out_data: *mut PathGeometryData) {
    // SAFETY: Forwarded from the caller.
    let data = PathGeometryData::of(unsafe { &*path });
    // SAFETY: The caller passes room for the data.
    unsafe { std::ptr::write(out_data, data) };
}
