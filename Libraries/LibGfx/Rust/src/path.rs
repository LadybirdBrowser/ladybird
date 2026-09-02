/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::mem::ManuallyDrop;
use std::ptr::NonNull;

unsafe extern "C" {
    fn ladybird_gfx_path_destroy(path: *mut c_void);
    fn ladybird_gfx_path_equals(a: *const c_void, b: *const c_void) -> bool;
    fn ladybird_gfx_path_append_svg_string(
        path: *const c_void,
        append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
        context: *mut c_void,
    );
    fn ladybird_gfx_path_bounding_box(path: *const c_void, out_x_y_width_height: *mut f32);
    fn ladybird_gfx_path_serialize(
        path: *const c_void,
        append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
        context: *mut c_void,
    );
    fn ladybird_gfx_path_contains(path: *const c_void, x: f32, y: f32, winding_rule: i32) -> bool;
    fn ladybird_gfx_path_length(path: *const c_void) -> f32;
    fn ladybird_gfx_path_create_from_ops(kinds: *const u8, values: *const f32, count: usize) -> *mut c_void;
    fn ladybird_gfx_path_create_from_serialized_bytes(bytes: *const u8, count: usize) -> *mut c_void;
    fn ladybird_gfx_path_copy_transformed(path: *const c_void, affine_values: *const f32) -> *mut c_void;
}

#[derive(Clone, Copy, Debug)]
#[repr(u8)]
enum PathBuilderOpKind {
    MoveTo,
    LineTo,
    QuadraticBezierCurveTo,
    CubicBezierCurveTo,
    EllipticalArcTo,
    ArcTo,
    Close,
}

#[derive(Clone, Debug, Default)]
pub struct PathBuilder {
    kinds: Vec<u8>,
    values: Vec<f32>,
}

impl PathBuilder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn is_empty(&self) -> bool {
        self.kinds.is_empty()
    }

    fn push(&mut self, kind: PathBuilderOpKind, values: [f32; 7]) {
        self.kinds.push(kind as u8);
        self.values.extend_from_slice(&values);
    }

    pub fn move_to(&mut self, x: f32, y: f32) {
        self.push(PathBuilderOpKind::MoveTo, [x, y, 0.0, 0.0, 0.0, 0.0, 0.0]);
    }

    pub fn line_to(&mut self, x: f32, y: f32) {
        self.push(PathBuilderOpKind::LineTo, [x, y, 0.0, 0.0, 0.0, 0.0, 0.0]);
    }

    pub fn quadratic_bezier_curve_to(&mut self, through_x: f32, through_y: f32, x: f32, y: f32) {
        self.push(
            PathBuilderOpKind::QuadraticBezierCurveTo,
            [through_x, through_y, x, y, 0.0, 0.0, 0.0],
        );
    }

    pub fn cubic_bezier_curve_to(&mut self, c1x: f32, c1y: f32, c2x: f32, c2y: f32, x: f32, y: f32) {
        self.push(PathBuilderOpKind::CubicBezierCurveTo, [c1x, c1y, c2x, c2y, x, y, 0.0]);
    }

    #[allow(clippy::too_many_arguments)]
    pub fn elliptical_arc_to(
        &mut self,
        x: f32,
        y: f32,
        radius_x: f32,
        radius_y: f32,
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
    ) {
        self.push(
            PathBuilderOpKind::EllipticalArcTo,
            [
                x,
                y,
                radius_x,
                radius_y,
                x_axis_rotation,
                if large_arc { 1.0 } else { 0.0 },
                if sweep { 1.0 } else { 0.0 },
            ],
        );
    }

    pub fn arc_to(&mut self, x: f32, y: f32, radius: f32, large_arc: bool, sweep: bool) {
        self.push(
            PathBuilderOpKind::ArcTo,
            [
                x,
                y,
                radius,
                if large_arc { 1.0 } else { 0.0 },
                if sweep { 1.0 } else { 0.0 },
                0.0,
                0.0,
            ],
        );
    }

    pub fn close(&mut self) {
        self.push(PathBuilderOpKind::Close, [0.0; 7]);
    }

    pub fn build(&self) -> OwnedPath {
        // SAFETY: The op arrays are live for the call, and the returned pointer is a fresh
        // heap-allocated Gfx::Path that only the OwnedPath owns.
        unsafe {
            OwnedPath::adopt(ladybird_gfx_path_create_from_ops(
                self.kinds.as_ptr(),
                self.values.as_ptr(),
                self.kinds.len(),
            ))
        }
    }
}

unsafe extern "C" fn append_to_vec(context: *mut c_void, bytes: *const u8, length: usize) {
    if length == 0 {
        return;
    }
    // SAFETY: `context` is the Vec pointer handed out by serialize_to_bytes; the bytes are live
    // for this synchronous call.
    let out = unsafe { &mut *context.cast::<Vec<u8>>() };
    out.extend_from_slice(unsafe { std::slice::from_raw_parts(bytes, length) });
}

