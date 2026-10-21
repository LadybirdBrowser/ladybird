/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which the 2D canvas contexts of LibWeb record, which Libraries/LibWeb/HTML/Canvas/Canvas2DRecorder.cpp
//! declares. A recorder owns the transport it records through. The structs in which C++ lends a paint, a stroke and how
//! a draw composites are those of display_list/canvas_2d_recorder.rs, which LibCompositing/RustFFI.h declares to C++.

use core::ffi::c_void;
use core::ptr::NonNull;

use libcompositing_rust::display_list::canvas_2d::Paint;
use libcompositing_rust::display_list::canvas_2d_recorder::{
    AlphaType, FfiCanvas2DCompositing, FfiCanvas2DPaint, FfiCanvas2DStroke, Recorder,
};
use libcompositing_rust::display_list::commands::{Canvas2DPaintKind, CanvasId, DisplayListGlyph};
use libgfx_rust::{AffineTransform, Color, FloatPoint, FloatRect, IntRect, IntSize, ScalingMode, WindingRule};

use super::host::{CppTransport, LentFont, LentImageFrame, LentVideoFrameHold};

pub(crate) type CanvasRecorder = Recorder<CppTransport>;

/// The paint, with the image of a pattern added to the stream.
fn paint_of<'a>(recorder: &CanvasRecorder, paint: &FfiCanvas2DPaint<'a>) -> Paint<'a> {
    let pattern_image = match paint.kind {
        Canvas2DPaintKind::Pattern => {
            NonNull::new(paint.pattern_image.cast_mut()).map(|frame| recorder.add_image_frame(&LentImageFrame(frame)))
        }
        _ => None,
    };
    paint.to_paint(pattern_image)
}

/// # Safety
///
/// `transport` is a RemoteCanvas2DTransport, one reference to which the recorder takes over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_recorder_create(transport: *mut c_void) -> *mut CanvasRecorder {
    let transport = NonNull::new(transport).expect("C++ passes the transport");
    // SAFETY: The caller hands over a reference to the transport.
    let transport = unsafe { CppTransport::from_raw(transport) };
    Box::into_raw(Box::new(Recorder::new(transport)))
}

