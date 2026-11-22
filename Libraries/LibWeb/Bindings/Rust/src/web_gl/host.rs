/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The transport of a context in the product: the CompositorConnection of the page of its canvas, which its messages go
//! through, and the shared memory that they carry.

use core::cell::Cell;

use libcompositing_rust::display_list::canvas_2d_recorder::{AlphaType, ColorSpace};
use libcompositing_rust::display_list::commands::{CanvasId, ImageFrameResourceId, VideoSinkHandle};
use libcompositing_rust::ipc_types::Compositing_WebGL_WebGLVersion;
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::{
    CreateWebglContext, DestroyWebglContext, WebglClearDrawingBuffer, WebglCommands, WebglCommandsFromSharedBuffer,
    WebglDrainCommandBuffer, WebglPresentCanvas, WebglReadBackDrawingBuffer, WebglReadBufferSubData, WebglReadPixels,
    WebglSetCommandBuffer, WebglSyncCall,
};
use libcompositing_rust::webgl::ImageSource;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{IntRect, IntSize};

use super::proxy::transport::{ReadPixelsResult, SharedMemory, Transport};
use super::state::MAX_WEBGL_DRAWING_BUFFER_DIMENSION;
use crate::canvas_2d::canvas::CapturedVideoFrame;
use crate::compositor_connection::{CompositorConnection, SharedBuffer};

fn rect_inside_largest_drawing_buffer(x: i32, y: i32, width: i32, height: i32) -> Option<IntRect> {
    let clamp = |coordinate: i32| coordinate.clamp(0, MAX_WEBGL_DRAWING_BUFFER_DIMENSION);
    let (left, top) = (clamp(x), clamp(y));
    let (right, bottom) = (clamp(x.saturating_add(width)), clamp(y.saturating_add(height)));
    (left < right && top < bottom).then_some(IntRect {
        x: left,
        y: top,
        width: right - left,
        height: bottom - top,
    })
}

impl SharedMemory for SharedBuffer {
    fn as_ptr(&self) -> *mut u8 {
        SharedBuffer::as_ptr(self)
    }

    fn len(&self) -> usize {
        SharedBuffer::len(self)
    }
}

pub(crate) enum Frame {
    Image(ImageFrameHandle),
    Video(CapturedVideoFrame),
}

pub(crate) struct ContextCreation {
    pub(crate) webgl2: bool,
    pub(crate) size: IntSize,
    pub(crate) depth: bool,
    pub(crate) stencil: bool,
    pub(crate) antialias: bool,
}

pub(crate) struct RemoteContext {
    connection: CompositorConnection,
    canvas_id: Cell<Option<CanvasId>>,
}

impl RemoteContext {
    /// # Safety
    ///
    /// `connection` holds a reference to the connection, which the call takes over.
    pub(crate) unsafe fn create(
        connection: CompositorConnection,
        creation: &ContextCreation,
    ) -> Option<(Self, Vec<Vec<u8>>)> {
        let context = Self {
            connection,
            canvas_id: Cell::new(None),
        };
        let response = connection.send_sync(CreateWebglContext {
            webgl_version: if creation.webgl2 {
                Compositing_WebGL_WebGLVersion::WebGL2
            } else {
                Compositing_WebGL_WebGLVersion::WebGL1
            },
            size: creation.size,
            depth: creation.depth,
            stencil: creation.stencil,
            antialias: creation.antialias,
        })?;
        if !response.success {
            return None;
        }
        context.canvas_id.set(Some(response.canvas_id));
        let supported_extensions = response
            .supported_extensions
            .into_iter()
            .map(|extension| extension.into_bytes())
            .collect();
        Some((context, supported_extensions))
    }

    fn image_sources_of(&self, frames: &[Frame]) -> Option<Vec<ImageSource>> {
        let image_frames: Vec<&ImageFrameHandle> = frames
            .iter()
            .filter_map(|frame| match frame {
                Frame::Image(image_frame) => Some(image_frame),
                Frame::Video(_) => None,
            })
            .collect();
        if !image_frames.is_empty() && !self.connection.register_image_frames(&image_frames) {
            self.connection.lose_compositor();
            return None;
        }
        Some(
            frames
                .iter()
                .map(|frame| match frame {
                    Frame::Image(image_frame) => {
                        ImageSource::RegisteredImageFrame(ImageFrameResourceId(image_frame.id()))
                    }
                    Frame::Video(video_frame) => ImageSource::HeldVideoFrame {
                        sink_handle: VideoSinkHandle(video_frame.sink_handle),
                        generation: video_frame.generation,
                    },
                })
                .collect(),
        )
    }

    fn post_commands(&self, frames: &[Frame], message: impl FnOnce(CanvasId, Vec<ImageSource>) -> bool) {
        let Some(canvas_id) = self.canvas_id.get() else {
            return;
        };
        if self.connection.poster_while_compositor_is_reachable().is_none() {
            return;
        }
        let Some(image_sources) = self.image_sources_of(frames) else {
            return;
        };
        if !message(canvas_id, image_sources) {
            return;
        }
        for frame in frames {
            if let Frame::Video(video_frame) = frame {
                video_frame.hold.note_sent_to_compositor();
            }
        }
    }

    fn post_or_lose_compositor<M: libipc_rust::wire::EndpointMessage>(&self, message: M) -> bool {
        match self.connection.poster_while_compositor_is_reachable() {
            Some(poster) => self.connection.post_or_lose_compositor(&poster, message),
            None => false,
        }
    }
}

