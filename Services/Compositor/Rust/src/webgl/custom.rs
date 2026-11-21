/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The commands and the synchronous calls whose structs GLFunctions.json spells out, which the replayer handles by
//! hand: those of the strings that GL takes as arrays, of the drawing buffer, and of the images that a texture upload
//! takes its pixels from.

use core::ffi::{CStr, c_char, c_void};

use libcompositing_rust::webgl::ImageSource;
use libcompositing_rust::webgl::texture_upload::texture_export_format;
use libcompositing_rust::webgl::vocabulary::{commands, sync_calls};
use libcompositing_rust::webgl::wire::{encode_reply, reply_data_spans};
use libgfx_rust::IntSize;

use super::gl::GlFunctions;
use super::gl_constants::*;
use super::host::{DefaultBuffers, ImageConversion, ImageConversionOutcome, VideoFrameCopy, WebGLHost};
use super::replay::{ReplayError, Replayer, bytes_of_slice, data_span, decode, split_packed_strings, string_data};

const MAX_DRAWING_BUFFER_DIMENSION: i32 = 16384;

fn copy_can_write_internal_format(internalformat: i32) -> bool {
    u32::try_from(internalformat).is_ok_and(|internalformat| {
        matches!(
            internalformat,
            GL_ALPHA
                | GL_LUMINANCE
                | GL_LUMINANCE_ALPHA
                | GL_RGB
                | GL_RGBA
                | GL_SRGB_EXT
                | GL_SRGB_ALPHA_EXT
                | GL_R8
                | GL_RG8
                | GL_RGB8
                | GL_RGBA8
                | GL_SRGB8
                | GL_SRGB8_ALPHA8
                | GL_RGB565
                | GL_RGBA4
                | GL_RGB5_A1
                | GL_RGB10_A2
                | GL_RGB9_E5
                | GL_R11F_G11F_B10F
                | GL_R16F
                | GL_RG16F
                | GL_RGB16F
                | GL_RGBA16F
                | GL_R32F
                | GL_RG32F
                | GL_RGB32F
                | GL_RGBA32F
        )
    })
}

fn format_is_integer(format: u32) -> bool {
    matches!(
        format,
        GL_RED_INTEGER | GL_RG_INTEGER | GL_RGB_INTEGER | GL_RGBA_INTEGER
    )
}

fn upload_size(
    frame_size: IntSize,
    has_explicit_destination_size: bool,
    destination_width: i32,
    destination_height: i32,
) -> Option<IntSize> {
    if !has_explicit_destination_size {
        return Some(frame_size);
    }
    (destination_width >= 0 && destination_height >= 0).then_some(IntSize {
        width: destination_width,
        height: destination_height,
    })
}

/// The extensions that GL has enabled, or that ANGLE can enable on request.
pub fn supported_extensions(gl: &GlFunctions) -> Vec<String> {
    let mut extensions = Vec::new();
    for list in [GL_EXTENSIONS, GL_REQUESTABLE_EXTENSIONS_ANGLE] {
        // SAFETY: A context is current, and GL answers a string or null.
        let names = unsafe { (gl.get_string)(list) };
        if names.is_null() {
            continue;
        }
        // SAFETY: GL answers a NUL-terminated string.
        let names = unsafe { CStr::from_ptr(names.cast()) }.to_string_lossy();
        extensions.extend(names.split(' ').filter(|name| !name.is_empty()).map(String::from));
    }
    extensions
}

fn extension_is_enabled(gl: &GlFunctions, extension: &[u8]) -> bool {
    // SAFETY: A context is current, and GL answers a string or null.
    let names = unsafe { (gl.get_string)(GL_EXTENSIONS) };
    // SAFETY: GL answers a NUL-terminated string.
    !names.is_null()
        && unsafe { CStr::from_ptr(names.cast()) }
            .to_bytes()
            .split(|&byte| byte == b' ')
            .any(|name| name == extension)
}

