/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transport of a context in the product: a RemoteWebGLTransport of LibWeb, through the C ABI that
//! Libraries/LibWeb/WebGL/RemoteWebGLTransportFFI.cpp gives it.

use core::ffi::c_void;
use core::ptr::{self, NonNull};

use super::proxy::transport::{ReadPixelsResult, SharedMemory, Transport};

unsafe extern "C" {
    fn web_webgl_transport_unref(transport: *mut c_void);
    fn web_webgl_transport_canvas_id(transport: *const c_void, out_canvas_id: *mut u64) -> bool;
    fn web_webgl_transport_destroy_context(transport: *mut c_void);
    fn web_webgl_transport_set_shared_command_buffer(transport: *mut c_void, anonymous_buffer: *const c_void);
    fn web_webgl_transport_send_commands_from_shared_buffer(
        transport: *mut c_void,
        offset: u64,
        size_in_bytes: u64,
        flush_sequence_number: u64,
        frames: *const Frame,
        frame_count: usize,
    );
    fn web_webgl_transport_wait_until_published_commands_executed(transport: *mut c_void) -> bool;
    fn web_webgl_transport_send_commands(
        transport: *mut c_void,
        commands: *const u8,
        size_in_bytes: usize,
        frames: *const Frame,
        frame_count: usize,
    );
    fn web_webgl_transport_present_canvas(transport: *mut c_void, preserve_drawing_buffer: bool);
    fn web_webgl_transport_clear_drawing_buffer(transport: *mut c_void);
    fn web_webgl_transport_sync_call(
        transport: *mut c_void,
        request: *const u8,
        request_size: usize,
        reply_sink: *mut c_void,
        write_reply: unsafe extern "C" fn(reply_sink: *mut c_void, reply: *const u8, reply_size: usize),
    );
    fn web_webgl_transport_read_pixels_robust_angle(
        transport: *mut c_void,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        buf_size: i32,
        pixels_anonymous_buffer_or_null: *const c_void,
        out_result: *mut ReadPixelsResult,
    );
    fn web_webgl_transport_read_buffer_sub_data(
        transport: *mut c_void,
        target: u32,
        offset: i64,
        size: i64,
        data_anonymous_buffer: *const c_void,
    ) -> bool;
    fn web_webgl_transport_read_back_drawing_buffer(
        transport: *mut c_void,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    ) -> *mut c_void;
    fn web_webgl_max_frames_per_message() -> usize;
    fn web_webgl_anonymous_buffer_create(size: usize, out_data: *mut *mut u8) -> *mut c_void;
    fn web_webgl_anonymous_buffer_destroy(anonymous_buffer: *mut c_void);
    fn web_webgl_frame_destroy(frame: *mut c_void);
    fn web_webgl_bitmap_unref(bitmap: *mut c_void);
}

pub(crate) struct AnonymousBuffer {
    buffer: NonNull<c_void>,
    data: *mut u8,
    size: usize,
}

impl AnonymousBuffer {
    fn create(size: usize) -> Option<Self> {
        let mut data = ptr::null_mut();
        // SAFETY: The function has no preconditions, and stores where the memory is mapped when it returns a buffer.
        let buffer = NonNull::new(unsafe { web_webgl_anonymous_buffer_create(size, &raw mut data) })?;
        Some(Self { buffer, data, size })
    }
}

impl Drop for AnonymousBuffer {
    fn drop(&mut self) {
        // SAFETY: The value owns the buffer, which nothing uses after this.
        unsafe { web_webgl_anonymous_buffer_destroy(self.buffer.as_ptr()) };
    }
}

impl SharedMemory for AnonymousBuffer {
    fn as_ptr(&self) -> *mut u8 {
        self.data
    }

    fn len(&self) -> usize {
        self.size
    }
}

#[repr(transparent)]
pub(crate) struct Frame(NonNull<c_void>);

impl Frame {
    /// # Safety
    ///
    /// `frame` is a Gfx::DecodedImageFrame allocated with `new`, which the value takes over.
    pub(crate) unsafe fn from_raw(frame: NonNull<c_void>) -> Self {
        Self(frame)
    }
}

impl Drop for Frame {
    fn drop(&mut self) {
        // SAFETY: The value owns the frame, which nothing uses after this.
        unsafe { web_webgl_frame_destroy(self.0.as_ptr()) };
    }
}

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
        unsafe { web_webgl_bitmap_unref(self.0.as_ptr()) };
    }
}

pub(crate) struct CppTransport(NonNull<c_void>);

