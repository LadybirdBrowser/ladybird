/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the replay of WebGL asks of the C++ side: the GL contexts with their drawing buffers, which C++ makes current,
//! presents and reads back, and the images that it converts or copies for texture uploads.

use core::ffi::{CStr, c_void};

use libcompositing_rust::display_list::commands::{CanvasId, VideoSinkHandle};
use libcompositing_rust::webgl::ImageSource;
use libcompositing_rust::webgl::texture_upload::ExportFormat;
use libgfx_rust::{IntRect, IntSize};

use crate::host::WebContentClientId;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct DefaultBuffers {
    pub framebuffer: u32,
    pub renderbuffer: u32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct ContextOptions {
    pub webgl2: bool,
    pub depth: bool,
    pub stencil: bool,
    pub antialias: bool,
    pub size: IntSize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum PresentOutcome {
    Presented,
    /// Nothing ran since the frame presented last, which the canvas went on showing.
    ShowedPresentedFrame,
    NoDrawingBuffer,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ImageConversion {
    pub export_format: ExportFormat,
    pub flip_y: bool,
    pub premultiply_alpha: bool,
    pub target_size: Option<IntSize>,
}

pub enum ImageConversionOutcome<P> {
    Converted { pixels: P, width: i32, height: i32 },
    UnknownImageFrame,
    Failed,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct VideoFrameCopy {
    /// The part of the frame to copy, from its top left.
    pub source_size: IntSize,
    pub size: IntSize,
    pub target: u32,
    pub level: i32,
    pub xoffset: i32,
    pub yoffset: i32,
    pub flip_y: bool,
    pub premultiply_alpha: bool,
}

/// Shared memory that WebContent writes into while the Compositor reads it, so no Rust reference may cover it.
pub trait MappedMemory {
    fn as_mut_ptr(&self) -> *mut u8;
    fn len(&self) -> usize;

    fn is_empty(&self) -> bool {
        self.len() == 0
    }
}

impl MappedMemory for crate::raster_host::SharedMemory {
    fn as_mut_ptr(&self) -> *mut u8 {
        self.as_ptr()
    }

    fn len(&self) -> usize {
        self.len()
    }
}

pub trait WebGLHost {
    type Pixels: AsRef<[u8]>;
    type VideoFrame;
    type VideoFrameImage;

    /// The address of a GL function, or null.
    fn gl_proc_address(&self, name: &CStr) -> *const c_void;
    fn allocate_canvas_id(&self) -> CanvasId;
    /// Answers whether a GL context with a drawing buffer of the size could be made.
    fn create_webgl_context(&self, client: WebContentClientId, canvas_id: CanvasId, options: ContextOptions) -> bool;
    fn destroy_webgl_context(&self, client: WebContentClientId, canvas_id: CanvasId);
    /// Makes the GL context current, allocating its drawing buffer if it has to.
    fn make_webgl_context_current(&self, client: WebContentClientId, canvas_id: CanvasId) -> DefaultBuffers;
    fn set_webgl_drawing_buffer_size(&self, client: WebContentClientId, canvas_id: CanvasId, size: IntSize);
    /// Makes what was drawn the content of the canvas, or the frame presented last where `show_presented_frame` asks for
    /// it and there is one.
    fn present_webgl_context(
        &self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        preserve_drawing_buffer: bool,
        show_presented_frame: bool,
    ) -> PresentOutcome;
    /// Writes the part of the drawing buffer, or of the frame presented last, that the rect covers to `pixels` as
    /// premultiplied BGRA8888 rows, and answers the size of that part.
    ///
    /// # Safety
    ///
    /// `pixels` is valid for writes of `pixels_size` bytes.
    unsafe fn read_back_webgl_drawing_buffer(
        &self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        from_presented_frame: bool,
        rect: IntRect,
        pixels: *mut u8,
        pixels_size: usize,
    ) -> Option<IntSize>;
    fn webgl_can_copy_images_into_textures(&self, client: WebContentClientId, canvas_id: CanvasId) -> bool;
    /// The frame of the video that the client holds, with its size.
    fn held_video_frame(
        &self,
        client: WebContentClientId,
        sink_handle: VideoSinkHandle,
        generation: u64,
    ) -> Option<(Self::VideoFrame, IntSize)>;
    fn video_frame_image(&self, frame: &Self::VideoFrame) -> Option<Self::VideoFrameImage>;
    fn copy_video_frame_image_into_bound_texture(
        &self,
        client: WebContentClientId,
        canvas_id: CanvasId,
        image: &Self::VideoFrameImage,
        copy: VideoFrameCopy,
    ) -> bool;
    fn convert_image_source(
        &self,
        client: WebContentClientId,
        source: ImageSource,
        conversion: ImageConversion,
    ) -> ImageConversionOutcome<Self::Pixels>;
}
