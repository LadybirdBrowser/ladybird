/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the 2D canvas contexts of LibWeb's C++ create the objects of this module and read them,
//! which the C++ classes of the interfaces declare (Libraries/LibWeb/HTML/CanvasGradient.cpp and the others). A cell
//! is the cell of an object of the interface that the name of the function says.

use core::ffi::c_void;

use ak::Utf16String;
use libcompositing_rust::display_list::canvas_2d::Paint;
use libcompositing_rust::display_list::canvas_2d_recorder::FfiCanvas2DPaint;
use libcompositing_rust::display_list::commands::{Canvas2DPaintKind, Canvas2DPatternRepetition};
use libgfx_rust::AffineTransform;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::path::PathMut;
use libjs_rust::api::Vm;

use super::canvas_gradient::CanvasGradient;
use super::canvas_path::{self, CanvasPathError, RoundRectRadius};
use super::canvas_pattern::CanvasPattern;
use super::text_metrics::{TextMeasurements, TextMetrics};
use crate::bindings::interface::{Interface, Ref, cell_from_abi, cell_into_abi};

/// # Safety
///
/// `vm` must be the live VM.
unsafe fn vm_from_abi<'vm>(vm: *mut c_void) -> &'vm Vm {
    assert!(!vm.is_null(), "C++ passes the VM");
    // SAFETY: The caller passes the live VM.
    unsafe { &*vm.cast::<Vm>() }
}

/// The implementation of the object whose cell C++ lends, for as long as C++ keeps the cell alive.
///
/// # Safety
///
/// `cell` must be the live cell of an object of the interface, and it must outlive `'a`.
unsafe fn object_from_abi<'a, T: Interface>(cell: *mut c_void) -> &'a T {
    // SAFETY: The caller passes a live cell of the interface.
    let object: Ref<T> = unsafe { cell_from_abi(cell) };
    // SAFETY: The caller keeps the cell alive for 'a.
    unsafe { &*object.as_ptr() }
}

/// Writes the paint into what C++ lends the recorder.
fn lend_paint<'a>(paint: Paint<'a>, out_paint: &mut FfiCanvas2DPaint<'a>) {
    match paint {
        Paint::Color(color) => {
            out_paint.kind = Canvas2DPaintKind::Color;
            out_paint.color = color;
        }
        Paint::LinearGradient {
            start_point,
            end_point,
            color_stops,
        } => {
            out_paint.kind = Canvas2DPaintKind::LinearGradient;
            out_paint.start_point = start_point;
            out_paint.end_point = end_point;
            out_paint.color_stop_colors = ak::Span::new(color_stops.colors);
            out_paint.color_stop_positions = ak::Span::new(color_stops.positions);
        }
        Paint::RadialGradient {
            start_center,
            start_radius,
            end_center,
            end_radius,
            color_stops,
        } => {
            out_paint.kind = Canvas2DPaintKind::RadialGradient;
            out_paint.start_point = start_center;
            out_paint.start_radius = start_radius;
            out_paint.end_point = end_center;
            out_paint.end_radius = end_radius;
            out_paint.color_stop_colors = ak::Span::new(color_stops.colors);
            out_paint.color_stop_positions = ak::Span::new(color_stops.positions);
        }
        Paint::ConicGradient {
            center,
            start_angle,
            color_stops,
        } => {
            out_paint.kind = Canvas2DPaintKind::ConicGradient;
            out_paint.start_point = center;
            out_paint.start_angle = start_angle;
            out_paint.color_stop_colors = ak::Span::new(color_stops.colors);
            out_paint.color_stop_positions = ak::Span::new(color_stops.positions);
        }
        Paint::Pattern { .. } => unreachable!("a pattern lends its image, which a paint names by an id"),
    }
}

/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_gradient_create_linear(
    vm: *mut c_void,
    x0: f64,
    y0: f64,
    x1: f64,
    y1: f64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(CanvasGradient::create_linear(vm, x0, y0, x1, y1))
}

