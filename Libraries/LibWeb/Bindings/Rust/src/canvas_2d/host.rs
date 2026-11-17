/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transport of a 2D canvas in the product: the stream of its Web::Compositor::CompositorHost, and the
//! CompositorConnection of LibWeb, through the C ABI that Libraries/LibWeb/Compositor/Canvas2DStreamFFI.cpp gives it.

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::mem::MaybeUninit;
use core::ptr::NonNull;
use std::rc::Rc;

use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, ColorSpace, Transport};
use libcompositing_rust::display_list::commands::{CanvasId, FontResourceId, ImageFrameResourceId};
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    CreateCanvas2dContext, DestroyCanvas2dContext,
};
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{IntRect, IntSize};
use libipc_rust::cpp_connection::{FfiThreadSafeMessagePoster, ThreadSafeMessagePoster};
use libipc_rust::wire::EndpointMessage;

use super::stream::{Canvas2DStream, flush, shared_stream};

unsafe extern "C" {
    fn web_canvas_2d_connection_unref(connection: *mut c_void);
    fn web_canvas_2d_connection_poster(connection: *mut c_void, out_poster: *mut FfiThreadSafeMessagePoster) -> bool;
    fn web_canvas_2d_connection_reserve_context_id(
        connection: *mut c_void,
        width: i32,
        height: i32,
        out_canvas_id: *mut u64,
    ) -> bool;
    fn web_canvas_2d_connection_post_stream_batch_fonts_and_image_frames(
        connection: *mut c_void,
        fonts: *const *const c_void,
        font_count: usize,
        image_frames: *const *const c_void,
        image_frame_count: usize,
    ) -> bool;
    fn web_canvas_2d_connection_did_fail_to_post(connection: *mut c_void);
    fn web_canvas_2d_connection_read_back_pixels(
        connection: *mut c_void,
        canvas_id: u64,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        alpha_type: AlphaType,
        color_space: ColorSpace,
    ) -> *mut c_void;
    fn web_canvas_2d_video_frame_hold_ref(hold: *mut c_void);
    fn web_canvas_2d_video_frame_hold_unref(hold: *mut c_void);
    fn web_canvas_2d_video_frame_hold_note_sent_to_compositor(hold: *mut c_void);
}

#[derive(Clone, Copy)]
pub(crate) struct CompositorConnection(NonNull<c_void>);

// SAFETY (of every call below): The connection is live, and the fonts and image frames are lent for the call, in which
// C++ takes references of its own.
impl CompositorConnection {
    /// # Safety
    ///
    /// `connection` must be a CompositorConnection that outlives the value.
    pub(crate) unsafe fn lent(connection: NonNull<c_void>) -> Self {
        Self(connection)
    }

    pub(crate) fn poster_while_compositor_is_reachable(self) -> Option<ThreadSafeMessagePoster> {
        let mut poster = MaybeUninit::<FfiThreadSafeMessagePoster>::uninit();
        let reachable = unsafe { web_canvas_2d_connection_poster(self.0.as_ptr(), poster.as_mut_ptr()) };
        // SAFETY: C++ sets the poster to one reference to the poster of the connection where it answers true.
        reachable.then(|| unsafe { ThreadSafeMessagePoster::adopt(poster.assume_init()) })
    }

    pub(crate) fn post_or_lose_compositor<M: EndpointMessage>(
        self,
        poster: &ThreadSafeMessagePoster,
        message: M,
    ) -> bool {
        let posted = poster.try_post(message).expect("a message of a 2D canvas encodes");
        if !posted {
            unsafe { web_canvas_2d_connection_did_fail_to_post(self.0.as_ptr()) };
        }
        posted
    }

    fn reserve_context_id(self, size: IntSize) -> Option<CanvasId> {
        let mut canvas_id = 0;
        unsafe {
            web_canvas_2d_connection_reserve_context_id(self.0.as_ptr(), size.width, size.height, &raw mut canvas_id)
        }
        .then_some(CanvasId(canvas_id))
    }

    pub(crate) fn post_stream_batch_fonts_and_image_frames(
        self,
        fonts: &[&FontHandle],
        image_frames: &[&ImageFrameHandle],
    ) -> bool {
        let fonts: Vec<*const c_void> = fonts.iter().map(|font| font.as_raw()).collect();
        let image_frames: Vec<*const c_void> = image_frames.iter().map(|frame| frame.as_raw()).collect();
        unsafe {
            web_canvas_2d_connection_post_stream_batch_fonts_and_image_frames(
                self.0.as_ptr(),
                fonts.as_ptr(),
                fonts.len(),
                image_frames.as_ptr(),
                image_frames.len(),
            )
        }
    }

    fn read_back_pixels(
        self,
        canvas_id: CanvasId,
        rect: IntRect,
        alpha_type: AlphaType,
        color_space: ColorSpace,
    ) -> Option<ImageFrameHandle> {
        let frame = unsafe {
            web_canvas_2d_connection_read_back_pixels(
                self.0.as_ptr(),
                canvas_id.0,
                rect.x,
                rect.y,
                rect.width,
                rect.height,
                alpha_type,
                color_space,
            )
        };
        // SAFETY: LibWeb hands over a frame that it allocated with new, or null.
        (!frame.is_null()).then(|| unsafe { ImageFrameHandle::adopt(frame) })
    }
}