impl Drop for RemoteContext {
    fn drop(&mut self) {
        // SAFETY: The value owns a reference to the connection.
        unsafe { self.connection.release() };
    }
}

impl Transport for RemoteContext {
    type SharedMemory = SharedBuffer;
    type Frame = Frame;
    type DrawingBufferPixels = ImageFrameHandle;

    fn create_shared_memory(&self, size: usize) -> Option<SharedBuffer> {
        SharedBuffer::create(size)
    }

    fn canvas_id(&self) -> Option<u64> {
        self.canvas_id.get().map(|canvas_id| canvas_id.0)
    }

    fn destroy_context(&self) {
        if let Some(canvas_id) = self.canvas_id.take() {
            self.connection.post(DestroyWebglContext { canvas_id });
        }
    }

    fn set_shared_command_buffer(&self, buffer: &SharedBuffer) {
        if let Some(canvas_id) = self.canvas_id.get() {
            self.post_or_lose_compositor(WebglSetCommandBuffer {
                canvas_id,
                command_buffer: buffer.for_message(),
            });
        }
    }

    fn send_commands_from_shared_buffer(
        &self,
        offset: u64,
        size_in_bytes: u64,
        flush_sequence_number: u64,
        frames: &[Frame],
    ) {
        self.post_commands(frames, |canvas_id, image_sources| {
            self.post_or_lose_compositor(WebglCommandsFromSharedBuffer {
                canvas_id,
                offset,
                size_in_bytes,
                flush_sequence_number,
                image_sources,
            })
        });
    }

    fn wait_until_published_commands_executed(&self) -> bool {
        let Some(canvas_id) = self.canvas_id.get() else {
            return false;
        };
        self.connection
            .send_sync(WebglDrainCommandBuffer { canvas_id })
            .is_some()
    }

    fn send_commands(&self, commands: &[u8], frames: &[Frame]) {
        let Some(buffer) = SharedBuffer::create(commands.len()) else {
            return;
        };
        // SAFETY: The buffer has room for the commands, and nothing else writes it yet.
        unsafe { core::ptr::copy_nonoverlapping(commands.as_ptr(), buffer.as_ptr(), commands.len()) };
        self.send_commands_written_to(&buffer, commands.len(), frames);
    }

    fn send_commands_written_to(&self, memory: &SharedBuffer, size_in_bytes: usize, frames: &[Frame]) {
        self.post_commands(frames, |canvas_id, image_sources| {
            self.post_or_lose_compositor(WebglCommands {
                canvas_id,
                commands: memory.for_message(),
                size_in_bytes: size_in_bytes as u64,
                image_sources,
            })
        });
    }

    fn present_canvas(&self, preserve_drawing_buffer: bool) {
        if let Some(canvas_id) = self.canvas_id.get() {
            self.connection.post(WebglPresentCanvas {
                canvas_id,
                preserve_drawing_buffer,
            });
        }
    }

    fn clear_drawing_buffer(&self) {
        if let Some(canvas_id) = self.canvas_id.get() {
            self.connection.post(WebglClearDrawingBuffer { canvas_id });
        }
    }

    fn sync_call(&self, request: &[u8], reply: &mut Vec<u8>) {
        let Some(canvas_id) = self.canvas_id.get() else {
            return;
        };
        if let Some(response) = self.connection.send_sync(WebglSyncCall {
            canvas_id,
            request: request.to_vec(),
        }) {
            reply.extend_from_slice(&response.reply);
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
        pixels: Option<&SharedBuffer>,
    ) -> ReadPixelsResult {
        let Some(canvas_id) = self.canvas_id.get() else {
            return ReadPixelsResult::default();
        };
        let pixels = pixels.map_or(
            libcompositing_rust::ipc_types::Core_AnonymousBuffer { shared_memory: None },
            SharedBuffer::for_message,
        );
        self.connection
            .send_sync(WebglReadPixels {
                canvas_id,
                x,
                y,
                width,
                height,
                format,
                r#type,
                buf_size,
                pixels,
            })
            .map_or_else(ReadPixelsResult::default, |response| ReadPixelsResult {
                length: response.length,
                columns: response.columns,
                rows: response.rows,
            })
    }

    fn read_buffer_sub_data(&self, target: u32, offset: i64, size: i64, data: &SharedBuffer) -> bool {
        let Some(canvas_id) = self.canvas_id.get() else {
            return false;
        };
        self.connection
            .send_sync(WebglReadBufferSubData {
                canvas_id,
                target,
                offset,
                size,
                data: data.for_message(),
            })
            .is_some_and(|response| response.success)
    }

    fn read_back_drawing_buffer(&self, x: i32, y: i32, width: i32, height: i32) -> Option<ImageFrameHandle> {
        let canvas_id = self.canvas_id.get()?;
        let rect = rect_inside_largest_drawing_buffer(x, y, width, height)?;
        let size = usize::try_from(rect.width)
            .ok()?
            .checked_mul(usize::try_from(rect.height).ok()?)?
            .checked_mul(4)?;
        let pixels = SharedBuffer::create(size)?;
        let response = self.connection.send_sync(WebglReadBackDrawingBuffer {
            canvas_id,
            rect,
            pixels: pixels.for_message(),
        })?;
        let IntSize { width, height } = response.size;
        if width <= 0 || height <= 0 {
            return None;
        }
        pixels.into_image_frame(width, height, AlphaType::Premultiplied, ColorSpace::Srgb)
    }
}
