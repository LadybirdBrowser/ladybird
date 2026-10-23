/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;
use std::marker::PhantomData;
use std::mem::ManuallyDrop;
use std::ptr::NonNull;

use crate::geometry::{FloatPoint, FloatSize};

mod arcs;
mod curves;
mod encoding;
pub mod ffi;
mod geometry;
mod hit_test;
mod measure;
mod stroke;
mod svg_text;

unsafe extern "C" {
    fn ladybird_gfx_process_next_path_identity() -> u64;
    fn ladybird_gfx_path_destroy(path: *mut c_void);
    fn ladybird_gfx_path_equals(a: *const c_void, b: *const c_void) -> bool;
    fn ladybird_gfx_path_append_svg_string(
        path: *const c_void,
        append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
        context: *mut c_void,
    );
    fn ladybird_gfx_path_bounding_box(path: *const c_void, out_x_y_width_height: *mut f32);
    fn ladybird_gfx_path_set_fill_type(path: *mut c_void, winding_rule: i32);
    fn ladybird_gfx_path_serialize(
        path: *const c_void,
        append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
        context: *mut c_void,
    );
    fn ladybird_gfx_path_contains(path: *const c_void, x: f32, y: f32, winding_rule: i32) -> bool;
    fn ladybird_gfx_path_stroke_to_fill(
        path: *const c_void,
        thickness: f32,
        cap_style: i32,
        join_style: i32,
        miter_limit: f32,
        dash_array: *const f32,
        dash_count: usize,
        dash_offset: f32,
    ) -> *mut c_void;
    fn ladybird_gfx_path_length(path: *const c_void) -> f32;
    fn ladybird_gfx_path_point_at_length(path: *const c_void, distance: f32, out: *mut f32) -> bool;
    fn ladybird_gfx_path_create_from_ops(kinds: *const u8, values: *const f32, count: usize) -> *mut c_void;
    fn ladybird_gfx_path_append_ops(path: *mut c_void, kinds: *const u8, values: *const f32, count: usize);
    fn ladybird_gfx_path_append_path(path: *mut c_void, other: *const c_void);
    fn ladybird_gfx_path_is_empty(path: *const c_void) -> bool;
    fn ladybird_gfx_path_last_point(path: *const c_void, out_x_y: *mut f32);
    fn ladybird_gfx_path_clone(path: *const c_void) -> *mut c_void;
    fn ladybird_gfx_path_create_from_serialized_bytes(bytes: *const u8, count: usize) -> *mut c_void;
    fn ladybird_gfx_path_copy_transformed(path: *const c_void, affine_values: *const f32) -> *mut c_void;
    fn ladybird_gfx_path_create_from_glyph_runs(runs: *const FfiGlyphRun, run_count: usize) -> *mut c_void;
    fn ladybird_gfx_path_place_glyph_runs_along(
        path: *const c_void,
        runs: *const FfiGlyphRun,
        run_count: usize,
        offset: f32,
    ) -> *mut c_void;
}

/// A shaped run of glyphs in one font, as the path operations below take one.
pub struct GlyphRun {
    pub font: crate::font::FontHandle,
    pub glyphs: Vec<crate::text_layout::DrawGlyph>,
}

#[repr(C)]
struct FfiGlyphRun {
    font: *const c_void,
    glyphs: *const crate::text_layout::DrawGlyph,
    glyph_count: usize,
}

fn to_ffi_glyph_runs(runs: &[GlyphRun]) -> Vec<FfiGlyphRun> {
    runs.iter()
        .map(|run| FfiGlyphRun {
            font: run.font.as_raw(),
            glyphs: run.glyphs.as_ptr(),
            glyph_count: run.glyphs.len(),
        })
        .collect()
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

/// A `Gfx::Path` that is built on where it is: one that an [`OwnedPath`] or C++ owns, borrowed mutably for `'a`.
/// Each operation is that of the path, with its rules for a path that has no current point.
pub struct PathMut<'a> {
    raw: NonNull<c_void>,
    path: PhantomData<&'a mut c_void>,
}