/// Clears the drawing buffer to the values that WebGL clears it to between frames, and restores the clear values and the
/// bindings of the page.
pub fn clear_buffer_to_default_values(gl: &GlFunctions, webgl2: bool, defaults: DefaultBuffers) {
    let (framebuffer_target, framebuffer_binding) = if webgl2 {
        (GL_DRAW_FRAMEBUFFER, GL_DRAW_FRAMEBUFFER_BINDING)
    } else {
        (GL_FRAMEBUFFER, GL_FRAMEBUFFER_BINDING)
    };
    let mut original_framebuffer = 0;
    let mut original_renderbuffer = 0;
    let mut clear_color = [0.0_f32; 4];
    let mut clear_depth = 0.0_f32;
    let mut clear_stencil = 0;
    // SAFETY: A context is current, and every pointer is room for what GL writes.
    unsafe {
        (gl.get_integerv)(framebuffer_binding, &raw mut original_framebuffer);
        (gl.get_integerv)(GL_RENDERBUFFER_BINDING, &raw mut original_renderbuffer);
        (gl.bind_framebuffer)(framebuffer_target, defaults.framebuffer);
        (gl.bind_renderbuffer)(GL_RENDERBUFFER, defaults.renderbuffer);
        (gl.get_floatv)(GL_COLOR_CLEAR_VALUE, clear_color.as_mut_ptr());
        (gl.get_floatv)(GL_DEPTH_CLEAR_VALUE, &raw mut clear_depth);
        (gl.get_integerv)(GL_STENCIL_CLEAR_VALUE, &raw mut clear_stencil);
        (gl.clear_color)(0.0, 0.0, 0.0, 0.0);
        (gl.clear_depthf)(1.0);
        (gl.clear_stencil)(0);
        (gl.clear)(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        (gl.clear_color)(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
        (gl.clear_depthf)(clear_depth);
        (gl.clear_stencil)(clear_stencil);
        (gl.bind_framebuffer)(framebuffer_target, original_framebuffer as u32);
        (gl.bind_renderbuffer)(GL_RENDERBUFFER, original_renderbuffer as u32);
    }
}

/// The pixels of an image as a texture upload takes them, or the negative size that GL is to reject.
struct ConvertedImage<P> {
    pixels: Option<P>,
    width: i32,
    height: i32,
}

/// What an upload from an image asks for, whatever kind of upload it is.
struct ImageUpload {
    bitmap_index: u32,
    format: u32,
    r#type: u32,
    has_explicit_destination_size: bool,
    destination_width: i32,
    destination_height: i32,
    flip_y: bool,
    premultiply_alpha: bool,
}

macro_rules! image_upload {
    ($command:expr) => {
        ImageUpload {
            bitmap_index: $command.bitmap_index,
            format: $command.format,
            r#type: $command.r#type,
            has_explicit_destination_size: $command.has_explicit_destination_size,
            destination_width: $command.destination_width,
            destination_height: $command.destination_height,
            flip_y: $command.flip_y,
            premultiply_alpha: $command.premultiply_alpha,
        }
    };
}

fn pixels_of<P: AsRef<[u8]>>(image: &ConvertedImage<P>) -> Result<(i32, *const c_void), ReplayError> {
    let bytes = image.pixels.as_ref().map_or(&[][..], AsRef::as_ref);
    let size = i32::try_from(bytes.len()).map_err(|_| ReplayError("WebGL image upload is too big"))?;
    Ok((size, bytes.as_ptr().cast()))
}

impl<H: WebGLHost> Replayer<'_, H> {
    pub(super) fn shader_source(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::ShaderSource = decode(payload)?;
        let source = string_data(payload, command.source)?;
        let shader = self.objects.lookup(command.shader);
        let length =
            i32::try_from(command.source.size - 1).map_err(|_| ReplayError("WebGL shader source is too long"))?;
        // SAFETY: The context of the replayer is current, and the source has the length.
        unsafe { (self.gl.shader_source)(shader, 1, &raw const source, &raw const length) };
        Ok(())
    }

    pub(super) fn transform_feedback_varyings(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::TransformFeedbackVaryings = decode(payload)?;
        let varyings = split_packed_strings(data_span(payload, command.varyings)?, command.count)?;
        let program = self.objects.lookup(command.program);
        // SAFETY: The context of the replayer is current, and every string is NUL-terminated.
        unsafe {
            (self.gl.transform_feedback_varyings)(program, command.count, varyings.as_ptr(), command.buffer_mode);
        }
        Ok(())
    }

    pub(super) fn set_drawing_buffer_size(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::SetDrawingBufferSize = decode(payload)?;
        let dimensions = 1..=MAX_DRAWING_BUFFER_DIMENSION;
        if !dimensions.contains(&command.width) || !dimensions.contains(&command.height) {
            return Err(ReplayError("WebGL drawing buffer size is out of range"));
        }
        let size = IntSize {
            width: command.width,
            height: command.height,
        };
        self.host
            .set_webgl_drawing_buffer_size(self.client, self.canvas_id, size);
        self.defaults = self.host.make_webgl_context_current(self.client, self.canvas_id);
        Ok(())
    }

    pub(super) fn read_pixels_into_pixel_pack_buffer(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::ReadPixelsIntoPixelPackBuffer = decode(payload)?;
        // SAFETY: The context of the replayer is current, and GL writes into the bound pixel pack buffer at the offset.
        unsafe {
            (self.gl.read_pixels_robust_angle)(
                command.x,
                command.y,
                command.width,
                command.height,
                command.format,
                command.r#type,
                0,
                core::ptr::null_mut(),
                core::ptr::null_mut(),
                core::ptr::null_mut(),
                command.offset as isize as *mut c_void,
            );
        }
        Ok(())
    }

    fn image_source_at(&self, bitmap_index: u32) -> Result<ImageSource, ReplayError> {
        self.image_sources
            .get(bitmap_index as usize)
            .copied()
            .ok_or(ReplayError("WebGL image upload references an out-of-range bitmap"))
    }

    /// None where the image could not be converted, which leaves the texture as it is.
    fn converted_image(&self, upload: &ImageUpload) -> Result<Option<ConvertedImage<H::Pixels>>, ReplayError> {
        let source = self.image_source_at(upload.bitmap_index)?;
        let export_format = texture_export_format(upload.format, upload.r#type).ok_or(ReplayError(
            "WebGL image upload has an unsupported format and type combination",
        ))?;
        let mut target_size = None;
        if upload.has_explicit_destination_size {
            if upload.destination_width < 0 || upload.destination_height < 0 {
                return Ok(Some(ConvertedImage {
                    pixels: None,
                    width: upload.destination_width,
                    height: upload.destination_height,
                }));
            }
            target_size = Some(IntSize {
                width: upload.destination_width,
                height: upload.destination_height,
            });
        }
        let conversion = ImageConversion {
            export_format,
            flip_y: upload.flip_y,
            premultiply_alpha: upload.premultiply_alpha,
            target_size,
        };
        match self.host.convert_image_source(self.client, source, conversion) {
            ImageConversionOutcome::Converted { pixels, width, height } => Ok(Some(ConvertedImage {
                pixels: Some(pixels),
                width,
                height,
            })),
            ImageConversionOutcome::UnknownImageFrame => Err(ReplayError(
                "WebGL image upload names an image frame that is not registered",
            )),
            ImageConversionOutcome::Failed => {
                eprintln!("Compositor: Could not convert an image for a WebGL texture upload");
                Ok(None)
            }
        }
    }

    fn supports_pixel_buffer_objects(&self) -> bool {
        self.webgl2 || extension_is_enabled(self.gl, b"GL_NV_pixel_buffer_object")
    }

    fn supports_unpack_subimage_parameters(&self) -> bool {
        self.webgl2 || extension_is_enabled(self.gl, b"GL_EXT_unpack_subimage")
    }

    fn with_pixel_unpack_buffer_unbound(&self, upload: impl FnOnce()) {
        let has_pixel_buffer_objects = self.supports_pixel_buffer_objects();
        let mut bound_buffer = 0;
        if has_pixel_buffer_objects {
            // SAFETY: The context of the replayer is current, and the binding is room for one value.
            unsafe {
                (self.gl.get_integerv_robust_angle)(
                    GL_PIXEL_UNPACK_BUFFER_BINDING,
                    1,
                    core::ptr::null_mut(),
                    &raw mut bound_buffer,
                );
                (self.gl.bind_buffer)(GL_PIXEL_UNPACK_BUFFER, 0);
            }
        }
        upload();
        if has_pixel_buffer_objects {
            // SAFETY: The context of the replayer is current.
            unsafe { (self.gl.bind_buffer)(GL_PIXEL_UNPACK_BUFFER, bound_buffer as u32) };
        }
    }

    /// The pixels of a converted image are tightly packed, row after row and image after image, so the unpack
    /// parameters that the page set for its own data are reset for the upload, and restored after it.
    fn with_packed_unpack_state(&self, upload: impl FnOnce()) {
        let mut saved_parameters = Vec::with_capacity(6);
        let mut reset = |parameter: u32, value: i32| {
            let mut saved_value = 0;
            // SAFETY: The context of the replayer is current, and the saved value is room for one value.
            unsafe {
                (self.gl.get_integerv)(parameter, &raw mut saved_value);
                (self.gl.pixel_storei)(parameter, value);
            }
            saved_parameters.push((parameter, saved_value));
        };
        reset(GL_UNPACK_ALIGNMENT, 1);
        if self.supports_unpack_subimage_parameters() {
            reset(GL_UNPACK_ROW_LENGTH, 0);
            reset(GL_UNPACK_SKIP_PIXELS, 0);
            reset(GL_UNPACK_SKIP_ROWS, 0);
        }
        if self.webgl2 {
            reset(GL_UNPACK_IMAGE_HEIGHT, 0);
            reset(GL_UNPACK_SKIP_IMAGES, 0);
        }
        upload();
        for (parameter, saved_value) in saved_parameters {
            // SAFETY: The context of the replayer is current.
            unsafe { (self.gl.pixel_storei)(parameter, saved_value) };
        }
    }

    /// Defines the level of the texture with the call that an upload of pixels would make, so that WebGL sees the same
    /// errors, and copies the frame into it on the GPU. False where the frame has to be uploaded as pixels instead.
    fn define_texture_image_from_video_frame(
        &mut self,
        command: &commands::TexImage2DFromBitmap,
        frame: &H::VideoFrame,
        frame_size: IntSize,
    ) -> bool {
        if !self
            .host
            .webgl_can_copy_images_into_textures(self.client, self.canvas_id)
            || !copy_can_write_internal_format(command.internalformat)
        {
            return false;
        }
        let Some(size) = upload_size(
            frame_size,
            command.has_explicit_destination_size,
            command.destination_width,
            command.destination_height,
        ) else {
            return false;
        };
        let Some(image) = self.host.video_frame_image(frame) else {
            return false;
        };

        self.stash_errors();
        self.with_pixel_unpack_buffer_unbound(|| {
            // SAFETY: The context of the replayer is current, and GL reads no pixels.
            unsafe {
                (self.gl.tex_image2d_robust_angle)(
                    command.target,
                    command.level,
                    command.internalformat,
                    size.width,
                    size.height,
                    0,
                    command.format,
                    command.r#type,
                    0,
                    core::ptr::null(),
                );
            }
        });
        if self.stash_errors() || size.is_empty() {
            return true;
        }
        let copy = VideoFrameCopy {
            source_size: frame_size,
            size,
            target: command.target,
            level: command.level,
            xoffset: 0,
            yoffset: 0,
            flip_y: command.flip_y,
            premultiply_alpha: command.premultiply_alpha,
        };
        if !self
            .host
            .copy_video_frame_image_into_bound_texture(self.client, self.canvas_id, &image, copy)
        {
            return false;
        }
        self.discard_errors();
        true
    }

    fn update_texture_image_from_video_frame(
        &mut self,
        command: &commands::TexSubImage2DFromBitmap,
        frame: &H::VideoFrame,
        frame_size: IntSize,
    ) -> bool {
        if !self
            .host
            .webgl_can_copy_images_into_textures(self.client, self.canvas_id)
            || format_is_integer(command.format)
        {
            return false;
        }
        let Some(size) = upload_size(
            frame_size,
            command.has_explicit_destination_size,
            command.destination_width,
            command.destination_height,
        ) else {
            return false;
        };
        let Some(image) = self.host.video_frame_image(frame) else {
            return false;
        };

        self.stash_errors();
        self.with_pixel_unpack_buffer_unbound(|| {
            // SAFETY: The context of the replayer is current, and GL reads no pixels.
            unsafe {
                (self.gl.tex_sub_image2d_robust_angle)(
                    command.target,
                    command.level,
                    command.xoffset,
                    command.yoffset,
                    0,
                    0,
                    command.format,
                    command.r#type,
                    0,
                    core::ptr::null(),
                );
            }
        });
        if self.stash_errors() || size.is_empty() {
            return true;
        }
        let copy = VideoFrameCopy {
            source_size: frame_size,
            size,
            target: command.target,
            level: command.level,
            xoffset: command.xoffset,
            yoffset: command.yoffset,
            flip_y: command.flip_y,
            premultiply_alpha: command.premultiply_alpha,
        };
        if !self
            .host
            .copy_video_frame_image_into_bound_texture(self.client, self.canvas_id, &image, copy)
        {
            return false;
        }
        // NB: A region beyond the level is the caller's error, as it would be for an upload of pixels.
        self.stash_errors();
        true
    }

    fn held_video_frame_of(&self, source: ImageSource) -> Option<(H::VideoFrame, IntSize)> {
        let ImageSource::HeldVideoFrame {
            sink_handle,
            generation,
        } = source
        else {
            return None;
        };
        self.host.held_video_frame(self.client, sink_handle, generation)
    }

    pub(super) fn tex_image2d_from_bitmap(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::TexImage2DFromBitmap = decode(payload)?;
        let source = self.image_source_at(command.bitmap_index)?;
        if let Some((frame, frame_size)) = self.held_video_frame_of(source)
            && self.define_texture_image_from_video_frame(&command, &frame, frame_size)
        {
            return Ok(());
        }
        let Some(image) = self.converted_image(&image_upload!(command))? else {
            return Ok(());
        };
        let (pixels_size, pixels) = pixels_of(&image)?;
        self.with_packed_unpack_state(|| {
            // SAFETY: The context of the replayer is current, and the pixels have the size.
            unsafe {
                (self.gl.tex_image2d_robust_angle)(
                    command.target,
                    command.level,
                    command.internalformat,
                    image.width,
                    image.height,
                    0,
                    command.format,
                    command.r#type,
                    pixels_size,
                    pixels,
                );
            }
        });
        Ok(())
    }

    pub(super) fn tex_sub_image2d_from_bitmap(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::TexSubImage2DFromBitmap = decode(payload)?;
        let source = self.image_source_at(command.bitmap_index)?;
        if let Some((frame, frame_size)) = self.held_video_frame_of(source)
            && self.update_texture_image_from_video_frame(&command, &frame, frame_size)
        {
            return Ok(());
        }
        let Some(image) = self.converted_image(&image_upload!(command))? else {
            return Ok(());
        };
        let (pixels_size, pixels) = pixels_of(&image)?;
        self.with_packed_unpack_state(|| {
            // SAFETY: The context of the replayer is current, and the pixels have the size.
            unsafe {
                (self.gl.tex_sub_image2d_robust_angle)(
                    command.target,
                    command.level,
                    command.xoffset,
                    command.yoffset,
                    image.width,
                    image.height,
                    command.format,
                    command.r#type,
                    pixels_size,
                    pixels,
                );
            }
        });
        Ok(())
    }

    pub(super) fn tex_image3d_from_bitmap(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::TexImage3DFromBitmap = decode(payload)?;
        let Some(image) = self.converted_image(&image_upload!(command))? else {
            return Ok(());
        };
        let (pixels_size, pixels) = pixels_of(&image)?;
        self.with_packed_unpack_state(|| {
            // SAFETY: The context of the replayer is current, and the pixels have the size.
            unsafe {
                (self.gl.tex_image3d_robust_angle)(
                    command.target,
                    command.level,
                    command.internalformat,
                    image.width,
                    image.height,
                    command.depth,
                    0,
                    command.format,
                    command.r#type,
                    pixels_size,
                    pixels,
                );
            }
        });
        Ok(())
    }

    pub(super) fn tex_sub_image3d_from_bitmap(&mut self, payload: &[u8]) -> Result<(), ReplayError> {
        let command: commands::TexSubImage3DFromBitmap = decode(payload)?;
        let Some(image) = self.converted_image(&image_upload!(command))? else {
            return Ok(());
        };
        let (pixels_size, pixels) = pixels_of(&image)?;
        self.with_packed_unpack_state(|| {
            // SAFETY: The context of the replayer is current, and the pixels have the size.
            unsafe {
                (self.gl.tex_sub_image3d_robust_angle)(
                    command.target,
                    command.level,
                    command.xoffset,
                    command.yoffset,
                    command.zoffset,
                    image.width,
                    image.height,
                    command.depth,
                    command.format,
                    command.r#type,
                    pixels_size,
                    pixels,
                );
            }
        });
        Ok(())
    }

    pub(super) fn get_string(&mut self, payload: &[u8]) -> Result<Vec<u8>, ReplayError> {
        let request: sync_calls::GetStringRequest = decode(payload)?;
        // SAFETY: The context of the replayer is current, and GL answers a string or null.
        let value = unsafe { (self.gl.get_string)(request.name) };
        let value: &[u8] = if value.is_null() {
            b"\0"
        } else {
            // SAFETY: GL answers a NUL-terminated string.
            unsafe { CStr::from_ptr(value.cast::<c_char>()) }.to_bytes_with_nul()
        };
        let [value_span] = reply_data_spans::<sync_calls::GetStringReply, 1>([value.len()]);
        let reply = sync_calls::GetStringReply { value: value_span };
        Ok(encode_reply(&reply, &[value]))
    }

    pub(super) fn get_vertex_attrib_pointerv_robust_angle(&mut self, payload: &[u8]) -> Result<Vec<u8>, ReplayError> {
        let request: sync_calls::GetVertexAttribPointervRobustANGLERequest = decode(payload)?;
        let mut pointer = core::ptr::null_mut();
        let mut length = 0;
        // SAFETY: The context of the replayer is current, and the pointer is room for one.
        unsafe {
            (self.gl.get_vertex_attrib_pointerv_robust_angle)(
                request.index,
                request.pname,
                1,
                &raw mut length,
                &raw mut pointer,
            );
        }
        let reply = sync_calls::GetVertexAttribPointervRobustANGLEReply {
            pointer: pointer as usize as i64,
        };
        Ok(encode_reply(&reply, &[]))
    }

    pub(super) fn get_uniform_indices(&mut self, payload: &[u8]) -> Result<Vec<u8>, ReplayError> {
        let request: sync_calls::GetUniformIndicesRequest = decode(payload)?;
        let names = split_packed_strings(data_span(payload, request.uniform_names)?, request.uniform_count)?;
        let program = self.objects.lookup(request.program);
        let mut indices = vec![0_u32; names.len()];
        // SAFETY: The context of the replayer is current, every name is NUL-terminated, and there is room for an index
        //         of each.
        unsafe {
            (self.gl.get_uniform_indices)(program, request.uniform_count, names.as_ptr(), indices.as_mut_ptr());
        }
        let indices = bytes_of_slice(&indices);
        let [indices_span] = reply_data_spans::<sync_calls::GetUniformIndicesReply, 1>([indices.len()]);
        let reply = sync_calls::GetUniformIndicesReply {
            uniform_indices: indices_span,
        };
        Ok(encode_reply(&reply, &[indices]))
    }
}
