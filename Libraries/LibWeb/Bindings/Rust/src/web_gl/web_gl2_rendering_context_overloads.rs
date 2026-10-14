/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/2.0/#3.7
//!
//! The operations of a WebGL 2 context that it has more overloads of than a WebGL 1 context.

use super::buffer_source::{destination_bytes, list_bytes, source_bytes, write_checked};
use super::canvas::is_valid_2d_pixel_unpack_state;
use super::constants::{GL_INVALID_ENUM, GL_INVALID_OPERATION, GL_INVALID_VALUE, GL_OUT_OF_MEMORY};
use super::state::{RenderingContext, WebGL2};
use super::types::Float32List;
use super::web_gl_rendering_context_overloads::{Int32List, TexImageSource};
use super::web_gl_uniform_location::WebGLUniformLocation;
use crate::bindings::conversions::{ArrayBufferView, BufferSource};
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, WebIdlResult};

#[expect(
    clippy::too_many_arguments,
    reason = "the operations have the arguments of the GL functions"
)]
impl RenderingContext<WebGL2> {
    pub(crate) fn buffer_data_with_size(
        &self,
        _: &mut CallContext,
        target: u32,
        size: i64,
        usage: u32,
    ) -> WebIdlResult<()> {
        self.proxy.buffer_data(target, size, None, usage);
        Ok(())
    }