impl PathMut<'_> {
    /// Borrows a `Gfx::Path` that C++ owns.
    ///
    /// # Safety
    ///
    /// `raw` must be a live `Gfx::Path`, which nothing else uses while the value lives.
    pub unsafe fn from_raw(raw: *mut c_void) -> Self {
        Self {
            raw: NonNull::new(raw).expect("Gfx::Path pointer must not be null"),
            path: PhantomData,
        }
    }

    fn apply(&mut self, kind: PathBuilderOpKind, values: [f32; 7]) {
        let kind = kind as u8;
        // SAFETY: The path is live and borrowed mutably, and the op is live for the call.
        unsafe { ladybird_gfx_path_append_ops(self.raw.as_ptr(), &raw const kind, values.as_ptr(), 1) };
    }

    /// Whether the path has no current point, which Gfx::Path::is_empty() calls empty.
    pub fn is_empty(&self) -> bool {
        // SAFETY: The path is live.
        unsafe { ladybird_gfx_path_is_empty(self.raw.as_ptr()) }
    }

    pub fn last_point(&self) -> FloatPoint {
        last_point_of(self.raw)
    }

    pub fn move_to(&mut self, point: FloatPoint) {
        self.apply(PathBuilderOpKind::MoveTo, [point.x, point.y, 0.0, 0.0, 0.0, 0.0, 0.0]);
    }

    pub fn line_to(&mut self, point: FloatPoint) {
        self.apply(PathBuilderOpKind::LineTo, [point.x, point.y, 0.0, 0.0, 0.0, 0.0, 0.0]);
    }

    pub fn quadratic_bezier_curve_to(&mut self, through: FloatPoint, point: FloatPoint) {
        self.apply(
            PathBuilderOpKind::QuadraticBezierCurveTo,
            [through.x, through.y, point.x, point.y, 0.0, 0.0, 0.0],
        );
    }

    pub fn cubic_bezier_curve_to(&mut self, c1: FloatPoint, c2: FloatPoint, point: FloatPoint) {
        self.apply(
            PathBuilderOpKind::CubicBezierCurveTo,
            [c1.x, c1.y, c2.x, c2.y, point.x, point.y, 0.0],
        );
    }

    pub fn elliptical_arc_to(
        &mut self,
        point: FloatPoint,
        radii: FloatSize,
        x_axis_rotation: f32,
        large_arc: bool,
        sweep: bool,
    ) {
        self.apply(
            PathBuilderOpKind::EllipticalArcTo,
            [
                point.x,
                point.y,
                radii.width,
                radii.height,
                x_axis_rotation,
                if large_arc { 1.0 } else { 0.0 },
                if sweep { 1.0 } else { 0.0 },
            ],
        );
    }

    pub fn arc_to(&mut self, point: FloatPoint, radius: f32, large_arc: bool, sweep: bool) {
        self.apply(
            PathBuilderOpKind::ArcTo,
            [
                point.x,
                point.y,
                radius,
                if large_arc { 1.0 } else { 0.0 },
                if sweep { 1.0 } else { 0.0 },
                0.0,
                0.0,
            ],
        );
    }

    pub fn close(&mut self) {
        self.apply(PathBuilderOpKind::Close, [0.0; 7]);
    }

    /// Adds all the subpaths of the other path to this one.
    pub fn append_path(&mut self, other: &OwnedPath) {
        // SAFETY: Both paths are live, and this one is borrowed mutably.
        unsafe { ladybird_gfx_path_append_path(self.raw.as_ptr(), other.raw.as_ptr()) };
    }
}

fn last_point_of(path: NonNull<c_void>) -> FloatPoint {
    let mut x_y = [0.0f32; 2];
    // SAFETY: The path is live for the call; the out-array holds two floats.
    unsafe { ladybird_gfx_path_last_point(path.as_ptr(), x_y.as_mut_ptr()) };
    FloatPoint { x: x_y[0], y: x_y[1] }
}

unsafe extern "C" fn append_to_vec(context: *mut c_void, bytes: *const u8, length: usize) {
    if length == 0 {
        return;
    }
    // SAFETY: `context` is the Vec pointer handed out by serialize_to_bytes; the bytes are live
    // for this synchronous call.
    let out = unsafe { &mut *context.cast::<Vec<u8>>() };
    out.extend_from_slice(unsafe { ak::slice_from_raw_parts(bytes, length) });
}

unsafe extern "C" fn append_to_string(context: *mut c_void, bytes: *const u8, length: usize) {
    // SAFETY: `context` is the String pointer passed by to_svg_string below, and the bytes are
    // live UTF-8 for this synchronous call.
    unsafe {
        let output = &mut *context.cast::<String>();
        if length > 0 {
            output.push_str(&String::from_utf8_lossy(ak::slice_from_raw_parts(bytes, length)));
        }
    }
}

/// How a path is stroked, as `OwnedPath::stroke_to_fill()` takes it.
#[derive(Clone, Debug, Default, PartialEq)]
pub struct StrokeStyle {
    pub thickness: f32,
    pub cap_style: crate::CapStyle,
    pub join_style: crate::JoinStyle,
    pub miter_limit: f32,
    pub dash_array: Vec<f32>,
    pub dash_offset: f32,
}

/// The sole owner of a heap-allocated `Gfx::Path`, destroying it on drop.
pub struct OwnedPath {
    raw: NonNull<c_void>,
    identity: u64,
}

