/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the 2D canvas contexts of LibWeb's C++ create the objects of this module and read them,
//! which the C++ classes of the interfaces declare (Libraries/LibWeb/HTML/CanvasGradient.cpp and the others). A cell
//! is the cell of an object of the interface that the name of the function says.

use core::ffi::c_void;

use libcompositing_rust::display_list::canvas_2d::Paint;
use libcompositing_rust::display_list::canvas_2d_recorder::FfiCanvas2DPaint;
use libcompositing_rust::display_list::commands::Canvas2DPaintKind;
use libjs_rust::api::Vm;

use super::canvas_gradient::CanvasGradient;
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
