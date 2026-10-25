/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/2.0/#3.7

use ak::Utf16String;
use libjs_rust::api::{Array, Gc, MarkedVec, TypedArrayBase, Value};

use super::buffer_kind::BufferKind;
use super::buffer_source::{
    copy_of_first_numbers, destination_bytes_of_get_buffer_sub_data, list_bytes, numbers_from_offset, source_bytes,
    write_checked,
};
use super::constants::{
    GL_ANY_SAMPLES_PASSED, GL_ANY_SAMPLES_PASSED_CONSERVATIVE, GL_COLOR, GL_CURRENT_QUERY, GL_DEPTH, GL_INVALID_ENUM,
    GL_INVALID_INDEX, GL_INVALID_OPERATION, GL_INVALID_VALUE, GL_NUM_SAMPLE_COUNTS, GL_QUERY_RESULT,
    GL_QUERY_RESULT_AVAILABLE, GL_SAMPLES, GL_STENCIL, GL_TEXTURE_COMPARE_FUNC, GL_TEXTURE_COMPARE_MODE,
    GL_TEXTURE_MAG_FILTER, GL_TEXTURE_MAX_ANISOTROPY_EXT, GL_TEXTURE_MAX_LOD, GL_TEXTURE_MIN_FILTER,
    GL_TEXTURE_MIN_LOD, GL_TEXTURE_WRAP_R, GL_TEXTURE_WRAP_S, GL_TEXTURE_WRAP_T,
    GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN, GL_TRUE, GL_UNIFORM_ARRAY_STRIDE,
    GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, GL_UNIFORM_BLOCK_BINDING,
    GL_UNIFORM_BLOCK_DATA_SIZE, GL_UNIFORM_BLOCK_INDEX, GL_UNIFORM_BLOCK_NAME_LENGTH,
    GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER, GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER,
    GL_UNIFORM_IS_ROW_MAJOR, GL_UNIFORM_MATRIX_STRIDE, GL_UNIFORM_OFFSET, GL_UNIFORM_SIZE, GL_UNIFORM_TYPE,
    GL_WAIT_FAILED,
};
use super::gl_string::{utf8_bytes, utf16_string_from_gl_string};
use super::proxy::recorder::bytes_of;
use super::state::{Binding, RenderingContext, VertexArrayBinding, WebGL2};
use super::types::Float32List;
use super::web_gl_buffer::WebGLBuffer;
use super::web_gl_program::WebGLProgram;
use super::web_gl_query::WebGLQuery;
use super::web_gl_rendering_context_overloads::{Int32List, TexImageSource};
use super::web_gl_sampler::WebGLSampler;
use super::web_gl_sync::WebGLSync;
use super::web_gl_texture::WebGLTexture;
use super::web_gl_transform_feedback::WebGLTransformFeedback;
use super::web_gl_uniform_location::WebGLUniformLocation;
use super::web_gl_vertex_array_object::WebGLVertexArrayObject;
use crate::bindings::conversions::ArrayBufferView;
use crate::bindings::interface::Ref;
use crate::bindings::{CallContext, WebIdlResult};

pub(crate) enum Uint32List {
    Uint32Array(Gc<TypedArrayBase>),
    Sequence(Vec<u32>),
}

fn element_count_from_gl(count: i32) -> usize {
    usize::try_from(count).expect("a count that GL returns is not negative")
}

fn clear_value_count(buffer: u32) -> Option<usize> {
    match buffer {
        GL_COLOR => Some(4),
        GL_DEPTH | GL_STENCIL => Some(1),
        _ => None,
    }
}

#[expect(
    clippy::too_many_arguments,
    reason = "the operations have the arguments of the GL functions"
)]
impl RenderingContext<WebGL2> {
    pub(crate) fn copy_buffer_sub_data(
        &self,
        _: &mut CallContext,
        read_target: u32,
        write_target: u32,
        read_offset: i64,
        write_offset: i64,
        size: i64,
    ) -> WebIdlResult<()> {
        self.proxy
            .copy_buffer_sub_data(read_target, write_target, read_offset, write_offset, size);
        Ok(())
    }