// SAFETY: OwnedPath uniquely owns its Gfx::Path. Every operation on a shared reference is a const
// query, which only reads the path.
unsafe impl Send for OwnedPath {}
// SAFETY: See the Send implementation above.
unsafe impl Sync for OwnedPath {}

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
        Self {
            raw: NonNull::new(raw).expect("Gfx::Path pointer must not be null"),
            // The counter is LibGfx's: this crate is compiled into more than one library, and a
            // counter here would let each of them hand out the same identities. See
            // `LibGfx/RustProcessState.cpp`.
            // SAFETY: The counter is an atomic on the other side of the boundary.
            identity: unsafe { ladybird_gfx_process_next_path_identity() },
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

    /// The path, borrowed to be built on. The path gets a new identity, as it may no longer be the
    /// path that a snapshot holding the old identity copied.
    pub fn as_path_mut(&mut self) -> PathMut<'_> {
        // SAFETY: The counter is an atomic on the other side of the boundary.
        self.identity = unsafe { ladybird_gfx_process_next_path_identity() };
        PathMut {
            raw: self.raw,
            path: PhantomData,
        }
    }

    /// Whether the path has no current point, which Gfx::Path::is_empty() calls empty.
    pub fn is_empty(&self) -> bool {
        // SAFETY: The path is live for the duration of the call.
        unsafe { ladybird_gfx_path_is_empty(self.raw.as_ptr()) }
    }

    pub fn last_point(&self) -> FloatPoint {
        last_point_of(self.raw)
    }

    /// A process-unique, never-reused identity for this path allocation, so
    /// consumers holding a copied snapshot can recognize an unchanged path
    /// without comparing contents.
    #[inline]
    pub fn identity(&self) -> u64 {
        self.identity
    }

    pub fn set_fill_type(&mut self, winding_rule: i32) {
        // SAFETY: The path is live and owned by this handle.
        unsafe { ladybird_gfx_path_set_fill_type(self.as_raw(), winding_rule) };
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

    /// The point at the given distance along the path, followed by the path's tangent there.
    pub fn point_at_length(&self, distance: f32) -> Option<[f32; 4]> {
        let mut out = [0.0f32; 4];
        // SAFETY: The path is live for the duration of the call; the out-array holds four floats.
        unsafe { ladybird_gfx_path_point_at_length(self.raw.as_ptr(), distance, out.as_mut_ptr()) }.then_some(out)
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

    /// The outlines of the given glyphs, as one path.
    pub fn from_glyph_runs(runs: &[GlyphRun]) -> OwnedPath {
        let ffi_runs = to_ffi_glyph_runs(runs);
        // SAFETY: Each handle keeps its font live and each glyph slice stays valid for the
        // synchronous call, which returns a fresh heap-allocated Gfx::Path.
        unsafe {
            OwnedPath::adopt(ladybird_gfx_path_create_from_glyph_runs(
                ffi_runs.as_ptr(),
                ffi_runs.len(),
            ))
        }
    }

    /// The outlines of the given glyphs, laid along this path from `offset`.
    pub fn place_glyph_runs_along(&self, runs: &[GlyphRun], offset: f32) -> OwnedPath {
        let ffi_runs = to_ffi_glyph_runs(runs);
        // SAFETY: This path and every run's font stay live for the synchronous call, which
        // returns a fresh heap-allocated Gfx::Path.
        unsafe {
            OwnedPath::adopt(ladybird_gfx_path_place_glyph_runs_along(
                self.as_raw(),
                ffi_runs.as_ptr(),
                ffi_runs.len(),
                offset,
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

    /// The outline of the area that stroking this path with the given style covers, as a path to fill with the
    /// nonzero winding rule.
    pub fn stroke_to_fill(&self, style: &StrokeStyle) -> OwnedPath {
        // SAFETY: The path and the dash array are live for the synchronous call, which returns a fresh heap-allocated
        //         Gfx::Path.
        unsafe {
            OwnedPath::adopt(ladybird_gfx_path_stroke_to_fill(
                self.raw.as_ptr(),
                style.thickness,
                style.cap_style as i32,
                style.join_style as i32,
                style.miter_limit,
                style.dash_array.as_ptr(),
                style.dash_array.len(),
                style.dash_offset,
            ))
        }
    }

    pub fn to_svg_string(&self) -> String {
        let mut output = String::new();
        // SAFETY: The path is live for the duration of the call, and the callback only writes
        // into the String whose pointer it receives.
        unsafe { ladybird_gfx_path_append_svg_string(self.raw.as_ptr(), append_to_string, (&raw mut output).cast()) };
        output
    }
}

/// A copy of the path, which has an identity of its own.
impl Clone for OwnedPath {
    fn clone(&self) -> Self {
        // SAFETY: The path is live for the call; the result is a fresh heap-allocated Gfx::Path.
        unsafe { OwnedPath::adopt(ladybird_gfx_path_clone(self.raw.as_ptr())) }
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
