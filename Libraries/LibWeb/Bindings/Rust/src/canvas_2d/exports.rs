/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI that Libraries/LibWeb/HTML/Canvas/Canvas2DContextBase.cpp declares, and calls with the cell of a context
//! of either kind.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;

use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, ColorSpace};
use libgfx_rust::{IntRect, IntSize};

use super::canvas::Canvas;
use super::context::{ContextParameters, ContextState, Offscreen, OnScreen, RenderingContext2D};
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Interface, allocate, cell_from_abi, cell_into_abi};
use crate::html::canvas_draw_image::CanvasImageSource;
use crate::html::canvas_pattern::read_back_current_video_frame;
use crate::html::canvas_rendering_context2d_settings::{CanvasColorType, CanvasRenderingContext2DSettings};
use crate::html::image_bitmap::ImageBitmap;
use crate::html::predefined_color_space::PredefinedColorSpace;

/// Mirrors `Web::HTML::ContextCreationArguments`.
#[repr(C)]
pub struct ContextCreationArguments {
    canvas: *mut c_void,
    canvas_host: *mut c_void,
    canvas_is_offscreen: bool,
    width: i32,
    height: i32,
    alpha: bool,
    desynchronized: bool,
    /// The underlying value of a Web::Bindings::PredefinedColorSpace.
    color_space: u8,
    /// The underlying value of a Web::Bindings::CanvasColorType.
    color_type: u8,
    will_read_frequently: bool,
}

/// The cell of a CanvasRenderingContext2D or of an OffscreenCanvasRenderingContext2D, which only C++ lends out, for as
/// long as the context is live.
#[repr(C)]
pub struct ContextCell {
    // NB: The cell is mutable behind a shared reference, and bigger than Rust sees, so a reference to one must not tell
    //     the compiler that the memory behind it is read only or unaliased.
    _opaque: core::cell::UnsafeCell<[u8; 0]>,
    _not_send_sync_or_unpin: PhantomData<(*const (), core::marker::PhantomPinned)>,
}

fn with_context<R>(context: &ContextCell, function: impl FnOnce(&ContextState) -> R) -> R {
    let cell = NonNull::from(context).cast();
    // SAFETY: A ContextCell is the live cell of one of the two classes, which the downcasts tell apart.
    unsafe {
        if let Some(context) = RenderingContext2D::<OnScreen>::CELL_CLASS.downcast(cell) {
            return function(&context);
        }
        let context = RenderingContext2D::<Offscreen>::CELL_CLASS
            .downcast(cell)
            .expect("the cell is a 2D context");
        function(&context)
    }
}

/// # Safety
///
/// The pointers of the arguments are what [`ContextCreationArguments`] describes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_context_create(
    vm: &libjs_rust::api::Vm,
    arguments: &ContextCreationArguments,
) -> *mut c_void {
    let canvas = Canvas {
        cell: NonNull::new(arguments.canvas).expect("C++ passes the canvas"),
        host: NonNull::new(arguments.canvas_host).expect("C++ passes the canvas"),
        is_offscreen: arguments.canvas_is_offscreen,
    };
    let parameters = ContextParameters {
        canvas,
        context_attributes: CanvasRenderingContext2DSettings {
            alpha: arguments.alpha,
            desynchronized: arguments.desynchronized,
            color_space: PredefinedColorSpace::from_u8(arguments.color_space).expect("C++ passes a color space"),
            color_type: CanvasColorType::from_u8(arguments.color_type).expect("C++ passes a color type"),
            will_read_frequently: arguments.will_read_frequently,
        },
        size: IntSize {
            width: arguments.width,
            height: arguments.height,
        },
    };
    if arguments.canvas_is_offscreen {
        cell_into_abi(allocate(vm, |defer_gc| {
            RenderingContext2D::<Offscreen>::new(defer_gc, ContextState::new(defer_gc, parameters))
        }))
    } else {
        cell_into_abi(allocate(vm, |defer_gc| {
            RenderingContext2D::<OnScreen>::new(defer_gc, ContextState::new(defer_gc, parameters))
        }))
    }
}

