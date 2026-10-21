/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.14
//!
//! The operations of a WebGL 1 context that a WebGL 2 context has more overloads of.

use libjs_rust::api::{Gc, TypedArrayBase};

use super::buffer_source::{destination_bytes, list_bytes, source_bytes, write_checked};
use super::byte_ranges::size_of_zero_filled_texture;
use super::constants::{
    GL_ALPHA, GL_INVALID_ENUM, GL_INVALID_OPERATION, GL_INVALID_VALUE, GL_LUMINANCE, GL_LUMINANCE_ALPHA,
    GL_OUT_OF_MEMORY, GL_RGB, GL_RGBA, GL_UNSIGNED_BYTE, GL_UNSIGNED_SHORT_4_4_4_4, GL_UNSIGNED_SHORT_5_5_5_1,
    GL_UNSIGNED_SHORT_5_6_5,
};
use super::state::{RenderingContext, WebGL1};
use super::types::Float32List;
use super::web_gl_uniform_location::WebGLUniformLocation;
use crate::bindings::conversions::{ArrayBufferView, BufferSource, CppInterface};
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::image_bitmap::ImageBitmap;
use crate::html::image_data::ImageData;

/// https://registry.khronos.org/webgl/specs/latest/1.0/#5.14.8
// FIXME: VideoFrame
pub(crate) enum TexImageSource {
    ImageBitmap(Ref<ImageBitmap>),
    ImageData(Ref<ImageData>),
    HTMLImageElement(CppInterface),
    HTMLCanvasElement(CppInterface),
    HTMLVideoElement(CppInterface),
    OffscreenCanvas(CppInterface),
}

pub(crate) enum Int32List {
    Int32Array(Gc<TypedArrayBase>),
    Sequence(Vec<i32>),
}

#[expect(
    clippy::too_many_arguments,
    reason = "the operations have the arguments of the GL functions"
)]
impl RenderingContext<WebGL1> {
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
        data: Option<BufferSource>,
        usage: u32,
    ) -> WebIdlResult<()> {
        // https://registry.khronos.org/webgl/specs/latest/1.0/#5.14.5
        // If the passed data is null then an INVALID_VALUE error is generated.
        let Some(data) = data else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Ok(bytes) = source_bytes(data, 0, 0) else {
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
        offset: i64,
        data: BufferSource,
    ) -> WebIdlResult<()> {
        let Ok(bytes) = source_bytes(data, 0, 0) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let size = i64::from(bytes.gl_size());
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload(&bytes, |bytes| {
                self.proxy.buffer_sub_data(target, offset, size, bytes);
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
        data: ArrayBufferView,
    ) -> WebIdlResult<()> {
        if !self
            .enabled_compressed_texture_formats
            .borrow()
            .contains(&internalformat)
        {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }
        let Ok(bytes) = source_bytes(data.into(), 0, 0) else {
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
        data: ArrayBufferView,
    ) -> WebIdlResult<()> {
        if !self.enabled_compressed_texture_formats.borrow().contains(&format) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }
        let Ok(bytes) = source_bytes(data.into(), 0, 0) else {
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

    pub(crate) fn read_pixels(
        &self,
        _: &mut CallContext,
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
        let Ok(destination_bytes) = destination_bytes(&view, 0) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let Ok(buf_size) = i32::try_from(destination_bytes.len()) else {
            self.set_error(GL_OUT_OF_MEMORY);
            return Ok(());
        };
        self.proxy
            .read_pixels_robust_angle(x, y, width, height, format, r#type, buf_size, |_, pixels| {
                if write_checked(&view, pixels, 0).is_err() {
                    self.set_error(GL_INVALID_OPERATION);
                }
            });
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
        if let Some(pixels) = pixels {
            let Ok(texture_data) = self.texture_data_for_2d_upload(pixels, 0, width, height, format, r#type) else {
                self.set_error(GL_INVALID_OPERATION);
                return Ok(());
            };
            let buf_size = texture_data.gl_size();
            // SAFETY: Recording the command neither runs script nor allocates a cell.
            unsafe {
                self.upload_texture_data(&texture_data, |texture_data| {
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
            return Ok(());
        }

        if r#type == GL_UNSIGNED_SHORT_5_6_5 && format != GL_RGB {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }
        if (r#type == GL_UNSIGNED_SHORT_4_4_4_4 || r#type == GL_UNSIGNED_SHORT_5_5_5_1) && format != GL_RGBA {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }
        let bytes_per_pixel: usize = match (format, r#type) {
            (GL_ALPHA | GL_LUMINANCE, GL_UNSIGNED_BYTE) => 1,
            (GL_LUMINANCE_ALPHA, GL_UNSIGNED_BYTE)
            | (GL_RGB | GL_RGBA, GL_UNSIGNED_SHORT_4_4_4_4 | GL_UNSIGNED_SHORT_5_5_5_1 | GL_UNSIGNED_SHORT_5_6_5) => 2,
            (GL_RGB, GL_UNSIGNED_BYTE) => 3,
            (GL_RGBA, GL_UNSIGNED_BYTE) => 4,
            _ => {
                self.set_error(GL_INVALID_ENUM);
                return Ok(());
            }
        };
        let Ok(size) = size_of_zero_filled_texture(bytes_per_pixel, width, height) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let zeroes = vec![0; size.get()];
        self.proxy.tex_image2d_robust_angle(
            target,
            level,
            internalformat,
            width,
            height,
            border,
            format,
            r#type,
            size.as_gl_sizei(),
            Some(&zeroes),
        );
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
        let Some((frame, upload)) = self.read_texture_image_source(&source, format, r#type)? else {
            return Ok(());
        };
        self.proxy
            .tex_image2d_from_bitmap(target, level, internalformat, format, r#type, frame, upload);
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
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Ok(texture_data) = self.texture_data_for_2d_upload(pixels, 0, width, height, format, r#type) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = texture_data.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_texture_data(&texture_data, |texture_data| {
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
        let Some((frame, upload)) = self.read_texture_image_source(&source, format, r#type)? else {
            return Ok(());
        };
        self.proxy
            .tex_sub_image2d_from_bitmap(target, level, xoffset, yoffset, format, r#type, frame, upload);
        Ok(())
    }

    pub(crate) fn uniform1fv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v: Float32List,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&v, 0, 0) else {
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
        value: Float32List,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        const MATRIX_SIZE: usize = 2 * 2;
        let Ok(values) = list_bytes(&value, 0, 0) else {
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
        value: Float32List,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        const MATRIX_SIZE: usize = 3 * 3;
        let Ok(values) = list_bytes(&value, 0, 0) else {
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
        value: Float32List,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        const MATRIX_SIZE: usize = 4 * 4;
        let Ok(values) = list_bytes(&value, 0, 0) else {
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
}