impl CppTransport {
    /// # Safety
    ///
    /// `transport` is a RemoteWebGLTransport, one reference to which the value takes over.
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
        unsafe { web_webgl_transport_unref(self.as_ptr()) };
    }
}

unsafe extern "C" fn append_reply(reply_sink: *mut c_void, reply: *const u8, reply_size: usize) {
    if reply_size == 0 {
        return;
    }
    // SAFETY: sync_call() passes its vector as the sink, and the transport passes the bytes of its reply.
    unsafe {
        let reply_sink = &mut *reply_sink.cast::<Vec<u8>>();
        reply_sink.extend_from_slice(core::slice::from_raw_parts(reply, reply_size));
    }
}

// SAFETY (of every call below): The value holds a reference to the transport, the buffers and the frames passed are
// alive for the call, and a pointer with a length is the pointer and the length of one slice.
impl Transport for CppTransport {
    type SharedMemory = AnonymousBuffer;
    type Frame = Frame;
    type Bitmap = Bitmap;

    fn max_frames_per_message(&self) -> usize {
        unsafe { web_webgl_max_frames_per_message() }
    }

    fn create_shared_memory(&self, size: usize) -> Option<AnonymousBuffer> {
        AnonymousBuffer::create(size)
    }

    fn canvas_id(&self) -> Option<u64> {
        let mut canvas_id = 0;
        unsafe { web_webgl_transport_canvas_id(self.as_ptr(), &raw mut canvas_id) }.then_some(canvas_id)
    }

    fn destroy_context(&self) {
        unsafe { web_webgl_transport_destroy_context(self.as_ptr()) };
    }

    fn set_shared_command_buffer(&self, buffer: &AnonymousBuffer) {
        unsafe { web_webgl_transport_set_shared_command_buffer(self.as_ptr(), buffer.buffer.as_ptr()) };
    }

    fn send_commands_from_shared_buffer(
        &self,
        offset: u64,
        size_in_bytes: u64,
        flush_sequence_number: u64,
        frames: &[Frame],
    ) {
        unsafe {
            web_webgl_transport_send_commands_from_shared_buffer(
                self.as_ptr(),
                offset,
                size_in_bytes,
                flush_sequence_number,
                frames.as_ptr(),
                frames.len(),
            );
        }
    }

    fn wait_until_published_commands_executed(&self) -> bool {
        unsafe { web_webgl_transport_wait_until_published_commands_executed(self.as_ptr()) }
    }

    fn send_commands(&self, commands: &[u8], frames: &[Frame]) {
        unsafe {
            web_webgl_transport_send_commands(
                self.as_ptr(),
                commands.as_ptr(),
                commands.len(),
                frames.as_ptr(),
                frames.len(),
            );
        }
    }

    fn present_canvas(&self, preserve_drawing_buffer: bool) {
        unsafe { web_webgl_transport_present_canvas(self.as_ptr(), preserve_drawing_buffer) };
    }

    fn clear_drawing_buffer(&self) {
        unsafe { web_webgl_transport_clear_drawing_buffer(self.as_ptr()) };
    }

    fn sync_call(&self, request: &[u8], reply: &mut Vec<u8>) {
        unsafe {
            web_webgl_transport_sync_call(
                self.as_ptr(),
                request.as_ptr(),
                request.len(),
                ptr::from_mut(reply).cast::<c_void>(),
                append_reply,
            );
        }
    }

    fn read_pixels_robust_angle(
        &self,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        buf_size: i32,
        pixels: Option<&AnonymousBuffer>,
    ) -> ReadPixelsResult {
        let mut result = ReadPixelsResult::default();
        let pixels = pixels.map_or(ptr::null(), |pixels| pixels.buffer.as_ptr().cast_const());
        unsafe {
            web_webgl_transport_read_pixels_robust_angle(
                self.as_ptr(),
                x,
                y,
                width,
                height,
                format,
                r#type,
                buf_size,
                pixels,
                &raw mut result,
            );
        }
        result
    }

    fn read_buffer_sub_data(&self, target: u32, offset: i64, size: i64, data: &AnonymousBuffer) -> bool {
        unsafe { web_webgl_transport_read_buffer_sub_data(self.as_ptr(), target, offset, size, data.buffer.as_ptr()) }
    }

    fn read_back_drawing_buffer(&self, x: i32, y: i32, width: i32, height: i32) -> Option<Bitmap> {
        let bitmap = unsafe { web_webgl_transport_read_back_drawing_buffer(self.as_ptr(), x, y, width, height) };
        NonNull::new(bitmap).map(Bitmap)
    }
}