/// The Web::HTML::CanvasHost of the canvas of the context.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_canvas_host(context: &ContextCell) -> *mut c_void {
    with_context(context, |context| context.canvas_handle().host.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_origin_clean(context: &ContextCell) -> bool {
    with_context(context, ContextState::origin_clean)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_set_size(context: &ContextCell, width: i32, height: i32) {
    with_context(context, |context| context.set_size(IntSize { width, height }));
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_reset_to_default_state(context: &ContextCell) {
    with_context(context, ContextState::reset_to_default_state);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_ensure_backing_storage(context: &ContextCell) {
    with_context(context, ContextState::ensure_backing_storage);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_discard_backing_storage(context: &ContextCell) {
    with_context(context, ContextState::discard_backing_storage);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_has_backing_storage(context: &ContextCell) -> bool {
    with_context(context, ContextState::has_backing_storage)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_drop_cached_readback(context: &ContextCell) {
    with_context(context, ContextState::drop_cached_readback);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_is_lost(context: &ContextCell) -> bool {
    with_context(context, ContextState::context_is_lost)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_set_lost(context: &ContextCell, lost: bool) {
    with_context(context, |context| context.set_context_lost(lost));
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_prepare_for_compositing(context: &ContextCell) {
    with_context(context, ContextState::prepare_for_compositing);
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_canvas_id(context: &ContextCell, out_canvas_id: &mut u64) -> bool {
    let Some(canvas_id) = with_context(context, ContextState::canvas_id) else {
        return false;
    };
    *out_canvas_id = canvas_id.0;
    true
}

/// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-color-space
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_color_space(context: &ContextCell) -> ColorSpace {
    with_context(context, |context| context.context_attributes.color_space.into())
}

/// A Gfx::Bitmap with a reference that the caller takes over, or null. Unpremultiplied pixels come back as RGBA8888,
/// and premultiplied pixels as BGRA8888, in the color space.
#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_read_pixels(
    context: &ContextCell,
    rect: IntRect,
    alpha_type: AlphaType,
    color_space: ColorSpace,
) -> *mut c_void {
    with_context(context, |context| context.read_pixels(rect, alpha_type, color_space))
        .map_or(core::ptr::null_mut(), super::host::Bitmap::into_raw)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_canvas_2d_context_replace_bitmap_with_cleared_bitmap(context: &ContextCell) {
    with_context(context, ContextState::replace_bitmap_with_cleared_bitmap);
}

/// Draws the ImageBitmap at the point as drawImage() does, and returns whether that threw nothing.
///
/// # Safety
///
/// `image_bitmap` must be a live cell of an ImageBitmap.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_context_draw_image_bitmap(
    context: &ContextCell,
    image_bitmap: *mut c_void,
    x: f32,
    y: f32,
) -> bool {
    // SAFETY: The caller passes a live cell of an ImageBitmap.
    let image_bitmap = unsafe { cell_from_abi::<ImageBitmap>(image_bitmap) };
    let image = CanvasImageSource::ImageBitmap(image_bitmap);
    let (width, height) = image.size();
    #[allow(
        clippy::cast_precision_loss,
        reason = "a size of an image becomes a float, as the C++ had it"
    )]
    let (width, height) = (width as f32, height as f32);
    with_context(context, |context| {
        context
            .draw_image_internal(image, 0.0, 0.0, width, height, x, y, width, height)
            .is_ok()
    })
}

unsafe extern "C" {
    /// Sets the Optional<Gfx::DecodedImageFrame> to a copy of the frame, which is lent for the call.
    fn web_canvas_2d_set_video_frame_copy(out_frame: *mut c_void, frame: *const c_void);
}

/// Sets the Optional<Gfx::DecodedImageFrame> to a copy of the frame of the video at its current playback position, and
/// returns whether the video has one.
///
/// # Safety
///
/// `video` must be a live wrapper of an HTMLVideoElement, and `out_frame` an Optional<Gfx::DecodedImageFrame>.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_canvas_2d_read_back_video_frame(video: *mut c_void, out_frame: *mut c_void) -> bool {
    let video = NonNull::new(video.cast::<libjs_rust::api::Object>()).expect("C++ passes the video");
    // SAFETY: The caller passes a live wrapper of an HTMLVideoElement.
    let video = unsafe { CppInterface::from_wrapper(libjs_rust::api::Gc::from_non_null(video)) };
    // NB: The colors of a video can be outside the gamut of sRGB, so the frame is read back in the widest color space of
    //     a canvas.
    let Some(frame) = read_back_current_video_frame(video, ColorSpace::DisplayP3) else {
        return false;
    };
    // SAFETY: The handle keeps the frame alive for the call, and the caller passes the optional.
    unsafe { web_canvas_2d_set_video_frame_copy(out_frame, frame.as_raw()) };
    true
}
