/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transport of a 2D canvas in the product: a RemoteCanvas2DTransport of LibWeb, through the C ABI that
//! Libraries/LibWeb/HTML/Canvas/RemoteCanvas2DTransportFFI.cpp gives it.

use core::ffi::c_void;
use core::ptr::NonNull;

use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, Transport};
use libcompositing_rust::display_list::commands::{CanvasId, FontResourceId, ImageFrameResourceId};
use libgfx_rust::{IntRect, IntSize};

unsafe extern "C" {
    fn web_canvas_2d_transport_unref(transport: *mut c_void);
    fn web_canvas_2d_transport_create_context(transport: *mut c_void, width: i32, height: i32, alpha: bool) -> bool;
    fn web_canvas_2d_transport_canvas_id(transport: *const c_void, out_canvas_id: *mut u64) -> bool;
    fn web_canvas_2d_transport_destroy_context(transport: *mut c_void);
    fn web_canvas_2d_transport_append_command(transport: *mut c_void, record: ak::Span<'_, u8>);
    fn web_canvas_2d_transport_add_font(transport: *mut c_void, font: *const c_void) -> u64;
    fn web_canvas_2d_transport_add_image_frame(transport: *mut c_void, frame: *const c_void) -> u64;
    fn web_canvas_2d_transport_retain_video_frame_hold(transport: *mut c_void, hold: *mut c_void);
    fn web_canvas_2d_transport_record_present(transport: *mut c_void);
    fn web_canvas_2d_transport_pending_command_count(transport: *const c_void) -> usize;
    fn web_canvas_2d_transport_flush(transport: *mut c_void);
    fn web_canvas_2d_transport_read_back_pixels(
        transport: *mut c_void,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        alpha_type: AlphaType,
    ) -> *mut c_void;
    fn web_canvas_2d_bitmap_unref(bitmap: *mut c_void);
}

/// A Gfx::Font that C++ lends.
pub(crate) struct LentFont(pub(crate) NonNull<c_void>);

/// A Gfx::DecodedImageFrame that C++ lends.
pub(crate) struct LentImageFrame(pub(crate) NonNull<c_void>);

/// A Compositing::VideoFrameHold that C++ lends.
pub(crate) struct LentVideoFrameHold(pub(crate) NonNull<c_void>);

/// A Gfx::Bitmap, one reference to which the value owns.
pub(crate) struct Bitmap(NonNull<c_void>);

impl Bitmap {
    pub(crate) fn into_raw(self) -> *mut c_void {
        let bitmap = self.0.as_ptr();
        core::mem::forget(self);
        bitmap
    }
}

impl Drop for Bitmap {
    fn drop(&mut self) {
        // SAFETY: The value owns one reference to the bitmap.
        unsafe { web_canvas_2d_bitmap_unref(self.0.as_ptr()) };
    }
}

pub(crate) struct CppTransport(NonNull<c_void>);

impl CppTransport {
    /// # Safety
    ///
    /// `transport` is a RemoteCanvas2DTransport, one reference to which the value takes over.
    pub(crate) unsafe fn from_raw(transport: NonNull<c_void>) -> Self {
        Self(transport)
    }

    fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }
}

impl Drop for CppTransport {
    fn drop(&mut self) {
        // SAFETY: The value owns one reference to the transport.
        unsafe { web_canvas_2d_transport_unref(self.as_ptr()) };
    }
}

// SAFETY (of every call below): The value holds a reference to the transport, and the font, the image frame and the
// video frame hold passed are lent for the call.
impl Transport for CppTransport {
    type Font = LentFont;
    type ImageFrame = LentImageFrame;
    type VideoFrameHold = LentVideoFrameHold;
    type Bitmap = Bitmap;

    fn create_context(&self, size: IntSize, alpha: bool) -> bool {
        unsafe { web_canvas_2d_transport_create_context(self.as_ptr(), size.width, size.height, alpha) }
    }

    fn canvas_id(&self) -> Option<CanvasId> {
        let mut canvas_id = 0;
        unsafe { web_canvas_2d_transport_canvas_id(self.as_ptr(), &raw mut canvas_id) }.then_some(CanvasId(canvas_id))
    }

    fn destroy_context(&self) {
        unsafe { web_canvas_2d_transport_destroy_context(self.as_ptr()) };
    }

    fn append_command(&self, record: &[u8]) {
        unsafe { web_canvas_2d_transport_append_command(self.as_ptr(), ak::Span::new(record)) };
    }

    fn add_font(&self, font: &LentFont) -> FontResourceId {
        FontResourceId(unsafe { web_canvas_2d_transport_add_font(self.as_ptr(), font.0.as_ptr()) })
    }

    fn add_image_frame(&self, frame: &LentImageFrame) -> ImageFrameResourceId {
        ImageFrameResourceId(unsafe { web_canvas_2d_transport_add_image_frame(self.as_ptr(), frame.0.as_ptr()) })
    }

    fn retain_video_frame_hold(&self, hold: &LentVideoFrameHold) {
        unsafe { web_canvas_2d_transport_retain_video_frame_hold(self.as_ptr(), hold.0.as_ptr()) };
    }

    fn record_present(&self) {
        unsafe { web_canvas_2d_transport_record_present(self.as_ptr()) };
    }

    fn pending_command_count(&self) -> usize {
        unsafe { web_canvas_2d_transport_pending_command_count(self.as_ptr()) }
    }

    fn flush(&self) {
        unsafe { web_canvas_2d_transport_flush(self.as_ptr()) };
    }

    fn read_back_pixels(&self, rect: IntRect, alpha_type: AlphaType) -> Option<Bitmap> {
        let bitmap = unsafe {
            web_canvas_2d_transport_read_back_pixels(self.as_ptr(), rect.x, rect.y, rect.width, rect.height, alpha_type)
        };
        NonNull::new(bitmap).map(Bitmap)
    }
}
