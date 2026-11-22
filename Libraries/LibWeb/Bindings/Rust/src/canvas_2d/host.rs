/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transport of a 2D canvas in the product: the stream of its Web::Compositor::CompositorHost, and the
//! CompositorConnection of LibWeb, through the C ABI that Libraries/LibWeb/Compositor/Canvas2DStreamFFI.cpp gives it.

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;
use std::ops::Range;
use std::rc::Rc;

use libcompositing_rust::display_list::canvas_2d_recorder::{
    AlphaType, ColorSpace, Transport, is_valid_canvas_2d_size,
};
use libcompositing_rust::display_list::commands::{CanvasId, FontResourceId, ImageFrameResourceId};
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    CreateCanvas2dContext, DestroyCanvas2dContext, ReadBackCanvas2dPixels, ReserveCanvasIds,
};
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{IntRect, IntSize};

use crate::compositor_connection::{CompositorConnection, SharedBuffer};

use super::stream::{Canvas2DStream, flush, shared_stream};

unsafe extern "C" {
    fn web_canvas_2d_connection_post_stream_batch_fonts_and_image_frames(
        connection: *mut c_void,
        fonts: *const *const c_void,
        font_count: usize,
        image_frames: *const *const c_void,
        image_frame_count: usize,
    ) -> bool;
    fn web_canvas_2d_video_frame_hold_ref(hold: *mut c_void);
    fn web_canvas_2d_video_frame_hold_unref(hold: *mut c_void);
    fn web_canvas_2d_video_frame_hold_note_sent_to_compositor(hold: *mut c_void);
}

struct ReservedCanvasIds {
    connection_serial: u64,
    ids: Range<u64>,
}

thread_local! {
    static RESERVED_CANVAS_IDS: RefCell<Option<ReservedCanvasIds>> = const { RefCell::new(None) };
}

// SAFETY (of every call below): The connection is live, and the fonts and image frames are lent for the call, in which
// C++ takes references of its own.
impl CompositorConnection {
    fn reserve_context_id(self, size: IntSize) -> Option<CanvasId> {
        // NB: The Compositor refuses the same sizes, so a context it is asked to create always exists afterwards.
        if !is_valid_canvas_2d_size(size) {
            return None;
        }
        let connection_serial = self.serial();
        let reserved_id = RESERVED_CANVAS_IDS.with_borrow_mut(|reserved| {
            reserved
                .as_mut()
                .filter(|reserved| reserved.connection_serial == connection_serial)
                .and_then(|reserved| reserved.ids.next())
        });
        if let Some(canvas_id) = reserved_id {
            return Some(CanvasId(canvas_id));
        }
        // OPTIMIZATION: Creating a context is asynchronous, under an ID from a range reserved up front. A synchronous
        //               reply would wait for the Compositor to replay every canvas command queued before it.
        let response = self.send_sync(ReserveCanvasIds)?;
        let first_canvas_id = response.first_canvas_id.0;
        let mut ids = first_canvas_id..first_canvas_id.checked_add(response.count)?;
        let canvas_id = ids.next();
        RESERVED_CANVAS_IDS.set(Some(ReservedCanvasIds { connection_serial, ids }));
        canvas_id.map(CanvasId)
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
                self.as_ptr(),
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
        let size = usize::try_from(rect.width)
            .ok()?
            .checked_mul(usize::try_from(rect.height).ok()?)?
            .checked_mul(4)?;
        let pixels = SharedBuffer::create(size)?;
        let response = self.send_sync(ReadBackCanvas2dPixels {
            canvas_id,
            rect,
            alpha_type,
            color_space: color_space.into(),
            pixels: pixels.for_message(),
        })?;
        let IntSize { width, height } = response.size;
        if width <= 0 || height <= 0 {
            return None;
        }
        pixels.into_image_frame(width, height, alpha_type, color_space)
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
    connection_serial: u64,
    canvas_id: Cell<Option<CanvasId>>,
}

impl CompositorTransport {
    /// # Safety
    ///
    /// The transport must be one that LibWeb has just handed over.
    pub(crate) unsafe fn adopt(transport: TransportAbi) -> Self {
        let stream = NonNull::new(transport.stream).expect("LibWeb passes the stream of the host");
        let connection = NonNull::new(transport.connection).expect("LibWeb passes the connection");
        // SAFETY: The value owns a reference to the connection, which it gives up when dropped.
        let connection = unsafe { CompositorConnection::lent(connection) };
        Self {
            // SAFETY: The host keeps the stream while it hands it out.
            stream: unsafe { shared_stream(stream) },
            connection,
            connection_serial: connection.serial(),
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
        unsafe { self.connection.release() };
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
        self.stream
            .borrow_mut()
            .append_command(self.connection_serial, canvas_id, record);
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
        self.stream
            .borrow_mut()
            .record_present(self.connection_serial, canvas_id);
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