/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_gradient_create_radial(
    vm: *mut c_void,
    x0: f64,
    y0: f64,
    r0: f64,
    x1: f64,
    y1: f64,
    r1: f64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(CanvasGradient::create_radial(vm, x0, y0, r0, x1, y1, r1))
}

/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_gradient_create_conic(
    vm: *mut c_void,
    start_angle: f64,
    x: f64,
    y: f64,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(CanvasGradient::create_conic(vm, start_angle, x, y))
}

/// # Safety
///
/// `gradient` must be a live cell.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_gradient_is_visible(gradient: *mut c_void) -> bool {
    // SAFETY: The caller passes a live cell.
    unsafe { object_from_abi::<CanvasGradient>(gradient) }.is_visible()
}

/// Lends the recorder the geometry and the stops of the gradient.
///
/// # Safety
///
/// `gradient` must be a live cell, which outlives the use of the paint, and nothing may add a stop to it meanwhile.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_gradient_lend_paint<'a>(
    gradient: *mut c_void,
    out_paint: &mut FfiCanvas2DPaint<'a>,
) {
    // SAFETY: The caller passes a live cell, which outlives 'a.
    let gradient: &'a CanvasGradient = unsafe { object_from_abi(gradient) };
    // SAFETY: The caller adds no stop while the paint is in use.
    lend_paint(unsafe { gradient.paint_unguarded() }, out_paint);
}

/// # Safety
///
/// `vm` must be the live VM.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_text_metrics_create(vm: *mut c_void, measurements: &TextMeasurements) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    cell_into_abi(TextMetrics::create(vm, *measurements))
}

/// `frame` is a Gfx::DecodedImageFrame that the caller lends for the call, which the pattern keeps a copy of, or null.
///
/// # Safety
///
/// `vm` must be the live VM, and `frame` a live frame or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_pattern_create(
    vm: *mut c_void,
    frame: *const c_void,
    repetition: Canvas2DPatternRepetition,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM.
    let vm = unsafe { vm_from_abi(vm) };
    // SAFETY: The caller passes a live frame.
    let image = (!frame.is_null()).then(|| unsafe { ImageFrameHandle::retain(frame) });
    cell_into_abi(CanvasPattern::create(vm, image, repetition))
}

/// Lends the recorder the image, the repetition and the transformation matrix of the pattern.
///
/// # Safety
///
/// `pattern` must be a live cell, which outlives the use of the paint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_pattern_lend_paint(pattern: *mut c_void, out_paint: &mut FfiCanvas2DPaint<'_>) {
    // SAFETY: The caller passes a live cell, which keeps the image alive while the paint is in use.
    let pattern: &CanvasPattern = unsafe { object_from_abi(pattern) };
    out_paint.kind = Canvas2DPaintKind::Pattern;
    out_paint.pattern_repetition = pattern.repetition();
    out_paint.pattern_image = pattern.image().map_or(core::ptr::null(), ImageFrameHandle::as_raw);
    if let Some(transform) = pattern.transform() {
        out_paint.has_pattern_transform = true;
        out_paint.pattern_transform = transform;
    }
}

/// What an operation of a canvas path threw, which `CanvasPathResult` of Libraries/LibWeb/HTML/Canvas/CanvasPath.cpp
/// mirrors.
#[derive(Clone, Copy)]
#[repr(u8)]
pub enum CanvasPathResultKind {
    Ok,
    IndexSizeError,
    RangeError,
}

/// The result of an operation of a canvas path, with the message of its exception, an AK::Utf16String that the
/// caller takes over, unless the operation threw none.
#[repr(C)]
pub struct CanvasPathResult {
    kind: CanvasPathResultKind,
    message: usize,
}