/// # Safety
///
/// `recorder` comes from web_canvas_2d_recorder_create(), and nothing uses it after this.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_recorder_destroy(recorder: *mut CanvasRecorder) {
    assert!(!recorder.is_null());
    // SAFETY: The caller passes a recorder that web_canvas_2d_recorder_create() leaked.
    drop(unsafe { Box::from_raw(recorder) });
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_create_context(
    recorder: &CanvasRecorder,
    width: i32,
    height: i32,
    alpha: bool,
) -> bool {
    recorder.create_context(IntSize { width, height }, alpha)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_canvas_id(recorder: &CanvasRecorder, out_canvas_id: &mut u64) -> bool {
    let Some(canvas_id) = recorder.canvas_id() else {
        return false;
    };
    *out_canvas_id = canvas_id.0;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_destroy_context(recorder: &CanvasRecorder) {
    recorder.destroy_context();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_flush(recorder: &CanvasRecorder) {
    recorder.flush();
}

/// Returns a Gfx::Bitmap with one reference, which the caller takes over, or null.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_read_back_pixels(
    recorder: &CanvasRecorder,
    rect: IntRect,
    alpha_type: AlphaType,
) -> *mut c_void {
    recorder
        .read_back_pixels(rect, alpha_type)
        .map_or(core::ptr::null_mut(), |bitmap| bitmap.into_raw())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_present(recorder: &CanvasRecorder) {
    recorder.present();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_prepare_to_record(recorder: &CanvasRecorder) {
    recorder.prepare_to_record();
}

/// `path` is a serialized Gfx::Path.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_fill_path(
    recorder: &mut CanvasRecorder,
    path: ak::Span<'_, u8>,
    paint: &FfiCanvas2DPaint<'_>,
    winding_rule: WindingRule,
    blur_radius: f32,
    compositing: &FfiCanvas2DCompositing<'_>,
) {
    let paint = paint_of(recorder, paint);
    recorder.fill_path(
        path.as_slice(),
        &paint,
        winding_rule,
        blur_radius,
        &compositing.to_compositing(),
    );
}

/// `path` is a serialized Gfx::Path.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_stroke_path(
    recorder: &mut CanvasRecorder,
    path: ak::Span<'_, u8>,
    paint: &FfiCanvas2DPaint<'_>,
    stroke: &FfiCanvas2DStroke<'_>,
    blur_radius: f32,
    compositing: &FfiCanvas2DCompositing<'_>,
) {
    let paint = paint_of(recorder, paint);
    recorder.stroke_path(
        path.as_slice(),
        &paint,
        &stroke.to_stroke(),
        blur_radius,
        &compositing.to_compositing(),
    );
}

/// `font` is a Gfx::Font that the caller lends for the call.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_draw_glyph_run(
    recorder: &mut CanvasRecorder,
    font: *const c_void,
    glyphs: ak::Span<'_, DisplayListGlyph>,
    translation: FloatPoint,
    paint: &FfiCanvas2DPaint<'_>,
    compositing: &FfiCanvas2DCompositing<'_>,
) {
    let font = LentFont(NonNull::new(font.cast_mut()).expect("C++ passes the font"));
    let paint = paint_of(recorder, paint);
    recorder.draw_glyph_run(
        &font,
        glyphs.as_slice(),
        translation,
        &paint,
        &compositing.to_compositing(),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_clear_rect(recorder: &mut CanvasRecorder, rect: FloatRect, color: Color) {
    recorder.clear_rect(rect, color);
}

/// `frame` is a Gfx::DecodedImageFrame that the caller lends for the call.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_draw_image_frame(
    recorder: &mut CanvasRecorder,
    frame: *const c_void,
    dst_rect: FloatRect,
    src_rect: IntRect,
    scaling_mode: ScalingMode,
    compositing: &FfiCanvas2DCompositing<'_>,
) {
    let frame = LentImageFrame(NonNull::new(frame.cast_mut()).expect("C++ passes the frame"));
    recorder.draw_image_frame(&frame, dst_rect, src_rect, scaling_mode, &compositing.to_compositing());
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_draw_canvas(
    recorder: &mut CanvasRecorder,
    source_canvas_id: u64,
    dst_rect: FloatRect,
    src_rect: IntRect,
    scaling_mode: ScalingMode,
    compositing: &FfiCanvas2DCompositing<'_>,
) {
    recorder.draw_canvas(
        CanvasId(source_canvas_id),
        dst_rect,
        src_rect,
        scaling_mode,
        &compositing.to_compositing(),
    );
}

/// `hold` is a Compositing::VideoFrameHold that the caller lends for the call.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_draw_video_frame(
    recorder: &mut CanvasRecorder,
    hold: *mut c_void,
    video_sink_handle: u64,
    generation: u64,
    dst_rect: FloatRect,
    src_rect: IntRect,
    scaling_mode: ScalingMode,
    compositing: &FfiCanvas2DCompositing<'_>,
) {
    let hold = LentVideoFrameHold(NonNull::new(hold).expect("C++ passes the hold"));
    recorder.draw_video_frame(
        &hold,
        video_sink_handle,
        generation,
        dst_rect,
        src_rect,
        scaling_mode,
        &compositing.to_compositing(),
    );
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_set_transform(recorder: &mut CanvasRecorder, transform: &AffineTransform) {
    recorder.set_transform(*transform);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_save(recorder: &mut CanvasRecorder) {
    recorder.save();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_restore(recorder: &mut CanvasRecorder) {
    recorder.restore();
}

/// `path` is a serialized Gfx::Path.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_clip_path(
    recorder: &mut CanvasRecorder,
    path: ak::Span<'_, u8>,
    winding_rule: WindingRule,
) {
    recorder.clip_path(path.as_slice(), winding_rule);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_reset(recorder: &mut CanvasRecorder) {
    recorder.reset();
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_recorder_clear_canvas(recorder: &mut CanvasRecorder, color: Color) {
    recorder.clear_canvas(color);
}