pub(crate) struct VideoFrameHold(NonNull<c_void>);

// SAFETY (of every call below): The value owns a reference to the hold.
impl VideoFrameHold {
    /// # Safety
    ///
    /// `hold` must be a VideoFrameHold, one reference to which the value takes over.
    pub(crate) unsafe fn adopt(hold: NonNull<c_void>) -> Self {
        Self(hold)
    }

    pub(crate) fn is_same_hold_as(&self, other: &Self) -> bool {
        self.0 == other.0
    }

    pub(crate) fn note_sent_to_compositor(&self) {
        unsafe { web_canvas_2d_video_frame_hold_note_sent_to_compositor(self.0.as_ptr()) };
    }
}

impl Clone for VideoFrameHold {
    fn clone(&self) -> Self {
        unsafe { web_canvas_2d_video_frame_hold_ref(self.0.as_ptr()) };
        Self(self.0)
    }
}

impl Drop for VideoFrameHold {
    fn drop(&mut self) {
        unsafe { web_canvas_2d_video_frame_hold_unref(self.0.as_ptr()) };
    }
}

#[repr(C)]
pub(crate) struct TransportAbi {
    pub(crate) stream: *mut c_void,
    pub(crate) connection: *mut c_void,
}

impl Default for TransportAbi {
    fn default() -> Self {
        Self {
            stream: core::ptr::null_mut(),
            connection: core::ptr::null_mut(),
        }
    }
}

pub(crate) struct CompositorTransport {
    stream: Rc<RefCell<Canvas2DStream>>,
    connection: CompositorConnection,
    canvas_id: Cell<Option<CanvasId>>,
}

impl CompositorTransport {
    /// # Safety
    ///
    /// The transport must be one that LibWeb has just handed over.
    pub(crate) unsafe fn adopt(transport: TransportAbi) -> Self {
        let stream = NonNull::new(transport.stream).expect("LibWeb passes the stream of the host");
        let connection = NonNull::new(transport.connection).expect("LibWeb passes the connection");
        Self {
            // SAFETY: The host keeps the stream while it hands it out.
            stream: unsafe { shared_stream(stream) },
            // SAFETY: The value owns a reference to the connection, which it gives up when dropped.
            connection: unsafe { CompositorConnection::lent(connection) },
            canvas_id: Cell::new(None),
        }
    }

    fn recording_canvas_id(&self) -> CanvasId {
        self.canvas_id
            .get()
            .expect("a canvas records once its context is created")
    }
}

impl Drop for CompositorTransport {
    fn drop(&mut self) {
        // SAFETY: The value owns a reference to the connection.
        unsafe { web_canvas_2d_connection_unref(self.connection.0.as_ptr()) };
    }
}

impl Transport for CompositorTransport {
    type Font = FontHandle;
    type ImageFrame = ImageFrameHandle;
    type VideoFrameHold = VideoFrameHold;

    fn create_context(&self, size: IntSize, alpha: bool, will_read_frequently: bool, color_space: ColorSpace) -> bool {
        assert!(self.canvas_id.get().is_none(), "a transport creates one context");
        let Some(canvas_id) = self.connection.reserve_context_id(size) else {
            return false;
        };
        let Some(poster) = self.connection.poster_while_compositor_is_reachable() else {
            return false;
        };
        let message = CreateCanvas2dContext {
            canvas_id,
            size,
            alpha,
            will_read_frequently,
            color_space: color_space.into(),
        };
        if !self.connection.post_or_lose_compositor(&poster, message) {
            return false;
        }
        self.canvas_id.set(Some(canvas_id));
        true
    }

    fn canvas_id(&self) -> Option<CanvasId> {
        self.canvas_id.get()
    }

    fn destroy_context(&self) {
        let Some(canvas_id) = self.canvas_id.take() else {
            return;
        };
        if let Some(poster) = self.connection.poster_while_compositor_is_reachable() {
            self.connection
                .post_or_lose_compositor(&poster, DestroyCanvas2dContext { canvas_id });
        }
    }

    fn append_command(&self, record: &[u8]) {
        let canvas_id = self.recording_canvas_id();
        self.stream.borrow_mut().append_command(canvas_id, record);
    }

    fn add_font(&self, font: &FontHandle) -> FontResourceId {
        self.stream.borrow_mut().add_font(font)
    }

    fn add_image_frame(&self, frame: &ImageFrameHandle) -> ImageFrameResourceId {
        self.stream.borrow_mut().add_image_frame(frame)
    }

    fn retain_video_frame_hold(&self, hold: &VideoFrameHold) {
        self.stream.borrow_mut().retain_video_frame_hold(hold);
    }

    fn record_present(&self) {
        let canvas_id = self.recording_canvas_id();
        self.stream.borrow_mut().record_present(canvas_id);
    }

    fn pending_command_count(&self) -> usize {
        self.stream.borrow().pending_command_count()
    }

    fn flush(&self) {
        flush(&self.stream, self.connection);
    }

    fn read_back_pixels(
        &self,
        rect: IntRect,
        alpha_type: AlphaType,
        color_space: ColorSpace,
    ) -> Option<ImageFrameHandle> {
        let canvas_id = self.canvas_id.get()?;
        self.connection
            .read_back_pixels(canvas_id, rect, alpha_type, color_space)
    }
}