impl From<Result<(), CanvasPathError>> for CanvasPathResult {
    fn from(result: Result<(), CanvasPathError>) -> Self {
        match result {
            Ok(()) => Self {
                kind: CanvasPathResultKind::Ok,
                message: 0,
            },
            Err(error) => Self {
                kind: if error.is_range_error() {
                    CanvasPathResultKind::RangeError
                } else {
                    CanvasPathResultKind::IndexSizeError
                },
                message: Utf16String::from_utf8(&error.message()).into_raw(),
            },
        }
    }
}

// NB: `path` of every operation below is a Gfx::Path that the caller lends mutably for the call.

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_close_path(path: *mut c_void) {
    // SAFETY: The caller lends a live path.
    canvas_path::close_path(&mut unsafe { PathMut::from_raw(path) });
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_move_to(path: *mut c_void, x: f32, y: f32) {
    // SAFETY: The caller lends a live path.
    canvas_path::move_to(&mut unsafe { PathMut::from_raw(path) }, x, y);
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_line_to(path: *mut c_void, x: f32, y: f32) {
    // SAFETY: The caller lends a live path.
    canvas_path::line_to(&mut unsafe { PathMut::from_raw(path) }, x, y);
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_quadratic_curve_to(path: *mut c_void, cpx: f32, cpy: f32, x: f32, y: f32) {
    // SAFETY: The caller lends a live path.
    canvas_path::quadratic_curve_to(&mut unsafe { PathMut::from_raw(path) }, cpx, cpy, x, y);
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_bezier_curve_to(
    path: *mut c_void,
    cp1x: f64,
    cp1y: f64,
    cp2x: f64,
    cp2y: f64,
    x: f64,
    y: f64,
) {
    // SAFETY: The caller lends a live path.
    canvas_path::bezier_curve_to(&mut unsafe { PathMut::from_raw(path) }, cp1x, cp1y, cp2x, cp2y, x, y);
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_arc_to(
    path: *mut c_void,
    transform: &AffineTransform,
    x1: f64,
    y1: f64,
    x2: f64,
    y2: f64,
    radius: f64,
) -> CanvasPathResult {
    // SAFETY: The caller lends a live path.
    canvas_path::arc_to(
        &mut unsafe { PathMut::from_raw(path) },
        *transform,
        x1,
        y1,
        x2,
        y2,
        radius,
    )
    .into()
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_rect(path: *mut c_void, x: f64, y: f64, w: f64, h: f64) {
    // SAFETY: The caller lends a live path.
    canvas_path::rect(&mut unsafe { PathMut::from_raw(path) }, x, y, w, h);
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_round_rect(
    path: *mut c_void,
    x: f64,
    y: f64,
    w: f64,
    h: f64,
    radii: ak::Span<'_, RoundRectRadius>,
) -> CanvasPathResult {
    // SAFETY: The caller lends a live path.
    canvas_path::round_rect(&mut unsafe { PathMut::from_raw(path) }, x, y, w, h, radii.as_slice()).into()
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_arc(
    path: *mut c_void,
    x: f32,
    y: f32,
    radius: f32,
    start_angle: f32,
    end_angle: f32,
    counterclockwise: bool,
) -> CanvasPathResult {
    // SAFETY: The caller lends a live path.
    let mut path = unsafe { PathMut::from_raw(path) };
    canvas_path::arc(&mut path, x, y, radius, start_angle, end_angle, counterclockwise).into()
}

/// # Safety
///
/// `path` must be a live Gfx::Path.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_path_ellipse(
    path: *mut c_void,
    x: f32,
    y: f32,
    radius_x: f32,
    radius_y: f32,
    rotation: f32,
    start_angle: f32,
    end_angle: f32,
    counterclockwise: bool,
) -> CanvasPathResult {
    // SAFETY: The caller lends a live path.
    let mut path = unsafe { PathMut::from_raw(path) };
    canvas_path::ellipse(
        &mut path,
        x,
        y,
        radius_x,
        radius_y,
        rotation,
        start_angle,
        end_angle,
        counterclockwise,
    )
    .into()
}