    pub(crate) fn buffer_data(
        &self,
        _: &mut CallContext,
        target: u32,
        src_data: Option<BufferSource>,
        usage: u32,
    ) -> WebIdlResult<()> {
        // https://registry.khronos.org/webgl/specs/latest/1.0/#5.14.5
        // If the passed data is null then an INVALID_VALUE error is generated.
        let Some(src_data) = src_data else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Ok(bytes) = source_bytes(src_data, 0, 0) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = i64::from(bytes.gl_size());
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&bytes, |bytes| {
                self.proxy.buffer_data(target, size, bytes, usage);
            });
        }
        Ok(())
    }

    pub(crate) fn buffer_data_with_offset(
        &self,
        _: &mut CallContext,
        target: u32,
        src_data: ArrayBufferView,
        usage: u32,
        src_offset: u64,
        length: u32,
    ) -> WebIdlResult<()> {
        let Ok(bytes) = source_bytes(src_data.into(), src_offset, length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = i64::from(bytes.gl_size());
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&bytes, |bytes| {
                self.proxy.buffer_data(target, size, bytes, usage);
            });
        }
        Ok(())
    }

    pub(crate) fn buffer_sub_data(
        &self,
        _: &mut CallContext,
        target: u32,
        dst_byte_offset: i64,
        src_data: BufferSource,
    ) -> WebIdlResult<()> {
        let Ok(bytes) = source_bytes(src_data, 0, 0) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = i64::from(bytes.gl_size());
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload(&bytes, |bytes| {
                self.proxy.buffer_sub_data(target, dst_byte_offset, size, bytes);
            });
        }
        Ok(())
    }

    pub(crate) fn buffer_sub_data_with_offset(
        &self,
        _: &mut CallContext,
        target: u32,
        dst_byte_offset: i64,
        src_data: ArrayBufferView,
        src_offset: u64,
        length: u32,
    ) -> WebIdlResult<()> {
        let Ok(bytes) = source_bytes(src_data.into(), src_offset, length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = i64::from(bytes.gl_size());
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload(&bytes, |bytes| {
                self.proxy.buffer_sub_data(target, dst_byte_offset, size, bytes);
            });
        }
        Ok(())
    }

    pub(crate) fn tex_image2d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        width: i32,
        height: i32,
        border: i32,
        format: u32,
        r#type: u32,
        pixels: Option<ArrayBufferView>,
    ) -> WebIdlResult<()> {
        let Some(pixels) = pixels else {
            self.proxy.tex_image2d_robust_angle(
                target,
                level,
                internalformat,
                width,
                height,
                border,
                format,
                r#type,
                0,
                None,
            );
            return Ok(());
        };

        let Ok(texture_data) = self.texture_data_for_2d_upload(pixels, 0, width, height, format, r#type) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = texture_data.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&texture_data, |texture_data| {
                self.proxy.tex_image2d_robust_angle(
                    target,
                    level,
                    internalformat,
                    width,
                    height,
                    border,
                    format,
                    r#type,
                    buf_size,
                    texture_data,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn tex_image2d_with_source(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        format: u32,
        r#type: u32,
        source: TexImageSource,
    ) -> WebIdlResult<()> {
        let Some((frame, upload)) = self.read_texture_image_source(&source, format, r#type) else {
            return Ok(());
        };
        self.proxy
            .tex_image2d_from_bitmap(target, level, internalformat, format, r#type, frame, upload);
        Ok(())
    }

    pub(crate) fn tex_image2d_with_size_and_source(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        width: i32,
        height: i32,
        border: i32,
        format: u32,
        r#type: u32,
        source: TexImageSource,
    ) -> WebIdlResult<()> {
        // https://registry.khronos.org/OpenGL-Refpages/es3.0/html/glTexImage2D.xhtml
        // border: This value must be 0.
        if border != 0 {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        }

        let Some((frame, mut upload)) = self.read_texture_image_source(&source, format, r#type) else {
            return Ok(());
        };
        upload.destination_size = Some((width, height));
        self.proxy
            .tex_image2d_from_bitmap(target, level, internalformat, format, r#type, frame, upload);
        Ok(())
    }

    pub(crate) fn tex_image2d_with_offset(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        width: i32,
        height: i32,
        border: i32,
        format: u32,
        r#type: u32,
        src_data: ArrayBufferView,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        let Ok(texture_data) = self.texture_data_for_2d_upload(src_data, src_offset, width, height, format, r#type)
        else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = texture_data.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&texture_data, |texture_data| {
                self.proxy.tex_image2d_robust_angle(
                    target,
                    level,
                    internalformat,
                    width,
                    height,
                    border,
                    format,
                    r#type,
                    buf_size,
                    texture_data,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn tex_sub_image2d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        pixels: Option<ArrayBufferView>,
    ) -> WebIdlResult<()> {
        let Some(pixels) = pixels else {
            if !is_valid_2d_pixel_unpack_state(width, &self.unpack_state.get()) {
                self.set_error(GL_INVALID_OPERATION);
                return Ok(());
            }
            self.proxy
                .tex_sub_image2d_robust_angle(target, level, xoffset, yoffset, width, height, format, r#type, 0, None);
            return Ok(());
        };

        let Ok(texture_data) = self.texture_data_for_2d_upload(pixels, 0, width, height, format, r#type) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = texture_data.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&texture_data, |texture_data| {
                self.proxy.tex_sub_image2d_robust_angle(
                    target,
                    level,
                    xoffset,
                    yoffset,
                    width,
                    height,
                    format,
                    r#type,
                    buf_size,
                    texture_data,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn tex_sub_image2d_with_source(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        format: u32,
        r#type: u32,
        source: TexImageSource,
    ) -> WebIdlResult<()> {
        let Some((frame, upload)) = self.read_texture_image_source(&source, format, r#type) else {
            return Ok(());
        };
        self.proxy
            .tex_sub_image2d_from_bitmap(target, level, xoffset, yoffset, format, r#type, frame, upload);
        Ok(())
    }

    pub(crate) fn tex_sub_image2d_with_size_and_source(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        source: TexImageSource,
    ) -> WebIdlResult<()> {
        let Some((frame, mut upload)) = self.read_texture_image_source(&source, format, r#type) else {
            return Ok(());
        };
        upload.destination_size = Some((width, height));
        self.proxy
            .tex_sub_image2d_from_bitmap(target, level, xoffset, yoffset, format, r#type, frame, upload);
        Ok(())
    }

    pub(crate) fn tex_sub_image2d_with_offset(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        src_data: ArrayBufferView,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        let Ok(texture_data) = self.texture_data_for_2d_upload(src_data, src_offset, width, height, format, r#type)
        else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = texture_data.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&texture_data, |texture_data| {
                self.proxy.tex_sub_image2d_robust_angle(
                    target,
                    level,
                    xoffset,
                    yoffset,
                    width,
                    height,
                    format,
                    r#type,
                    buf_size,
                    texture_data,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn compressed_tex_image2d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: u32,
        width: i32,
        height: i32,
        border: i32,
        src_data: ArrayBufferView,
        src_offset: u64,
        src_length_override: u32,
    ) -> WebIdlResult<()> {
        if !self
            .enabled_compressed_texture_formats
            .borrow()
            .contains(&internalformat)
        {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }
        let Ok(bytes) = source_bytes(src_data.into(), src_offset, src_length_override) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = bytes.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload(&bytes, |bytes| {
                self.proxy.compressed_tex_image2d_robust_angle(
                    target,
                    level,
                    internalformat,
                    width,
                    height,
                    border,
                    size,
                    size,
                    bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn compressed_tex_sub_image2d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        width: i32,
        height: i32,
        format: u32,
        src_data: ArrayBufferView,
        src_offset: u64,
        src_length_override: u32,
    ) -> WebIdlResult<()> {
        if !self.enabled_compressed_texture_formats.borrow().contains(&format) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }
        let Ok(bytes) = source_bytes(src_data.into(), src_offset, src_length_override) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = bytes.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload(&bytes, |bytes| {
                self.proxy.compressed_tex_sub_image2d_robust_angle(
                    target, level, xoffset, yoffset, width, height, format, size, size, bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn uniform1fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform1fv(location_handle, values.gl_count(), values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform2fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(2) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform2fv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform3fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(3) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform3fv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform4fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(4) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform4fv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform1iv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Int32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform1iv(location_handle, values.gl_count(), values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform2iv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Int32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(2) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform2iv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform3iv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Int32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(3) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform3iv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform4iv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Int32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(4) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform4iv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix2fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        transpose: bool,
        data: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        const MATRIX_SIZE: usize = 2 * 2;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix2fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix3fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        transpose: bool,
        data: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        const MATRIX_SIZE: usize = 3 * 3;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix3fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix4fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        transpose: bool,
        data: Float32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        const MATRIX_SIZE: usize = 4 * 4;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix4fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    /// https://registry.khronos.org/webgl/specs/latest/1.0/index.html#5.14.12
    pub(crate) fn read_pixels(
        &self,
        cx: &mut CallContext,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        pixels: Option<ArrayBufferView>,
    ) -> WebIdlResult<()> {
        let Some(view) = pixels else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.read_pixels_with_offset(cx, x, y, width, height, format, r#type, view, 0)
    }

    /// https://registry.khronos.org/webgl/specs/latest/2.0/#4.7.10
    pub(crate) fn read_pixels_with_offset(
        &self,
        _: &mut CallContext,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        dst_data: ArrayBufferView,
        dst_offset: u64,
    ) -> WebIdlResult<()> {
        // If a WebGLBuffer is bound to the PIXEL_PACK_BUFFER target, generates an INVALID_OPERATION error.
        if self.pixel_pack_buffer_binding.get().is_some() {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }

        // If pixel store parameter constraints are not met, generates an INVALID_OPERATION error.
        // If dstData doesn't have enough space for the read operation starting at dstOffset, generate
        // INVALID_OPERATION.
        let Ok(destination_bytes) = destination_bytes(&dst_data, dst_offset) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };

        let Ok(buf_size) = i32::try_from(destination_bytes.len()) else {
            self.set_error(GL_OUT_OF_MEMORY);
            return Ok(());
        };
        self.proxy
            .read_pixels_robust_angle(x, y, width, height, format, r#type, buf_size, |_, pixels| {
                if pixels.is_empty() {
                    return;
                }
                if write_checked(&dst_data, pixels, destination_bytes.start).is_err() {
                    self.set_error(GL_INVALID_OPERATION);
                }
            });
        Ok(())
    }

    /// https://registry.khronos.org/webgl/specs/latest/2.0/#4.7.10
    pub(crate) fn read_pixels_into_pack_buffer(
        &self,
        _: &mut CallContext,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
        offset: i64,
    ) -> WebIdlResult<()> {
        // If no WebGLBuffer is bound to the PIXEL_PACK_BUFFER target, generates an INVALID_OPERATION error.
        if self.pixel_pack_buffer_binding.get().is_none() {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }

        self.proxy
            .read_pixels_into_pixel_pack_buffer(x, y, width, height, format, r#type, offset);
        Ok(())
    }
}