unsafe extern "C" fn append_to_string(context: *mut c_void, bytes: *const u8, length: usize) {
    // SAFETY: `context` is the String pointer passed by to_svg_string below, and the bytes are
    // live UTF-8 for this synchronous call.
    unsafe {
        let output = &mut *context.cast::<String>();
        if length > 0 {
            output.push_str(&String::from_utf8_lossy(std::slice::from_raw_parts(bytes, length)));
        }
    }
}

/// The sole owner of a heap-allocated `Gfx::Path`, destroying it on drop.
pub struct OwnedPath {
    raw: NonNull<c_void>,
    identity: u64,
}

impl std::fmt::Debug for OwnedPath {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("OwnedPath")
            .field("identity", &self.identity)
            .finish()
    }
}

impl OwnedPath {
    /// Assumes sole ownership of a heap-allocated `Gfx::Path`.
    ///
    /// # Safety
    ///
    /// `raw` must be the only pointer through which the heap-allocated
    /// `Gfx::Path` is owned or destroyed.
    #[inline]
    pub unsafe fn adopt(raw: *mut c_void) -> Self {
        static NEXT_IDENTITY: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(1);
        Self {
            raw: NonNull::new(raw).expect("Gfx::Path pointer must not be null"),
            identity: NEXT_IDENTITY.fetch_add(1, std::sync::atomic::Ordering::Relaxed),
        }
    }

    /// The raw `Gfx::Path` pointer, still owned by this value.
    #[inline]
    pub fn as_raw(&self) -> *mut c_void {
        self.raw.as_ptr()
    }

    /// Transfers ownership of the heap-allocated `Gfx::Path` to the caller.
    #[inline]
    pub fn into_raw(self) -> *mut c_void {
        let this = ManuallyDrop::new(self);
        this.raw.as_ptr()
    }

    /// A process-unique, never-reused identity for this path allocation, so
    /// consumers holding a copied snapshot can recognize an unchanged path
    /// without comparing contents.
    #[inline]
    pub fn identity(&self) -> u64 {
        self.identity
    }

    pub fn bounding_box(&self) -> [f32; 4] {
        let mut out = [0.0f32; 4];
        // SAFETY: The path is live for the duration of the call; the out-array holds four floats.
        unsafe { ladybird_gfx_path_bounding_box(self.raw.as_ptr(), out.as_mut_ptr()) };
        out
    }

    pub fn length(&self) -> f32 {
        // SAFETY: The path is live for the duration of the call.
        unsafe { ladybird_gfx_path_length(self.raw.as_ptr()) }
    }

    pub fn copy_transformed(&self, affine_values: [f32; 6]) -> OwnedPath {
        // SAFETY: The path is live for the call; the result is a fresh heap-allocated Gfx::Path.
        unsafe {
            OwnedPath::adopt(ladybird_gfx_path_copy_transformed(
                self.raw.as_ptr(),
                affine_values.as_ptr(),
            ))
        }
    }

    pub fn from_serialized_bytes(bytes: &[u8]) -> OwnedPath {
        // SAFETY: The bytes are live for the call, and the result is a fresh heap-allocated Gfx::Path;
        // malformed bytes yield an empty path rather than a failure.
        let raw = unsafe { ladybird_gfx_path_create_from_serialized_bytes(bytes.as_ptr(), bytes.len()) };
        // SAFETY: The host handed over sole ownership of the fresh path.
        unsafe { OwnedPath::adopt(raw) }
    }

    pub fn serialize_to_bytes(&self) -> Vec<u8> {
        let mut bytes: Vec<u8> = Vec::new();
        // SAFETY: The path is live for the duration of the call, and the callback only writes
        // into the Vec whose pointer it receives.
        unsafe { ladybird_gfx_path_serialize(self.raw.as_ptr(), append_to_vec, (&raw mut bytes).cast()) };
        bytes
    }

    pub fn contains(&self, x: f32, y: f32, winding_rule: i32) -> bool {
        // SAFETY: The path is live for the duration of the call.
        unsafe { ladybird_gfx_path_contains(self.raw.as_ptr(), x, y, winding_rule) }
    }

    pub fn to_svg_string(&self) -> String {
        let mut output = String::new();
        // SAFETY: The path is live for the duration of the call, and the callback only writes
        // into the String whose pointer it receives.
        unsafe { ladybird_gfx_path_append_svg_string(self.raw.as_ptr(), append_to_string, (&raw mut output).cast()) };
        output
    }
}

impl PartialEq for OwnedPath {
    fn eq(&self, other: &Self) -> bool {
        self.identity == other.identity
            // SAFETY: Both sides own live heap-allocated paths for the duration of the call.
            || unsafe { ladybird_gfx_path_equals(self.raw.as_ptr(), other.raw.as_ptr()) }
    }
}

impl Drop for OwnedPath {
    fn drop(&mut self) {
        // SAFETY: adopt() took sole ownership of the heap-allocated path.
        unsafe { ladybird_gfx_path_destroy(self.raw.as_ptr()) };
    }
}
