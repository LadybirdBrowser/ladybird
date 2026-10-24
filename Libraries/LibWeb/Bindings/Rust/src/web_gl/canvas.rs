/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a WebGL context asks of the rest of LibWeb, which is C++: its canvas, the sources of texture images, and
//! the helpers that LibCompositing shares between the two sides of the command stream.
//! Libraries/LibWeb/WebGL/WebGLRenderingContextBase.cpp defines these functions.

use core::ffi::c_void;
use core::ptr::NonNull;

use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{Gc, Object, Realm};

use super::host::Frame;
use super::pixel_unpacking::PixelUnpackState;
use crate::bindings::conversions::CppInterface;
use crate::bindings::promise::{WebIdlPromise, promise_from_abi};

/// The sources of texture images that are implemented in C++, in the order of the C++ enum of the same name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum TexImageSourceKind {
    HTMLImageElement,
    HTMLCanvasElement,
    HTMLVideoElement,
    OffscreenCanvas,
}

/// The values of the C++ enum `TexImageSourceResult`.
const TEX_IMAGE_SOURCE_RESULT_FRAME: u8 = 0;
const TEX_IMAGE_SOURCE_RESULT_NO_FRAME: u8 = 1;
const TEX_IMAGE_SOURCE_RESULT_INVALID_VALUE: u8 = 2;

unsafe extern "C" {
    fn web_webgl_canvas_did_change_content(canvas_host: *mut c_void);
    fn web_webgl_canvas_bitmap_size(canvas_host: *mut c_void, out_width: *mut i32, out_height: *mut i32);
    fn web_webgl_canvas_wrapper(canvas: *mut c_void, canvas_is_offscreen: bool, realm: *mut c_void) -> *mut c_void;
    fn web_webgl_canvas_relevant_realm(canvas_host: *mut c_void) -> *mut c_void;
    safe fn web_webgl_expose_experimental_interfaces() -> bool;
    safe fn web_webgl_is_valid_2d_pixel_unpack_state(width: i32, unpack_state: &PixelUnpackState) -> bool;
    safe fn web_webgl_required_2d_texture_data_size(
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        unpack_state: &PixelUnpackState,
        out_size: &mut usize,
    ) -> bool;
    fn web_webgl_read_tex_image_source(
        source_wrapper: *mut c_void,
        source_kind: TexImageSourceKind,
        format: u32,
        r#type: u32,
        out_frame: *mut *mut c_void,
    ) -> u8;
    fn web_webgl_read_tex_image_frame(
        image_frame: *const c_void,
        format: u32,
        r#type: u32,
        out_frame: *mut *mut c_void,
    ) -> u8;
    fn web_webgl_read_tex_image_pixels(
        pixels: *const u8,
        width: u32,
        height: u32,
        format: u32,
        r#type: u32,
        out_frame: *mut *mut c_void,
    ) -> u8;
    fn web_webgl_make_xr_compatible(context: *mut c_void) -> *mut c_void;
}

#[derive(Clone, Copy)]
pub(crate) struct Canvas {
    /// The cell of the canvas, which the context keeps alive through a slot of its own.
    pub(crate) cell: NonNull<c_void>,
    pub(crate) host: NonNull<c_void>,
    pub(crate) is_offscreen: bool,
}

// SAFETY (of every call below): The context that holds the canvas keeps its cell alive, and the host is the same
// object.
impl Canvas {
    pub(crate) fn did_change_content(self) {
        unsafe { web_webgl_canvas_did_change_content(self.host.as_ptr()) };
    }

    pub(crate) fn bitmap_size(self) -> (i32, i32) {
        let (mut width, mut height) = (0, 0);
        unsafe { web_webgl_canvas_bitmap_size(self.host.as_ptr(), &raw mut width, &raw mut height) };
        (width, height)
    }

    pub(crate) fn wrapper(self, realm: Gc<Realm>) -> CppInterface {
        let wrapper = unsafe { web_webgl_canvas_wrapper(self.cell.as_ptr(), self.is_offscreen, realm.as_ptr().cast()) };
        let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns the wrapper of the canvas");
        // SAFETY: LibWeb returns a live wrapper of an HTMLCanvasElement or an OffscreenCanvas.
        unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }

    /// The realm of the relevant global object of the canvas, once that global object has a wrapper.
    pub(crate) fn relevant_realm(self) -> Option<Gc<Realm>> {
        let realm = unsafe { web_webgl_canvas_relevant_realm(self.host.as_ptr()) };
        // SAFETY: LibWeb returns a live realm, or null.
        NonNull::new(realm.cast::<Realm>()).map(|realm| unsafe { Gc::from_non_null(realm) })
    }
}

pub(crate) fn expose_experimental_interfaces() -> bool {
    web_webgl_expose_experimental_interfaces()
}

pub(crate) fn is_valid_2d_pixel_unpack_state(width: i32, unpack_state: &PixelUnpackState) -> bool {
    web_webgl_is_valid_2d_pixel_unpack_state(width, unpack_state)
}

pub(crate) fn required_2d_texture_data_size(
    width: i32,
    height: i32,
    format: u32,
    r#type: u32,
    unpack_state: &PixelUnpackState,
) -> Option<usize> {
    let mut size = 0;
    web_webgl_required_2d_texture_data_size(width, height, format, r#type, unpack_state, &mut size).then_some(size)
}

pub(crate) enum TexImageSourceFrame {
    Frame(Frame),
    /// The source has no image, or no image is uploaded with this format and type.
    NoFrame,
    InvalidValue,
}

pub(crate) fn read_tex_image_source(
    source: CppInterface,
    source_kind: TexImageSourceKind,
    format: u32,
    r#type: u32,
) -> TexImageSourceFrame {
    let mut frame = core::ptr::null_mut();
    // SAFETY: The wrapper is live and of the interface that the kind names, and the frame is stored when the
    //         function returns that there is one.
    let result = unsafe {
        web_webgl_read_tex_image_source(
            source.wrapper().as_ptr().cast(),
            source_kind,
            format,
            r#type,
            &raw mut frame,
        )
    };
    tex_image_source_frame(result, frame)
}

/// The frame of an image, a frame of an ImageBitmap, for the format and the type.
pub(crate) fn read_tex_image_frame(image_frame: &ImageFrameHandle, format: u32, r#type: u32) -> TexImageSourceFrame {
    let mut frame = core::ptr::null_mut();
    // SAFETY: The handle keeps the image frame alive, and the frame is stored when the function returns that there is
    //         one.
    let result = unsafe { web_webgl_read_tex_image_frame(image_frame.as_raw(), format, r#type, &raw mut frame) };
    tex_image_source_frame(result, frame)
}

/// The frame of a copy of the pixels of an ImageData, for the format and the type.
///
/// # Safety
///
/// `pixels` must be the width × height RGBA8888 pixels of the ImageData, which no script may change during the call.
pub(crate) unsafe fn read_tex_image_pixels(
    pixels: NonNull<u8>,
    width: u32,
    height: u32,
    format: u32,
    r#type: u32,
) -> TexImageSourceFrame {
    let mut frame = core::ptr::null_mut();
    // SAFETY: The caller passes the pixels, and the frame is stored when the function returns that there is one.
    let result =
        unsafe { web_webgl_read_tex_image_pixels(pixels.as_ptr(), width, height, format, r#type, &raw mut frame) };
    tex_image_source_frame(result, frame)
}

fn tex_image_source_frame(result: u8, frame: *mut c_void) -> TexImageSourceFrame {
    match result {
        TEX_IMAGE_SOURCE_RESULT_FRAME => {
            let frame = NonNull::new(frame).expect("LibWeb returns the frame");
            // SAFETY: LibWeb hands over a frame that it allocated.
            TexImageSourceFrame::Frame(unsafe { Frame::from_raw(frame) })
        }
        TEX_IMAGE_SOURCE_RESULT_NO_FRAME => TexImageSourceFrame::NoFrame,
        TEX_IMAGE_SOURCE_RESULT_INVALID_VALUE => TexImageSourceFrame::InvalidValue,
        unknown_result => unreachable!("LibWeb returned {unknown_result} for the reading of a texture image source"),
    }
}

/// LibWeb's C++ code settles the promise.
///
/// # Safety
///
/// `context` is the cell of a WebGL context.
pub(crate) unsafe fn make_xr_compatible(context: *mut c_void) -> WebIdlPromise {
    // SAFETY: The caller passes the cell of a context, and LibWeb returns a live promise.
    unsafe { promise_from_abi(web_webgl_make_xr_compatible(context)) }
}