    /// https://registry.khronos.org/webgl/specs/latest/2.0/#3.7.3
    pub(crate) fn get_buffer_sub_data(
        &self,
        _: &mut CallContext,
        target: u32,
        src_byte_offset: i64,
        dst_buffer: ArrayBufferView,
        dst_offset: u64,
        length: u32,
    ) -> WebIdlResult<()> {
        let Ok(destination_bytes) = destination_bytes_of_get_buffer_sub_data(&dst_buffer, dst_offset, length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(destination_bytes) = destination_bytes else {
            return Ok(());
        };

        // If copyLength is greater than zero, copy copyLength typed elements (each of size elementSize) from buf into
        // dstBuffer, reading buf starting at byte index srcByteOffset and writing into dstBuffer starting at element
        // index dstOffset.
        self.proxy
            .read_buffer_sub_data(target, src_byte_offset, destination_bytes.len(), |data| {
                if write_checked(&dst_buffer, data, destination_bytes.start).is_err() {
                    self.set_error(GL_INVALID_VALUE);
                }
            });
        Ok(())
    }

    pub(crate) fn blit_framebuffer(
        &self,
        _: &mut CallContext,
        src_x0: i32,
        src_y0: i32,
        src_x1: i32,
        src_y1: i32,
        dst_x0: i32,
        dst_y0: i32,
        dst_x1: i32,
        dst_y1: i32,
        mask: u32,
        filter: u32,
    ) -> WebIdlResult<()> {
        self.did_update_canvas_content();
        self.proxy.blit_framebuffer(
            src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter,
        );
        Ok(())
    }

    pub(crate) fn framebuffer_texture_layer(
        &self,
        _: &mut CallContext,
        target: u32,
        attachment: u32,
        texture: Option<Ref<WebGLTexture>>,
        level: i32,
        layer: i32,
    ) -> WebIdlResult<()> {
        let mut texture_handle = 0;
        if let Some(texture) = texture {
            let Some(handle) = self.handle_of(texture) else {
                return Ok(());
            };
            texture_handle = handle;
        }
        self.proxy
            .framebuffer_texture_layer(target, attachment, texture_handle, level, layer);
        Ok(())
    }

    pub(crate) fn invalidate_framebuffer(
        &self,
        _: &mut CallContext,
        target: u32,
        attachments: Vec<u32>,
    ) -> WebIdlResult<()> {
        let Ok(attachment_count) = i32::try_from(attachments.len()) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .invalidate_framebuffer(target, attachment_count, bytes_of(&attachments));
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn invalidate_sub_framebuffer(
        &self,
        _: &mut CallContext,
        target: u32,
        attachments: Vec<u32>,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    ) -> WebIdlResult<()> {
        let Ok(attachment_count) = i32::try_from(attachments.len()) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .invalidate_sub_framebuffer(target, attachment_count, bytes_of(&attachments), x, y, width, height);
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn read_buffer(&self, _: &mut CallContext, src: u32) -> WebIdlResult<()> {
        self.proxy.read_buffer(src);
        Ok(())
    }

    pub(crate) fn get_internalformat_parameter(
        &self,
        cx: &mut CallContext,
        target: u32,
        internalformat: u32,
        pname: u32,
    ) -> WebIdlResult<Value> {
        match pname {
            GL_SAMPLES => {
                let mut num_sample_counts = [0];
                self.proxy.get_internalformativ_robust_angle(
                    target,
                    internalformat,
                    GL_NUM_SAMPLE_COUNTS,
                    1,
                    &mut num_sample_counts,
                );
                let [num_sample_counts] = num_sample_counts;
                let mut samples = vec![0; element_count_from_gl(num_sample_counts)];
                self.proxy.get_internalformativ_robust_angle(
                    target,
                    internalformat,
                    GL_SAMPLES,
                    num_sample_counts,
                    &mut samples,
                );
                Ok(self.int32_array(cx, &samples))
            }
            _ => {
                self.set_error(GL_INVALID_ENUM);
                Ok(Value::NULL)
            }
        }
    }

    pub(crate) fn renderbuffer_storage_multisample(
        &self,
        _: &mut CallContext,
        target: u32,
        samples: i32,
        internalformat: u32,
        width: i32,
        height: i32,
    ) -> WebIdlResult<()> {
        self.proxy
            .renderbuffer_storage_multisample(target, samples, internalformat, width, height);
        Ok(())
    }

    pub(crate) fn tex_storage2d(
        &self,
        _: &mut CallContext,
        target: u32,
        levels: i32,
        internalformat: u32,
        width: i32,
        height: i32,
    ) -> WebIdlResult<()> {
        self.proxy.tex_storage2d(target, levels, internalformat, width, height);
        Ok(())
    }

    pub(crate) fn tex_storage3d(
        &self,
        _: &mut CallContext,
        target: u32,
        levels: i32,
        internalformat: u32,
        width: i32,
        height: i32,
        depth: i32,
    ) -> WebIdlResult<()> {
        self.proxy
            .tex_storage3d(target, levels, internalformat, width, height, depth);
        Ok(())
    }

    pub(crate) fn tex_image3d_with_source(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        width: i32,
        height: i32,
        depth: i32,
        border: i32,
        format: u32,
        r#type: u32,
        source: TexImageSource,
    ) -> WebIdlResult<()> {
        // https://registry.khronos.org/OpenGL-Refpages/es3.0/html/glTexImage3D.xhtml
        // border: This value must be 0.
        if border != 0 {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        }

        let Some((frame, mut upload)) = self.read_texture_image_source(&source, format, r#type)? else {
            return Ok(());
        };
        upload.destination_size = Some((width, height));
        self.proxy
            .tex_image3d_from_bitmap(target, level, internalformat, depth, format, r#type, frame, upload);
        Ok(())
    }

    pub(crate) fn tex_image3d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        width: i32,
        height: i32,
        depth: i32,
        border: i32,
        format: u32,
        r#type: u32,
        src_data: Option<ArrayBufferView>,
    ) -> WebIdlResult<()> {
        let Some(src_data) = src_data else {
            self.proxy.tex_image3d_robust_angle(
                target,
                level,
                internalformat,
                width,
                height,
                depth,
                border,
                format,
                r#type,
                0,
                None,
            );
            return Ok(());
        };

        if !self.unpack_flags_allow_3d_upload_from_array_buffer_view() {
            return Ok(());
        }
        let Ok(bytes) = source_bytes(src_data.into(), 0, 0) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = bytes.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&bytes, |bytes| {
                self.proxy.tex_image3d_robust_angle(
                    target,
                    level,
                    internalformat,
                    width,
                    height,
                    depth,
                    border,
                    format,
                    r#type,
                    buf_size,
                    bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn tex_image3d_with_offset(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: i32,
        width: i32,
        height: i32,
        depth: i32,
        border: i32,
        format: u32,
        r#type: u32,
        src_data: ArrayBufferView,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        if !self.unpack_flags_allow_3d_upload_from_array_buffer_view() {
            return Ok(());
        }
        let Ok(bytes) = source_bytes(src_data.into(), src_offset, 0) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = bytes.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&bytes, |bytes| {
                self.proxy.tex_image3d_robust_angle(
                    target,
                    level,
                    internalformat,
                    width,
                    height,
                    depth,
                    border,
                    format,
                    r#type,
                    buf_size,
                    bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn tex_sub_image3d_with_source(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        zoffset: i32,
        width: i32,
        height: i32,
        depth: i32,
        format: u32,
        r#type: u32,
        source: TexImageSource,
    ) -> WebIdlResult<()> {
        let Some((frame, mut upload)) = self.read_texture_image_source(&source, format, r#type)? else {
            return Ok(());
        };
        upload.destination_size = Some((width, height));
        self.proxy.tex_sub_image3d_from_bitmap(
            target, level, xoffset, yoffset, zoffset, depth, format, r#type, frame, upload,
        );
        Ok(())
    }

    pub(crate) fn tex_sub_image3d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        zoffset: i32,
        width: i32,
        height: i32,
        depth: i32,
        format: u32,
        r#type: u32,
        src_data: Option<ArrayBufferView>,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        let Some(src_data) = src_data else {
            self.proxy.tex_sub_image3d_robust_angle(
                target, level, xoffset, yoffset, zoffset, width, height, depth, format, r#type, 0, None,
            );
            return Ok(());
        };

        if !self.unpack_flags_allow_3d_upload_from_array_buffer_view() {
            return Ok(());
        }
        let Ok(bytes) = source_bytes(src_data.into(), src_offset, 0) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let buf_size = bytes.gl_size();
        // SAFETY: Recording the command neither runs script nor allocates a cell.
        unsafe {
            self.upload_through_nullable_pointer(&bytes, |bytes| {
                self.proxy.tex_sub_image3d_robust_angle(
                    target, level, xoffset, yoffset, zoffset, width, height, depth, format, r#type, buf_size, bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn compressed_tex_image3d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: u32,
        width: i32,
        height: i32,
        depth: i32,
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
                self.proxy.compressed_tex_image3d_robust_angle(
                    target,
                    level,
                    internalformat,
                    width,
                    height,
                    depth,
                    border,
                    size,
                    size,
                    bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn compressed_tex_sub_image3d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        zoffset: i32,
        width: i32,
        height: i32,
        depth: i32,
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
                self.proxy.compressed_tex_sub_image3d_robust_angle(
                    target, level, xoffset, yoffset, zoffset, width, height, depth, format, size, size, bytes,
                );
            });
        }
        Ok(())
    }

    pub(crate) fn uniform1ui(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v0: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform1ui(location_handle, v0);
        Ok(())
    }

    pub(crate) fn uniform2ui(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v0: u32,
        v1: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform2ui(location_handle, v0, v1);
        Ok(())
    }

    pub(crate) fn uniform3ui(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v0: u32,
        v1: u32,
        v2: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform3ui(location_handle, v0, v1, v2);
        Ok(())
    }

    pub(crate) fn uniform4ui(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        v0: u32,
        v1: u32,
        v2: u32,
        v3: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform4ui(location_handle, v0, v1, v2, v3);
        Ok(())
    }

    pub(crate) fn uniform1uiv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        data: Uint32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform1uiv(location_handle, values.gl_count(), values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform2uiv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        data: Uint32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(2) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform2uiv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform3uiv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        data: Uint32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(3) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform3uiv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform4uiv(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        data: Uint32List,
        src_offset: u64,
        src_length: u32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(4) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.uniform4uiv(location_handle, count, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix3x2fv(
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
        const MATRIX_SIZE: usize = 3 * 2;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix3x2fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix4x2fv(
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
        const MATRIX_SIZE: usize = 4 * 2;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix4x2fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix2x3fv(
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
        const MATRIX_SIZE: usize = 2 * 3;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix2x3fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix4x3fv(
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
        const MATRIX_SIZE: usize = 4 * 3;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix4x3fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix2x4fv(
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
        const MATRIX_SIZE: usize = 2 * 4;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix2x4fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn uniform_matrix3x4fv(
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
        const MATRIX_SIZE: usize = 3 * 4;
        let Ok(values) = list_bytes(&data, src_offset, src_length) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(count) = values.gl_count_of_whole_groups_of(MATRIX_SIZE) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy
            .uniform_matrix3x4fv(location_handle, count, transpose, values.as_bytes());
        Ok(())
    }

    pub(crate) fn vertex_attrib_i4i(
        &self,
        _: &mut CallContext,
        index: u32,
        x: i32,
        y: i32,
        z: i32,
        w: i32,
    ) -> WebIdlResult<()> {
        self.proxy.vertex_attrib_i4i(index, x, y, z, w);
        Ok(())
    }

    pub(crate) fn vertex_attrib_i4iv(&self, _: &mut CallContext, index: u32, values: Int32List) -> WebIdlResult<()> {
        let Some(values) = copy_of_first_numbers(&values, 4) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.vertex_attrib_i4iv(index, &values);
        Ok(())
    }

    pub(crate) fn vertex_attrib_i4ui(
        &self,
        _: &mut CallContext,
        index: u32,
        x: u32,
        y: u32,
        z: u32,
        w: u32,
    ) -> WebIdlResult<()> {
        self.proxy.vertex_attrib_i4ui(index, x, y, z, w);
        Ok(())
    }

    pub(crate) fn vertex_attrib_i4uiv(&self, _: &mut CallContext, index: u32, values: Uint32List) -> WebIdlResult<()> {
        let Some(values) = copy_of_first_numbers(&values, 4) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.vertex_attrib_i4uiv(index, &values);
        Ok(())
    }

    pub(crate) fn vertex_attrib_i_pointer(
        &self,
        _: &mut CallContext,
        index: u32,
        size: i32,
        r#type: u32,
        stride: i32,
        offset: i64,
    ) -> WebIdlResult<()> {
        self.proxy.vertex_attrib_i_pointer(index, size, r#type, stride, offset);
        Ok(())
    }

    pub(crate) fn vertex_attrib_divisor(&self, _: &mut CallContext, index: u32, divisor: u32) -> WebIdlResult<()> {
        self.proxy.vertex_attrib_divisor(index, divisor);
        Ok(())
    }

    pub(crate) fn draw_arrays_instanced(
        &self,
        _: &mut CallContext,
        mode: u32,
        first: i32,
        count: i32,
        instance_count: i32,
    ) -> WebIdlResult<()> {
        self.did_update_canvas_content();
        self.proxy.draw_arrays_instanced(mode, first, count, instance_count);
        Ok(())
    }

    pub(crate) fn draw_elements_instanced(
        &self,
        _: &mut CallContext,
        mode: u32,
        count: i32,
        r#type: u32,
        offset: i64,
        instance_count: i32,
    ) -> WebIdlResult<()> {
        self.proxy
            .draw_elements_instanced(mode, count, r#type, offset, instance_count);
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn draw_range_elements(
        &self,
        _: &mut CallContext,
        mode: u32,
        start: u32,
        end: u32,
        count: i32,
        r#type: u32,
        offset: i64,
    ) -> WebIdlResult<()> {
        self.did_update_canvas_content();
        self.proxy.draw_range_elements(mode, start, end, count, r#type, offset);
        Ok(())
    }

    pub(crate) fn draw_buffers(&self, _: &mut CallContext, buffers: Vec<u32>) -> WebIdlResult<()> {
        let Ok(buffer_count) = i32::try_from(buffers.len()) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.draw_buffers(buffer_count, bytes_of(&buffers));
        Ok(())
    }

    pub(crate) fn clear_bufferfv(
        &self,
        _: &mut CallContext,
        buffer: u32,
        drawbuffer: i32,
        values: Float32List,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        let Ok(values) = numbers_from_offset(&values, src_offset) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(clear_value_count) = clear_value_count(buffer) else {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        };
        let Some(values) = values.copy_of_first(clear_value_count) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.clear_bufferfv(buffer, drawbuffer, &values);
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn clear_bufferiv(
        &self,
        _: &mut CallContext,
        buffer: u32,
        drawbuffer: i32,
        values: Int32List,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        let Ok(values) = numbers_from_offset(&values, src_offset) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(clear_value_count) = clear_value_count(buffer) else {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        };
        let Some(values) = values.copy_of_first(clear_value_count) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.clear_bufferiv(buffer, drawbuffer, &values);
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn clear_bufferuiv(
        &self,
        _: &mut CallContext,
        buffer: u32,
        drawbuffer: i32,
        values: Uint32List,
        src_offset: u64,
    ) -> WebIdlResult<()> {
        let Ok(values) = numbers_from_offset(&values, src_offset) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        let Some(clear_value_count) = clear_value_count(buffer) else {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        };
        let Some(values) = values.copy_of_first(clear_value_count) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.clear_bufferuiv(buffer, drawbuffer, &values);
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn clear_bufferfi(
        &self,
        _: &mut CallContext,
        buffer: u32,
        drawbuffer: i32,
        depth: f32,
        stencil: i32,
    ) -> WebIdlResult<()> {
        self.did_update_canvas_content();
        self.proxy.clear_bufferfi(buffer, drawbuffer, depth, stencil);
        Ok(())
    }

    pub(crate) fn create_query(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLQuery>> {
        let mut handle = [0];
        self.proxy.gen_queries(&mut handle);
        Ok(WebGLQuery::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn delete_query(&self, _: &mut CallContext, query: Option<Ref<WebGLQuery>>) -> WebIdlResult<()> {
        let Some(query) = query else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(query) else {
            return Ok(());
        };
        self.proxy.delete_queries(1, &[handle]);
        Ok(())
    }

    fn active_query_binding(&self, target: u32) -> Option<&Binding<WebGLQuery>> {
        match target {
            GL_ANY_SAMPLES_PASSED => Some(&self.any_samples_passed),
            GL_ANY_SAMPLES_PASSED_CONSERVATIVE => Some(&self.any_samples_passed_conservative),
            GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN => Some(&self.transform_feedback_primitives_written),
            _ => None,
        }
    }

    pub(crate) fn begin_query(&self, _: &mut CallContext, target: u32, query: Ref<WebGLQuery>) -> WebIdlResult<()> {
        let Some(query_handle) = self.handle_of(query) else {
            return Ok(());
        };
        if let Some(active_query_binding) = self.active_query_binding(target) {
            active_query_binding.set(Some(query));
        }
        self.proxy.begin_query(target, query_handle);
        Ok(())
    }

    pub(crate) fn end_query(&self, _: &mut CallContext, target: u32) -> WebIdlResult<()> {
        if let Some(active_query_binding) = self.active_query_binding(target) {
            active_query_binding.set(None);
        }
        self.proxy.end_query(target);
        Ok(())
    }

    pub(crate) fn get_query(
        &self,
        _: &mut CallContext,
        target: u32,
        pname: u32,
    ) -> WebIdlResult<Option<Ref<WebGLQuery>>> {
        if pname != GL_CURRENT_QUERY {
            self.set_error(GL_INVALID_ENUM);
            return Ok(None);
        }
        let Some(active_query_binding) = self.active_query_binding(target) else {
            self.set_error(GL_INVALID_ENUM);
            return Ok(None);
        };
        Ok(active_query_binding.get())
    }

    pub(crate) fn get_query_parameter(
        &self,
        _: &mut CallContext,
        query: Ref<WebGLQuery>,
        pname: u32,
    ) -> WebIdlResult<Value> {
        let Some(query_handle) = self.handle_of(query) else {
            return Ok(Value::NULL);
        };
        let mut result = [0];
        self.proxy
            .get_query_objectuiv_robust_angle(query_handle, pname, 1, &mut result);
        let [result] = result;
        Ok(match pname {
            GL_QUERY_RESULT => Value::from_f64(f64::from(result)),
            GL_QUERY_RESULT_AVAILABLE => Value::from_bool(result == GL_TRUE),
            _ => Value::NULL,
        })
    }

    pub(crate) fn create_sampler(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLSampler>> {
        let mut handle = [0];
        self.proxy.gen_samplers(&mut handle);
        Ok(WebGLSampler::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn delete_sampler(&self, _: &mut CallContext, sampler: Option<Ref<WebGLSampler>>) -> WebIdlResult<()> {
        let Some(sampler) = sampler else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(sampler) else {
            return Ok(());
        };
        self.proxy.delete_samplers(1, &[handle]);
        Ok(())
    }

    pub(crate) fn bind_sampler(
        &self,
        _: &mut CallContext,
        unit: u32,
        sampler: Option<Ref<WebGLSampler>>,
    ) -> WebIdlResult<()> {
        let mut sampler_handle = 0;
        if let Some(sampler) = sampler {
            let Some(handle) = self.handle_of(sampler) else {
                return Ok(());
            };
            sampler_handle = handle;
        }
        self.proxy.bind_sampler(unit, sampler_handle);
        Ok(())
    }

    fn is_sampler_parameter_name(&self, pname: u32) -> bool {
        match pname {
            GL_TEXTURE_COMPARE_FUNC
            | GL_TEXTURE_COMPARE_MODE
            | GL_TEXTURE_MAG_FILTER
            | GL_TEXTURE_MAX_LOD
            | GL_TEXTURE_MIN_FILTER
            | GL_TEXTURE_MIN_LOD
            | GL_TEXTURE_WRAP_R
            | GL_TEXTURE_WRAP_S
            | GL_TEXTURE_WRAP_T => true,
            GL_TEXTURE_MAX_ANISOTROPY_EXT => self.extension_enabled("EXT_texture_filter_anisotropic"),
            _ => false,
        }
    }

    pub(crate) fn sampler_parameteri(
        &self,
        _: &mut CallContext,
        sampler: Ref<WebGLSampler>,
        pname: u32,
        param: i32,
    ) -> WebIdlResult<()> {
        let Some(sampler_handle) = self.handle_of(sampler) else {
            return Ok(());
        };
        if !self.is_sampler_parameter_name(pname) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }
        self.proxy.sampler_parameteri(sampler_handle, pname, param);
        Ok(())
    }

    pub(crate) fn sampler_parameterf(
        &self,
        _: &mut CallContext,
        sampler: Ref<WebGLSampler>,
        pname: u32,
        param: f32,
    ) -> WebIdlResult<()> {
        let Some(sampler_handle) = self.handle_of(sampler) else {
            return Ok(());
        };
        if !self.is_sampler_parameter_name(pname) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }
        self.proxy.sampler_parameterf(sampler_handle, pname, param);
        Ok(())
    }

    pub(crate) fn fence_sync(
        &self,
        cx: &mut CallContext,
        condition: u32,
        flags: u32,
    ) -> WebIdlResult<Option<Ref<WebGLSync>>> {
        let sync_handle = self.proxy.fence_sync(condition, flags);
        Ok(Some(WebGLSync::create(cx.vm(), self, sync_handle)))
    }

    pub(crate) fn delete_sync(&self, _: &mut CallContext, sync: Option<Ref<WebGLSync>>) -> WebIdlResult<()> {
        let Some(sync) = sync else {
            return Ok(());
        };
        let Ok(sync_handle) = sync.sync_handle_for_deletion(self) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        let Some(sync_handle) = sync_handle else {
            return Ok(());
        };
        self.proxy.delete_sync(sync_handle);
        Ok(())
    }

    pub(crate) fn client_wait_sync(
        &self,
        _: &mut CallContext,
        sync: Ref<WebGLSync>,
        flags: u32,
        timeout: u64,
    ) -> WebIdlResult<u32> {
        let Ok(sync_handle) = sync.sync_handle(self) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(GL_WAIT_FAILED);
        };
        Ok(self.proxy.client_wait_sync(sync_handle, flags, timeout))
    }

    pub(crate) fn wait_sync(
        &self,
        _: &mut CallContext,
        sync: Ref<WebGLSync>,
        flags: u32,
        timeout: i64,
    ) -> WebIdlResult<()> {
        let Ok(sync_handle) = sync.sync_handle(self) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };
        // NB: GL has an unsigned timeout, whose largest value is what TIMEOUT_IGNORED, which is -1 here, stands for.
        self.proxy.wait_sync(sync_handle, flags, timeout.cast_unsigned());
        Ok(())
    }

    pub(crate) fn get_sync_parameter(
        &self,
        _: &mut CallContext,
        sync: Ref<WebGLSync>,
        pname: u32,
    ) -> WebIdlResult<Value> {
        let Ok(sync_handle) = sync.sync_handle(self) else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(Value::NULL);
        };
        let mut result = [0];
        self.proxy.get_synciv(sync_handle, pname, 1, &mut result);
        let [result] = result;
        Ok(Value::from_i32(result))
    }

    pub(crate) fn create_transform_feedback(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLTransformFeedback>> {
        let mut handle = [0];
        self.proxy.gen_transform_feedbacks(&mut handle);
        Ok(WebGLTransformFeedback::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn delete_transform_feedback(
        &self,
        _: &mut CallContext,
        tf: Option<Ref<WebGLTransformFeedback>>,
    ) -> WebIdlResult<()> {
        let Some(transform_feedback) = tf else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(transform_feedback) else {
            return Ok(());
        };
        self.proxy.delete_transform_feedbacks(1, &[handle]);
        Ok(())
    }

    pub(crate) fn bind_transform_feedback(
        &self,
        _: &mut CallContext,
        target: u32,
        tf: Option<Ref<WebGLTransformFeedback>>,
    ) -> WebIdlResult<()> {
        let mut transform_feedback_handle = 0;
        if let Some(transform_feedback) = tf {
            let Some(handle) = self.handle_of(transform_feedback) else {
                return Ok(());
            };
            transform_feedback_handle = handle;
        }
        self.proxy.bind_transform_feedback(target, transform_feedback_handle);
        Ok(())
    }

    pub(crate) fn begin_transform_feedback(&self, _: &mut CallContext, primitive_mode: u32) -> WebIdlResult<()> {
        self.proxy.begin_transform_feedback(primitive_mode);
        Ok(())
    }

    pub(crate) fn end_transform_feedback(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.proxy.end_transform_feedback();
        Ok(())
    }

    pub(crate) fn transform_feedback_varyings(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        varyings: Vec<Utf16String>,
        buffer_mode: u32,
    ) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        let varyings: Vec<Vec<u8>> = varyings.iter().map(utf8_bytes).collect();
        let varyings: Vec<&[u8]> = varyings.iter().map(Vec::as_slice).collect();
        self.proxy
            .transform_feedback_varyings(program_handle, &varyings, buffer_mode);
        Ok(())
    }

    pub(crate) fn pause_transform_feedback(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.proxy.pause_transform_feedback();
        Ok(())
    }

    pub(crate) fn resume_transform_feedback(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.proxy.resume_transform_feedback();
        Ok(())
    }

    fn buffer_handle_for_indexed_binding(&self, target: u32, buffer: Option<Ref<WebGLBuffer>>) -> Option<u32> {
        let Some(buffer) = buffer else {
            return Some(0);
        };
        let buffer_handle = self.handle_of(buffer)?;
        if !buffer.kind_allows_binding_to(target) {
            self.set_error(GL_INVALID_OPERATION);
            return None;
        }

        // NB: GL reports a target that these do not bind buffers to, and binds nothing to it.
        if let Some(kind_of_buffers_at_target) = BufferKind::of_buffers_at_indexed_target(target) {
            buffer.was_bound_as(kind_of_buffers_at_target);
        }
        Some(buffer_handle)
    }

    pub(crate) fn bind_buffer_base(
        &self,
        _: &mut CallContext,
        target: u32,
        index: u32,
        buffer: Option<Ref<WebGLBuffer>>,
    ) -> WebIdlResult<()> {
        let Some(buffer_handle) = self.buffer_handle_for_indexed_binding(target, buffer) else {
            return Ok(());
        };
        self.proxy.bind_buffer_base(target, index, buffer_handle);
        Ok(())
    }

    pub(crate) fn bind_buffer_range(
        &self,
        _: &mut CallContext,
        target: u32,
        index: u32,
        buffer: Option<Ref<WebGLBuffer>>,
        offset: i64,
        size: i64,
    ) -> WebIdlResult<()> {
        let Some(buffer_handle) = self.buffer_handle_for_indexed_binding(target, buffer) else {
            return Ok(());
        };
        self.proxy.bind_buffer_range(target, index, buffer_handle, offset, size);
        Ok(())
    }

    pub(crate) fn get_uniform_indices(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        uniform_names: Vec<Utf16String>,
    ) -> WebIdlResult<Option<Vec<u32>>> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(None);
        };
        let uniform_names: Vec<Vec<u8>> = uniform_names.iter().map(utf8_bytes).collect();
        let uniform_names: Vec<&[u8]> = uniform_names.iter().map(Vec::as_slice).collect();
        let mut uniform_indices = vec![0; uniform_names.len()];
        self.proxy
            .get_uniform_indices(program_handle, &uniform_names, &mut uniform_indices);
        Ok(Some(uniform_indices))
    }

    pub(crate) fn get_active_uniforms(
        &self,
        cx: &mut CallContext,
        program: Ref<WebGLProgram>,
        uniform_indices: Vec<u32>,
        pname: u32,
    ) -> WebIdlResult<Value> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(Value::NULL);
        };
        let Ok(uniform_count) = i32::try_from(uniform_indices.len()) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(Value::NULL);
        };
        let mut params = vec![0; uniform_indices.len()];
        self.proxy.get_active_uniformsiv(
            program_handle,
            uniform_count,
            bytes_of(&uniform_indices),
            pname,
            &mut params,
        );

        let params_as_values = MarkedVec::with_capacity(cx.vm(), params.len());
        for param in params {
            params_as_values.push(match pname {
                GL_UNIFORM_TYPE | GL_UNIFORM_SIZE => Value::from_f64(f64::from(param.cast_unsigned())),
                GL_UNIFORM_BLOCK_INDEX | GL_UNIFORM_OFFSET | GL_UNIFORM_ARRAY_STRIDE | GL_UNIFORM_MATRIX_STRIDE => {
                    Value::from_i32(param)
                }
                GL_UNIFORM_IS_ROW_MAJOR => Value::from_bool(param.cast_unsigned() == GL_TRUE),
                _ => {
                    self.set_error(GL_INVALID_ENUM);
                    return Ok(Value::NULL);
                }
            });
        }
        Ok(Value::from_object(Array::create_from_list(
            cx.vm(),
            self.realm(),
            &params_as_values,
        )))
    }

    pub(crate) fn get_uniform_block_index(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        uniform_block_name: Utf16String,
    ) -> WebIdlResult<u32> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(GL_INVALID_INDEX);
        };
        Ok(self
            .proxy
            .get_uniform_block_index(program_handle, &utf8_bytes(&uniform_block_name)))
    }

    pub(crate) fn get_active_uniform_block_parameter(
        &self,
        cx: &mut CallContext,
        program: Ref<WebGLProgram>,
        uniform_block_index: u32,
        pname: u32,
    ) -> WebIdlResult<Value> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(Value::NULL);
        };
        match pname {
            GL_UNIFORM_BLOCK_BINDING | GL_UNIFORM_BLOCK_DATA_SIZE | GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS => {
                let mut result = [0];
                self.proxy.get_active_uniform_blockiv_robust_angle(
                    program_handle,
                    uniform_block_index,
                    pname,
                    1,
                    &mut result,
                );
                let [result] = result;
                Ok(Value::from_i32(result))
            }
            GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES => {
                let mut num_active_uniforms = [0];
                self.proxy.get_active_uniform_blockiv_robust_angle(
                    program_handle,
                    uniform_block_index,
                    GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS,
                    1,
                    &mut num_active_uniforms,
                );
                let [num_active_uniforms] = num_active_uniforms;
                let mut active_uniform_indices = vec![0; element_count_from_gl(num_active_uniforms)];
                self.proxy.get_active_uniform_blockiv_robust_angle(
                    program_handle,
                    uniform_block_index,
                    GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES,
                    num_active_uniforms,
                    &mut active_uniform_indices,
                );
                let active_uniform_indices: Vec<u32> =
                    active_uniform_indices.into_iter().map(i32::cast_unsigned).collect();
                Ok(self.uint32_array(cx, &active_uniform_indices))
            }
            GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER | GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER => {
                let mut result = [0];
                self.proxy.get_active_uniform_blockiv_robust_angle(
                    program_handle,
                    uniform_block_index,
                    pname,
                    1,
                    &mut result,
                );
                let [result] = result;
                Ok(Value::from_bool(result.cast_unsigned() == GL_TRUE))
            }
            _ => {
                self.set_error(GL_INVALID_ENUM);
                Ok(Value::NULL)
            }
        }
    }

    pub(crate) fn get_active_uniform_block_name(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        uniform_block_index: u32,
    ) -> WebIdlResult<Option<Utf16String>> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(None);
        };
        let mut uniform_block_name_length = [0];
        self.proxy.get_active_uniform_blockiv_robust_angle(
            program_handle,
            uniform_block_index,
            GL_UNIFORM_BLOCK_NAME_LENGTH,
            1,
            &mut uniform_block_name_length,
        );
        let [uniform_block_name_length] = uniform_block_name_length;
        let mut uniform_block_name = vec![0; element_count_from_gl(uniform_block_name_length)];
        if uniform_block_name.is_empty() {
            return Ok(Some(Utf16String::from_ascii(b"")));
        }
        self.proxy.get_active_uniform_block_name(
            program_handle,
            uniform_block_index,
            uniform_block_name_length,
            &mut uniform_block_name,
        );
        let length_without_nul = uniform_block_name.len() - 1;
        Ok(Some(utf16_string_from_gl_string(
            &uniform_block_name[..length_without_nul],
        )))
    }

    pub(crate) fn uniform_block_binding(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        uniform_block_index: u32,
        uniform_block_binding: u32,
    ) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        self.proxy
            .uniform_block_binding(program_handle, uniform_block_index, uniform_block_binding);
        Ok(())
    }

    pub(crate) fn create_vertex_array(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLVertexArrayObject>> {
        let mut handle = [0];
        self.proxy.gen_vertex_arrays(&mut handle);
        Ok(WebGLVertexArrayObject::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn delete_vertex_array(
        &self,
        _: &mut CallContext,
        vertex_array: Option<Ref<WebGLVertexArrayObject>>,
    ) -> WebIdlResult<()> {
        let Some(vertex_array) = vertex_array else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(vertex_array) else {
            return Ok(());
        };
        self.proxy.delete_vertex_arrays(1, &[handle]);
        let is_current_vertex_array = matches!(
            self.current_vertex_array.get(),
            Some(VertexArrayBinding::WebGL2(current_vertex_array))
                if current_vertex_array.as_ptr() == vertex_array.as_ptr()
        );
        if is_current_vertex_array {
            self.current_vertex_array.set(None);
        }
        Ok(())
    }

    pub(crate) fn is_vertex_array(
        &self,
        _: &mut CallContext,
        vertex_array: Option<Ref<WebGLVertexArrayObject>>,
    ) -> WebIdlResult<bool> {
        let Some(vertex_array) = vertex_array else {
            return Ok(false);
        };
        let Some(handle) = self.handle_for_query_of(vertex_array) else {
            return Ok(false);
        };
        Ok(self.proxy.is_vertex_array(handle))
    }

    pub(crate) fn bind_vertex_array(
        &self,
        _: &mut CallContext,
        array: Option<Ref<WebGLVertexArrayObject>>,
    ) -> WebIdlResult<()> {
        let mut array_handle = 0;
        if let Some(array) = array {
            let Some(handle) = self.handle_of(array) else {
                return Ok(());
            };
            array_handle = handle;
        }
        self.proxy.bind_vertex_array(array_handle);
        self.current_vertex_array.set(array.map(VertexArrayBinding::WebGL2));
        Ok(())
    }
}
