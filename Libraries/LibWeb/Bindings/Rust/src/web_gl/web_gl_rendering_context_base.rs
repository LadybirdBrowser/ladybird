/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://registry.khronos.org/webgl/specs/latest/1.0/#5.14

use ak::Utf16String;
use libjs_rust::api::{Gc, MarkedVec, Object, Trace};

use super::buffer_kind::BufferKind;
use super::buffer_source::copy_of_first_numbers;
use super::byte_ranges::gl_sizei_of_array_length;
use super::constants::{
    GL_ARRAY_BUFFER, GL_COLOR_ATTACHMENT0, GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, GL_DEPTH_ATTACHMENT,
    GL_DEPTH_STENCIL, GL_DEPTH_STENCIL_ATTACHMENT, GL_DEPTH24_STENCIL8, GL_DRAW_FRAMEBUFFER, GL_ELEMENT_ARRAY_BUFFER,
    GL_FRAGMENT_SHADER, GL_FRAMEBUFFER, GL_INFO_LOG_LENGTH, GL_INVALID_ENUM, GL_INVALID_OPERATION, GL_INVALID_VALUE,
    GL_PIXEL_PACK_BUFFER, GL_PIXEL_UNPACK_BUFFER, GL_READ_FRAMEBUFFER, GL_RENDERBUFFER, GL_SHADER_SOURCE_LENGTH,
    GL_STENCIL_ATTACHMENT, GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY, GL_TEXTURE_3D, GL_TEXTURE_CUBE_MAP,
    GL_TEXTURE_CUBE_MAP_NEGATIVE_X, GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, GL_TEXTURE_CUBE_MAP_NEGATIVE_Z,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X, GL_TEXTURE_CUBE_MAP_POSITIVE_Y, GL_TEXTURE_CUBE_MAP_POSITIVE_Z,
    GL_TRANSFORM_FEEDBACK_BUFFER, GL_UNIFORM_BUFFER, GL_UNPACK_ALIGNMENT, GL_UNPACK_COLORSPACE_CONVERSION_WEBGL,
    GL_UNPACK_FLIP_Y_WEBGL, GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL, GL_UNPACK_ROW_LENGTH, GL_UNPACK_SKIP_PIXELS,
    GL_UNPACK_SKIP_ROWS, GL_VERTEX_ATTRIB_ARRAY_POINTER, GL_VERTEX_SHADER,
};
use super::gl_string::{utf8_bytes, utf16_string_from_gl_string};
use super::state::{
    Binding, ContextState, MAX_WEBGL_DRAWING_BUFFER_DIMENSION, RenderingContext, Version, WebGLVersion,
};
use super::types::Float32List;
use super::web_gl_active_info::WebGLActiveInfo;
use super::web_gl_buffer::WebGLBuffer;
use super::web_gl_framebuffer::WebGLFramebuffer;
use super::web_gl_program::WebGLProgram;
use super::web_gl_renderbuffer::WebGLRenderbuffer;
use super::web_gl_rendering_context::WebGLPowerPreference;
use super::web_gl_shader::WebGLShader;
use super::web_gl_shader_precision_format::WebGLShaderPrecisionFormat;
use super::web_gl_texture::WebGLTexture;
use super::web_gl_uniform_location::WebGLUniformLocation;
use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::Ref;
use crate::bindings::promise::WebIdlPromise;
use crate::bindings::{CallContext, WebIdlResult};

/// https://www.khronos.org/registry/webgl/specs/latest/1.0/#WEBGLCONTEXTATTRIBUTES
#[derive(Clone, Copy, Debug)]
pub(crate) struct WebGLContextAttributes {
    pub(crate) alpha: bool,
    pub(crate) depth: bool,
    pub(crate) stencil: bool,
    pub(crate) antialias: bool,
    pub(crate) premultiplied_alpha: bool,
    pub(crate) preserve_drawing_buffer: bool,
    pub(crate) power_preference: WebGLPowerPreference,
    pub(crate) fail_if_major_performance_caveat: bool,
    pub(crate) desynchronized: bool,
    /// https://immersive-web.github.io/webxr/#dom-webglcontextattributes-xrcompatible
    pub(crate) xr_compatible: bool,
}

const ACTIVE_INFO_NAME_BUFFER_SIZE: usize = 256;
const ACTIVE_INFO_NAME_BUFFER_GL_SIZE: i32 = gl_sizei_of_array_length(ACTIVE_INFO_NAME_BUFFER_SIZE);

pub(crate) fn is_valid_framebuffer_binding_target(version: WebGLVersion, target: u32) -> bool {
    if target == GL_FRAMEBUFFER {
        return true;
    }
    version == WebGLVersion::WebGL2 && (target == GL_READ_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
}

pub(crate) fn is_valid_framebuffer_attachment(
    version: WebGLVersion,
    draw_buffers_extension_enabled: bool,
    attachment: u32,
) -> bool {
    if matches!(
        attachment,
        GL_COLOR_ATTACHMENT0 | GL_DEPTH_ATTACHMENT | GL_STENCIL_ATTACHMENT | GL_DEPTH_STENCIL_ATTACHMENT
    ) {
        return true;
    }
    if version == WebGLVersion::WebGL2 || draw_buffers_extension_enabled {
        return attachment > GL_COLOR_ATTACHMENT0 && attachment <= GL_COLOR_ATTACHMENT0 + 15;
    }
    false
}

fn is_valid_framebuffer_texture_2d_target(target: u32) -> bool {
    matches!(
        target,
        GL_TEXTURE_2D
            | GL_TEXTURE_CUBE_MAP_POSITIVE_X
            | GL_TEXTURE_CUBE_MAP_NEGATIVE_X
            | GL_TEXTURE_CUBE_MAP_POSITIVE_Y
            | GL_TEXTURE_CUBE_MAP_NEGATIVE_Y
            | GL_TEXTURE_CUBE_MAP_POSITIVE_Z
            | GL_TEXTURE_CUBE_MAP_NEGATIVE_Z
    )
}

fn holds_object<T: Trace>(binding: &Binding<T>, object: Ref<T>) -> bool {
    binding.get().is_some_and(|held| held.as_ptr() == object.as_ptr())
}

fn active_info_name(name_buffer: &[u8], length: i32) -> Utf16String {
    let length = usize::try_from(length).expect("the length of a name that GL wrote is not negative");
    utf16_string_from_gl_string(&name_buffer[..length])
}

fn zeroed_gl_string_buffer(size_with_terminating_nul: i32) -> Vec<u8> {
    let size = usize::try_from(size_with_terminating_nul).expect("GL does not report a negative size for a string");
    vec![0; size]
}

#[expect(
    clippy::too_many_arguments,
    reason = "the operations have the arguments of the GL functions"
)]
impl<V: Version> RenderingContext<V> {
    // NB: The state has a `canvas()` and an `is_context_lost()` of its own, which a method call on a context does not
    //     reach past the operations of the same names.
    pub(crate) fn canvas(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        Ok(ContextState::canvas(self).wrapper(cx.realm()))
    }

    pub(crate) fn drawing_buffer_width(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        let (width, _) = ContextState::canvas(self).bitmap_size();
        Ok(width.min(MAX_WEBGL_DRAWING_BUFFER_DIMENSION))
    }

    pub(crate) fn drawing_buffer_height(&self, _: &mut CallContext) -> WebIdlResult<i32> {
        let (_, height) = ContextState::canvas(self).bitmap_size();
        Ok(height.min(MAX_WEBGL_DRAWING_BUFFER_DIMENSION))
    }

    pub(crate) fn get_context_attributes(&self, _: &mut CallContext) -> WebIdlResult<Option<WebGLContextAttributes>> {
        if ContextState::is_context_lost(self) {
            return Ok(None);
        }
        Ok(Some(self.actual_context_parameters))
    }

    pub(crate) fn is_context_lost(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(ContextState::is_context_lost(self))
    }

    pub(crate) fn get_supported_extensions(&self, _: &mut CallContext) -> WebIdlResult<Option<Vec<Utf16String>>> {
        Ok(Some(self.supported_extensions()))
    }

    pub(crate) fn get_extension(&self, cx: &mut CallContext, name: Utf16String) -> WebIdlResult<Option<Gc<Object>>> {
        Ok(self.extension(cx, &name))
    }

    pub(crate) fn active_texture(&self, _: &mut CallContext, texture: u32) -> WebIdlResult<()> {
        self.proxy.active_texture(texture);
        Ok(())
    }

    pub(crate) fn attach_shader(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        shader: Ref<WebGLShader>,
    ) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(());
        };

        if holds_object(&program.attached_vertex_shader, shader)
            || holds_object(&program.attached_fragment_shader, shader)
        {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }

        if shader.r#type() == GL_VERTEX_SHADER && program.attached_vertex_shader.get().is_some() {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }

        if shader.r#type() == GL_FRAGMENT_SHADER && program.attached_fragment_shader.get().is_some() {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }

        self.proxy.attach_shader(program_handle, shader_handle);

        match shader.r#type() {
            GL_VERTEX_SHADER => program.attached_vertex_shader.set(Some(shader)),
            GL_FRAGMENT_SHADER => program.attached_fragment_shader.set(Some(shader)),
            _ => unreachable!("a shader is a vertex shader or a fragment shader"),
        }
        Ok(())
    }

    pub(crate) fn bind_attrib_location(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        index: u32,
        name: Utf16String,
    ) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        self.proxy
            .bind_attrib_location(program_handle, index, &utf8_bytes(&name));
        Ok(())
    }

    pub(crate) fn bind_buffer(
        &self,
        _: &mut CallContext,
        target: u32,
        buffer: Option<Ref<WebGLBuffer>>,
    ) -> WebIdlResult<()> {
        let mut buffer_handle = 0;
        if let Some(buffer) = buffer {
            let Some(handle) = self.handle_of(buffer) else {
                return Ok(());
            };
            buffer_handle = handle;

            if !buffer.kind_allows_binding_to(target) {
                self.set_error(GL_INVALID_OPERATION);
                return Ok(());
            }
        }

        let (binding, kind_of_buffers_at_target) = match target {
            GL_ARRAY_BUFFER => (&self.array_buffer_binding, BufferKind::OtherData),
            GL_ELEMENT_ARRAY_BUFFER => (&self.element_array_buffer_binding, BufferKind::ElementArray),
            // FIXME: A buffer of indices can be bound to COPY_READ_BUFFER and to COPY_WRITE_BUFFER as well.
            GL_COPY_READ_BUFFER if self.is_webgl2() => (&self.copy_read_buffer_binding, BufferKind::OtherData),
            GL_COPY_WRITE_BUFFER if self.is_webgl2() => (&self.copy_write_buffer_binding, BufferKind::OtherData),
            GL_PIXEL_PACK_BUFFER if self.is_webgl2() => (&self.pixel_pack_buffer_binding, BufferKind::OtherData),
            GL_PIXEL_UNPACK_BUFFER if self.is_webgl2() => (&self.pixel_unpack_buffer_binding, BufferKind::OtherData),
            GL_TRANSFORM_FEEDBACK_BUFFER if self.is_webgl2() => {
                (&self.transform_feedback_buffer_binding, BufferKind::OtherData)
            }
            GL_UNIFORM_BUFFER if self.is_webgl2() => (&self.uniform_buffer_binding, BufferKind::OtherData),
            _ => {
                self.set_error(GL_INVALID_ENUM);
                return Ok(());
            }
        };
        if let Some(buffer) = buffer {
            buffer.was_bound_as(kind_of_buffers_at_target);
        }
        binding.set(buffer);

        self.proxy.bind_buffer(target, buffer_handle);
        Ok(())
    }

    pub(crate) fn bind_framebuffer(
        &self,
        _: &mut CallContext,
        target: u32,
        framebuffer: Option<Ref<WebGLFramebuffer>>,
    ) -> WebIdlResult<()> {
        let mut framebuffer_handle = 0;
        if let Some(framebuffer) = framebuffer {
            let Some(handle) = self.handle_of(framebuffer) else {
                return Ok(());
            };
            framebuffer_handle = handle;
        }

        self.proxy.bind_framebuffer(target, framebuffer_handle);
        self.framebuffer_binding.set(framebuffer);
        Ok(())
    }

    pub(crate) fn bind_renderbuffer(
        &self,
        _: &mut CallContext,
        target: u32,
        renderbuffer: Option<Ref<WebGLRenderbuffer>>,
    ) -> WebIdlResult<()> {
        let mut renderbuffer_handle = 0;
        if let Some(renderbuffer) = renderbuffer {
            let Some(handle) = self.handle_of(renderbuffer) else {
                return Ok(());
            };
            renderbuffer_handle = handle;
        }

        self.proxy.bind_renderbuffer(target, renderbuffer_handle);
        self.renderbuffer_binding.set(renderbuffer);
        Ok(())
    }

    pub(crate) fn bind_texture(
        &self,
        _: &mut CallContext,
        target: u32,
        texture: Option<Ref<WebGLTexture>>,
    ) -> WebIdlResult<()> {
        let mut texture_handle = 0;
        if let Some(texture) = texture {
            let Some(handle) = self.handle_of(texture) else {
                return Ok(());
            };
            texture_handle = handle;
        }

        let binding = match target {
            GL_TEXTURE_2D => &self.texture_binding_2d,
            GL_TEXTURE_CUBE_MAP => &self.texture_binding_cube_map,
            GL_TEXTURE_2D_ARRAY if self.is_webgl2() => &self.texture_binding_2d_array,
            GL_TEXTURE_3D if self.is_webgl2() => &self.texture_binding_3d,
            _ => {
                self.set_error(GL_INVALID_ENUM);
                return Ok(());
            }
        };
        binding.set(texture);

        self.proxy.bind_texture(target, texture_handle);
        Ok(())
    }

    pub(crate) fn blend_color(
        &self,
        _: &mut CallContext,
        red: f32,
        green: f32,
        blue: f32,
        alpha: f32,
    ) -> WebIdlResult<()> {
        self.proxy.blend_color(red, green, blue, alpha);
        Ok(())
    }

    pub(crate) fn blend_equation(&self, _: &mut CallContext, mode: u32) -> WebIdlResult<()> {
        self.proxy.blend_equation(mode);
        Ok(())
    }

    pub(crate) fn blend_equation_separate(
        &self,
        _: &mut CallContext,
        mode_rgb: u32,
        mode_alpha: u32,
    ) -> WebIdlResult<()> {
        self.proxy.blend_equation_separate(mode_rgb, mode_alpha);
        Ok(())
    }

    pub(crate) fn blend_func(&self, _: &mut CallContext, sfactor: u32, dfactor: u32) -> WebIdlResult<()> {
        self.proxy.blend_func(sfactor, dfactor);
        Ok(())
    }

    pub(crate) fn blend_func_separate(
        &self,
        _: &mut CallContext,
        src_rgb: u32,
        dst_rgb: u32,
        src_alpha: u32,
        dst_alpha: u32,
    ) -> WebIdlResult<()> {
        self.proxy.blend_func_separate(src_rgb, dst_rgb, src_alpha, dst_alpha);
        Ok(())
    }

    pub(crate) fn check_framebuffer_status(&self, _: &mut CallContext, target: u32) -> WebIdlResult<u32> {
        Ok(self.proxy.check_framebuffer_status(target))
    }

    pub(crate) fn clear(&self, _: &mut CallContext, mask: u32) -> WebIdlResult<()> {
        self.did_update_canvas_content();
        self.proxy.clear(mask);
        Ok(())
    }

    pub(crate) fn clear_color(
        &self,
        _: &mut CallContext,
        red: f32,
        green: f32,
        blue: f32,
        alpha: f32,
    ) -> WebIdlResult<()> {
        self.proxy.clear_color(red, green, blue, alpha);
        Ok(())
    }

    pub(crate) fn clear_depth(&self, _: &mut CallContext, depth: f32) -> WebIdlResult<()> {
        self.proxy.clear_depthf(depth);
        Ok(())
    }

    pub(crate) fn clear_stencil(&self, _: &mut CallContext, s: i32) -> WebIdlResult<()> {
        self.proxy.clear_stencil(s);
        Ok(())
    }

    pub(crate) fn color_mask(
        &self,
        _: &mut CallContext,
        red: bool,
        green: bool,
        blue: bool,
        alpha: bool,
    ) -> WebIdlResult<()> {
        self.proxy.color_mask(red, green, blue, alpha);
        Ok(())
    }

    pub(crate) fn compile_shader(&self, _: &mut CallContext, shader: Ref<WebGLShader>) -> WebIdlResult<()> {
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(());
        };
        self.proxy.compile_shader(shader_handle);
        Ok(())
    }

    pub(crate) fn copy_tex_image2d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        internalformat: u32,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
        border: i32,
    ) -> WebIdlResult<()> {
        self.proxy
            .copy_tex_image2d(target, level, internalformat, x, y, width, height, border);
        Ok(())
    }

    pub(crate) fn copy_tex_sub_image2d(
        &self,
        _: &mut CallContext,
        target: u32,
        level: i32,
        xoffset: i32,
        yoffset: i32,
        x: i32,
        y: i32,
        width: i32,
        height: i32,
    ) -> WebIdlResult<()> {
        self.proxy
            .copy_tex_sub_image2d(target, level, xoffset, yoffset, x, y, width, height);
        Ok(())
    }

    pub(crate) fn create_buffer(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLBuffer>> {
        let mut handle = [0];
        self.proxy.gen_buffers(&mut handle);
        Ok(WebGLBuffer::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn create_framebuffer(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLFramebuffer>> {
        let mut handle = [0];
        self.proxy.gen_framebuffers(&mut handle);
        Ok(WebGLFramebuffer::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn create_program(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLProgram>> {
        let handle = self.proxy.create_program();
        Ok(WebGLProgram::create(cx.vm(), self, handle))
    }

    pub(crate) fn create_renderbuffer(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLRenderbuffer>> {
        let mut handle = [0];
        self.proxy.gen_renderbuffers(&mut handle);
        Ok(WebGLRenderbuffer::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn create_shader(&self, cx: &mut CallContext, r#type: u32) -> WebIdlResult<Option<Ref<WebGLShader>>> {
        if r#type != GL_VERTEX_SHADER && r#type != GL_FRAGMENT_SHADER {
            self.set_error(GL_INVALID_ENUM);
            return Ok(None);
        }

        let handle = self.proxy.create_shader(r#type);
        Ok(Some(WebGLShader::create(cx.vm(), self, handle, r#type)))
    }

    pub(crate) fn create_texture(&self, cx: &mut CallContext) -> WebIdlResult<Ref<WebGLTexture>> {
        let mut handle = [0];
        self.proxy.gen_textures(&mut handle);
        Ok(WebGLTexture::create(cx.vm(), self, handle[0]))
    }

    pub(crate) fn cull_face(&self, _: &mut CallContext, mode: u32) -> WebIdlResult<()> {
        self.proxy.cull_face(mode);
        Ok(())
    }

    pub(crate) fn delete_buffer(&self, _: &mut CallContext, buffer: Option<Ref<WebGLBuffer>>) -> WebIdlResult<()> {
        let Some(buffer) = buffer else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(buffer) else {
            return Ok(());
        };
        self.proxy.delete_buffers(1, &[handle]);
        Ok(())
    }

    pub(crate) fn delete_framebuffer(
        &self,
        _: &mut CallContext,
        framebuffer: Option<Ref<WebGLFramebuffer>>,
    ) -> WebIdlResult<()> {
        let Some(framebuffer) = framebuffer else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(framebuffer) else {
            return Ok(());
        };
        self.proxy.delete_framebuffers(1, &[handle]);

        if holds_object(&self.framebuffer_binding, framebuffer) {
            self.framebuffer_binding.set(None);
        }
        Ok(())
    }

    pub(crate) fn delete_program(&self, _: &mut CallContext, program: Option<Ref<WebGLProgram>>) -> WebIdlResult<()> {
        let Some(program) = program else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(program) else {
            return Ok(());
        };
        self.proxy.delete_program(handle);

        if holds_object(&self.current_program, program) {
            self.current_program.set(None);
        }
        Ok(())
    }

    pub(crate) fn delete_renderbuffer(
        &self,
        _: &mut CallContext,
        renderbuffer: Option<Ref<WebGLRenderbuffer>>,
    ) -> WebIdlResult<()> {
        let Some(renderbuffer) = renderbuffer else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(renderbuffer) else {
            return Ok(());
        };
        self.proxy.delete_renderbuffers(1, &[handle]);
        Ok(())
    }

    pub(crate) fn delete_shader(&self, _: &mut CallContext, shader: Option<Ref<WebGLShader>>) -> WebIdlResult<()> {
        let Some(shader) = shader else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(shader) else {
            return Ok(());
        };
        self.proxy.delete_shader(handle);
        Ok(())
    }

    pub(crate) fn delete_texture(&self, _: &mut CallContext, texture: Option<Ref<WebGLTexture>>) -> WebIdlResult<()> {
        let Some(texture) = texture else {
            return Ok(());
        };
        let Some(handle) = self.handle_for_deletion_of(texture) else {
            return Ok(());
        };
        self.proxy.delete_textures(1, &[handle]);

        for binding in [
            &self.texture_binding_2d,
            &self.texture_binding_cube_map,
            &self.texture_binding_2d_array,
            &self.texture_binding_3d,
        ] {
            if holds_object(binding, texture) {
                binding.set(None);
            }
        }
        Ok(())
    }

    pub(crate) fn depth_func(&self, _: &mut CallContext, func: u32) -> WebIdlResult<()> {
        self.proxy.depth_func(func);
        Ok(())
    }

    pub(crate) fn depth_mask(&self, _: &mut CallContext, flag: bool) -> WebIdlResult<()> {
        self.proxy.depth_mask(flag);
        Ok(())
    }

    pub(crate) fn depth_range(&self, _: &mut CallContext, z_near: f32, z_far: f32) -> WebIdlResult<()> {
        self.proxy.depth_rangef(z_near, z_far);
        Ok(())
    }

    pub(crate) fn detach_shader(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        shader: Ref<WebGLShader>,
    ) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(());
        };

        self.proxy.detach_shader(program_handle, shader_handle);

        match shader.r#type() {
            GL_VERTEX_SHADER => program.attached_vertex_shader.set(None),
            GL_FRAGMENT_SHADER => program.attached_fragment_shader.set(None),
            _ => {}
        }
        Ok(())
    }

    pub(crate) fn disable(&self, _: &mut CallContext, cap: u32) -> WebIdlResult<()> {
        self.proxy.disable(cap);
        Ok(())
    }

    pub(crate) fn disable_vertex_attrib_array(&self, _: &mut CallContext, index: u32) -> WebIdlResult<()> {
        self.proxy.disable_vertex_attrib_array(index);
        Ok(())
    }

    pub(crate) fn draw_arrays(&self, _: &mut CallContext, mode: u32, first: i32, count: i32) -> WebIdlResult<()> {
        self.did_update_canvas_content();
        self.proxy.draw_arrays(mode, first, count);
        Ok(())
    }

    pub(crate) fn draw_elements(
        &self,
        _: &mut CallContext,
        mode: u32,
        count: i32,
        r#type: u32,
        offset: i64,
    ) -> WebIdlResult<()> {
        self.proxy.draw_elements(mode, count, r#type, offset);
        self.did_update_canvas_content();
        Ok(())
    }

    pub(crate) fn enable(&self, _: &mut CallContext, cap: u32) -> WebIdlResult<()> {
        self.proxy.enable(cap);
        Ok(())
    }

    pub(crate) fn enable_vertex_attrib_array(&self, _: &mut CallContext, index: u32) -> WebIdlResult<()> {
        self.proxy.enable_vertex_attrib_array(index);
        Ok(())
    }

    pub(crate) fn finish(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.proxy.finish();
        Ok(())
    }

    pub(crate) fn flush(&self, _: &mut CallContext) -> WebIdlResult<()> {
        self.proxy.flush();
        Ok(())
    }

    pub(crate) fn framebuffer_renderbuffer(
        &self,
        _: &mut CallContext,
        target: u32,
        attachment: u32,
        renderbuffertarget: u32,
        renderbuffer: Option<Ref<WebGLRenderbuffer>>,
    ) -> WebIdlResult<()> {
        if !is_valid_framebuffer_binding_target(self.version(), target) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }

        if !is_valid_framebuffer_attachment(self.version(), self.extension_enabled("WEBGL_draw_buffers"), attachment) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }

        if renderbuffertarget != GL_RENDERBUFFER {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }

        let Some(bound_framebuffer) = self.framebuffer_binding.get() else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };

        let mut renderbuffer_handle = 0;
        if let Some(renderbuffer) = renderbuffer {
            let Some(handle) = self.handle_of(renderbuffer) else {
                return Ok(());
            };
            renderbuffer_handle = handle;
        }
        self.proxy
            .framebuffer_renderbuffer(target, attachment, renderbuffertarget, renderbuffer_handle);
        bound_framebuffer.set_renderbuffer_attachment(attachment, renderbuffer);
        Ok(())
    }

    pub(crate) fn framebuffer_texture2d(
        &self,
        _: &mut CallContext,
        target: u32,
        attachment: u32,
        textarget: u32,
        texture: Option<Ref<WebGLTexture>>,
        level: i32,
    ) -> WebIdlResult<()> {
        if !is_valid_framebuffer_binding_target(self.version(), target) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }

        if !is_valid_framebuffer_attachment(self.version(), self.extension_enabled("WEBGL_draw_buffers"), attachment) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }

        if !is_valid_framebuffer_texture_2d_target(textarget) {
            self.set_error(GL_INVALID_ENUM);
            return Ok(());
        }

        if level < 0 || (self.version() == WebGLVersion::WebGL1 && level != 0) {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        }

        let Some(bound_framebuffer) = self.framebuffer_binding.get() else {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        };

        let mut texture_handle = 0;
        if let Some(texture) = texture {
            let Some(handle) = self.handle_of(texture) else {
                return Ok(());
            };
            texture_handle = handle;
        }
        self.proxy
            .framebuffer_texture2d(target, attachment, textarget, texture_handle, level);
        bound_framebuffer.set_texture_attachment(attachment, texture, textarget, level);
        Ok(())
    }

    pub(crate) fn front_face(&self, _: &mut CallContext, mode: u32) -> WebIdlResult<()> {
        self.proxy.front_face(mode);
        Ok(())
    }

    pub(crate) fn generate_mipmap(&self, _: &mut CallContext, target: u32) -> WebIdlResult<()> {
        self.proxy.generate_mipmap(target);
        Ok(())
    }

    pub(crate) fn get_active_attrib(
        &self,
        cx: &mut CallContext,
        program: Ref<WebGLProgram>,
        index: u32,
    ) -> WebIdlResult<Option<Ref<WebGLActiveInfo>>> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(None);
        };

        let mut name = [0; ACTIVE_INFO_NAME_BUFFER_SIZE];
        let outputs = self
            .proxy
            .get_active_attrib(program_handle, index, ACTIVE_INFO_NAME_BUFFER_GL_SIZE, &mut name);
        Ok(Some(WebGLActiveInfo::create(
            cx.vm(),
            active_info_name(&name, outputs.length),
            outputs.r#type,
            outputs.size,
        )))
    }

    pub(crate) fn get_active_uniform(
        &self,
        cx: &mut CallContext,
        program: Ref<WebGLProgram>,
        index: u32,
    ) -> WebIdlResult<Option<Ref<WebGLActiveInfo>>> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(None);
        };

        let mut name = [0; ACTIVE_INFO_NAME_BUFFER_SIZE];
        let outputs = self
            .proxy
            .get_active_uniform(program_handle, index, ACTIVE_INFO_NAME_BUFFER_GL_SIZE, &mut name);
        Ok(Some(WebGLActiveInfo::create(
            cx.vm(),
            active_info_name(&name, outputs.length),
            outputs.r#type,
            outputs.size,
        )))
    }

    pub(crate) fn get_attached_shaders<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
        program: Ref<WebGLProgram>,
    ) -> WebIdlResult<Option<MarkedVec<'vm, Ref<WebGLShader>>>> {
        if self.handle_of(program).is_none() {
            return Ok(None);
        }

        let attached_shaders = MarkedVec::with_capacity(cx.vm(), 2);
        for shader in [
            program.attached_vertex_shader.get(),
            program.attached_fragment_shader.get(),
        ]
        .into_iter()
        .flatten()
        {
            attached_shaders.push(shader);
        }
        Ok(Some(attached_shaders))
    }

    pub(crate) fn get_attrib_location(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
        name: Utf16String,
    ) -> WebIdlResult<i32> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(-1);
        };
        Ok(self.proxy.get_attrib_location(program_handle, &utf8_bytes(&name)))
    }

    pub(crate) fn make_xr_compatible(&self, _: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        // SAFETY: The cell is that of this context.
        Ok(unsafe { super::canvas::make_xr_compatible(self.context_ref().cell()) })
    }

    pub(crate) fn get_error(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.take_error())
    }

    pub(crate) fn get_program_info_log(
        &self,
        _: &mut CallContext,
        program: Ref<WebGLProgram>,
    ) -> WebIdlResult<Option<Utf16String>> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(None);
        };

        let mut info_log_length = [0i32; 1];
        self.proxy
            .get_programiv(program_handle, GL_INFO_LOG_LENGTH, &mut info_log_length);
        let [info_log_length] = info_log_length;
        if info_log_length == 0 {
            return Ok(Some(Utf16String::from_ascii(b"")));
        }
        let mut info_log = zeroed_gl_string_buffer(info_log_length);
        self.proxy
            .get_program_info_log(program_handle, info_log_length, &mut info_log);
        Ok(Some(utf16_string_from_gl_string(&info_log[..info_log.len() - 1])))
    }

    pub(crate) fn get_shader_precision_format(
        &self,
        cx: &mut CallContext,
        shadertype: u32,
        precisiontype: u32,
    ) -> WebIdlResult<Option<Ref<WebGLShaderPrecisionFormat>>> {
        let mut range = [0i32; 2];
        let mut precision = [0i32; 1];
        self.proxy
            .get_shader_precision_format(shadertype, precisiontype, &mut range, &mut precision);
        let [range_min, range_max] = range;
        let [precision] = precision;
        Ok(Some(WebGLShaderPrecisionFormat::create(
            cx.vm(),
            range_min,
            range_max,
            precision,
        )))
    }

    pub(crate) fn get_shader_info_log(
        &self,
        _: &mut CallContext,
        shader: Ref<WebGLShader>,
    ) -> WebIdlResult<Option<Utf16String>> {
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(None);
        };

        let mut info_log_length = [0i32; 1];
        self.proxy
            .get_shaderiv(shader_handle, GL_INFO_LOG_LENGTH, &mut info_log_length);
        let [info_log_length] = info_log_length;
        if info_log_length == 0 {
            return Ok(Some(Utf16String::from_ascii(b"")));
        }
        let mut info_log = zeroed_gl_string_buffer(info_log_length);
        self.proxy
            .get_shader_info_log(shader_handle, info_log_length, &mut info_log);
        Ok(Some(utf16_string_from_gl_string(&info_log[..info_log.len() - 1])))
    }

    pub(crate) fn get_shader_source(
        &self,
        _: &mut CallContext,
        shader: Ref<WebGLShader>,
    ) -> WebIdlResult<Option<Utf16String>> {
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(None);
        };

        let mut shader_source_length = [0i32; 1];
        self.proxy
            .get_shaderiv(shader_handle, GL_SHADER_SOURCE_LENGTH, &mut shader_source_length);
        let [shader_source_length] = shader_source_length;
        if shader_source_length == 0 {
            return Ok(Some(Utf16String::from_ascii(b"")));
        }

        let mut shader_source = zeroed_gl_string_buffer(shader_source_length);
        self.proxy
            .get_shader_source(shader_handle, shader_source_length, &mut shader_source);
        Ok(Some(utf16_string_from_gl_string(
            &shader_source[..shader_source.len() - 1],
        )))
    }

    pub(crate) fn get_uniform_location(
        &self,
        cx: &mut CallContext,
        program: Ref<WebGLProgram>,
        name: Utf16String,
    ) -> WebIdlResult<Option<Ref<WebGLUniformLocation>>> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(None);
        };

        // "This function returns -1 if name does not correspond to an active uniform variable in program or if name
        //  starts with the reserved prefix "gl_"."
        // WebGL Spec: The return value is null if name does not correspond to an active uniform variable in the passed
        // program.
        let location = self.proxy.get_uniform_location(program_handle, &utf8_bytes(&name));
        if location == -1 {
            return Ok(None);
        }

        Ok(Some(WebGLUniformLocation::create(cx.vm(), location, program)))
    }

    pub(crate) fn get_vertex_attrib_offset(&self, _: &mut CallContext, index: u32, pname: u32) -> WebIdlResult<i64> {
        if pname != GL_VERTEX_ATTRIB_ARRAY_POINTER {
            self.set_error(GL_INVALID_ENUM);
            return Ok(0);
        }

        Ok(self
            .proxy
            .get_vertex_attrib_pointerv_robust_angle(index, GL_VERTEX_ATTRIB_ARRAY_POINTER))
    }

    pub(crate) fn hint(&self, _: &mut CallContext, target: u32, mode: u32) -> WebIdlResult<()> {
        self.proxy.hint(target, mode);
        Ok(())
    }

    pub(crate) fn is_buffer(&self, _: &mut CallContext, buffer: Option<Ref<WebGLBuffer>>) -> WebIdlResult<bool> {
        let Some(buffer) = buffer else {
            return Ok(false);
        };
        let Some(buffer_handle) = self.handle_for_query_of(buffer) else {
            return Ok(false);
        };
        Ok(self.proxy.is_buffer(buffer_handle))
    }

    pub(crate) fn is_enabled(&self, _: &mut CallContext, cap: u32) -> WebIdlResult<bool> {
        Ok(self.proxy.is_enabled(cap))
    }

    pub(crate) fn is_framebuffer(
        &self,
        _: &mut CallContext,
        framebuffer: Option<Ref<WebGLFramebuffer>>,
    ) -> WebIdlResult<bool> {
        let Some(framebuffer) = framebuffer else {
            return Ok(false);
        };
        let Some(framebuffer_handle) = self.handle_for_query_of(framebuffer) else {
            return Ok(false);
        };
        Ok(self.proxy.is_framebuffer(framebuffer_handle))
    }

    pub(crate) fn is_program(&self, _: &mut CallContext, program: Option<Ref<WebGLProgram>>) -> WebIdlResult<bool> {
        let Some(program) = program else {
            return Ok(false);
        };
        let Some(program_handle) = self.handle_for_query_of(program) else {
            return Ok(false);
        };
        Ok(self.proxy.is_program(program_handle))
    }

    pub(crate) fn is_renderbuffer(
        &self,
        _: &mut CallContext,
        renderbuffer: Option<Ref<WebGLRenderbuffer>>,
    ) -> WebIdlResult<bool> {
        let Some(renderbuffer) = renderbuffer else {
            return Ok(false);
        };
        let Some(renderbuffer_handle) = self.handle_for_query_of(renderbuffer) else {
            return Ok(false);
        };
        Ok(self.proxy.is_renderbuffer(renderbuffer_handle))
    }

    pub(crate) fn is_shader(&self, _: &mut CallContext, shader: Option<Ref<WebGLShader>>) -> WebIdlResult<bool> {
        let Some(shader) = shader else {
            return Ok(false);
        };
        let Some(shader_handle) = self.handle_for_query_of(shader) else {
            return Ok(false);
        };
        Ok(self.proxy.is_shader(shader_handle))
    }

    pub(crate) fn is_texture(&self, _: &mut CallContext, texture: Option<Ref<WebGLTexture>>) -> WebIdlResult<bool> {
        let Some(texture) = texture else {
            return Ok(false);
        };
        let Some(texture_handle) = self.handle_for_query_of(texture) else {
            return Ok(false);
        };
        Ok(self.proxy.is_texture(texture_handle))
    }

    pub(crate) fn line_width(&self, _: &mut CallContext, width: f32) -> WebIdlResult<()> {
        self.proxy.line_width(width);
        Ok(())
    }

    pub(crate) fn link_program(&self, _: &mut CallContext, program: Ref<WebGLProgram>) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        self.proxy.link_program(program_handle);
        Ok(())
    }

    pub(crate) fn pixel_storei(&self, _: &mut CallContext, pname: u32, param: i32) -> WebIdlResult<()> {
        let mut unpack_state = self.unpack_state.get();
        match pname {
            GL_UNPACK_ALIGNMENT => {
                let Ok(alignment @ (1 | 2 | 4 | 8)) = usize::try_from(param) else {
                    self.set_error(GL_INVALID_VALUE);
                    return Ok(());
                };
                unpack_state.alignment = alignment;
            }
            GL_UNPACK_ROW_LENGTH => {
                if self.is_webgl2()
                    && let Ok(row_length) = usize::try_from(param)
                {
                    unpack_state.row_length = row_length;
                }
            }
            GL_UNPACK_SKIP_PIXELS => {
                if self.is_webgl2()
                    && let Ok(skip_pixels) = usize::try_from(param)
                {
                    unpack_state.skip_pixels = skip_pixels;
                }
            }
            GL_UNPACK_SKIP_ROWS => {
                if self.is_webgl2()
                    && let Ok(skip_rows) = usize::try_from(param)
                {
                    unpack_state.skip_rows = skip_rows;
                }
            }
            GL_UNPACK_FLIP_Y_WEBGL => {
                self.unpack_flip_y.set(param != 0);
                return Ok(());
            }
            GL_UNPACK_PREMULTIPLY_ALPHA_WEBGL => {
                self.unpack_premultiply_alpha.set(param != 0);
                return Ok(());
            }
            GL_UNPACK_COLORSPACE_CONVERSION_WEBGL => {
                self.unpack_colorspace_conversion.set(param.cast_unsigned());
                return Ok(());
            }
            _ => {}
        }
        self.unpack_state.set(unpack_state);

        self.proxy.pixel_storei(pname, param);
        Ok(())
    }

    pub(crate) fn polygon_offset(&self, _: &mut CallContext, factor: f32, units: f32) -> WebIdlResult<()> {
        self.proxy.polygon_offset(factor, units);
        Ok(())
    }

    pub(crate) fn renderbuffer_storage(
        &self,
        _: &mut CallContext,
        target: u32,
        internalformat: u32,
        width: i32,
        height: i32,
    ) -> WebIdlResult<()> {
        let internalformat = if internalformat == GL_DEPTH_STENCIL {
            GL_DEPTH24_STENCIL8
        } else {
            internalformat
        };

        self.proxy.renderbuffer_storage(target, internalformat, width, height);
        Ok(())
    }

    pub(crate) fn sample_coverage(&self, _: &mut CallContext, value: f32, invert: bool) -> WebIdlResult<()> {
        self.proxy.sample_coverage(value, invert);
        Ok(())
    }

    pub(crate) fn scissor(&self, _: &mut CallContext, x: i32, y: i32, width: i32, height: i32) -> WebIdlResult<()> {
        self.proxy.scissor(x, y, width, height);
        Ok(())
    }

    pub(crate) fn shader_source(
        &self,
        _: &mut CallContext,
        shader: Ref<WebGLShader>,
        source: Utf16String,
    ) -> WebIdlResult<()> {
        let Some(shader_handle) = self.handle_of(shader) else {
            return Ok(());
        };

        self.proxy.shader_source(shader_handle, &utf8_bytes(&source));
        Ok(())
    }

    pub(crate) fn stencil_func(&self, _: &mut CallContext, func: u32, r#ref: i32, mask: u32) -> WebIdlResult<()> {
        self.proxy.stencil_func(func, r#ref, mask);
        Ok(())
    }

    pub(crate) fn stencil_func_separate(
        &self,
        _: &mut CallContext,
        face: u32,
        func: u32,
        r#ref: i32,
        mask: u32,
    ) -> WebIdlResult<()> {
        self.proxy.stencil_func_separate(face, func, r#ref, mask);
        Ok(())
    }

    pub(crate) fn stencil_mask(&self, _: &mut CallContext, mask: u32) -> WebIdlResult<()> {
        self.proxy.stencil_mask(mask);
        Ok(())
    }

    pub(crate) fn stencil_mask_separate(&self, _: &mut CallContext, face: u32, mask: u32) -> WebIdlResult<()> {
        self.proxy.stencil_mask_separate(face, mask);
        Ok(())
    }

    pub(crate) fn stencil_op(&self, _: &mut CallContext, fail: u32, zfail: u32, zpass: u32) -> WebIdlResult<()> {
        self.proxy.stencil_op(fail, zfail, zpass);
        Ok(())
    }

    pub(crate) fn stencil_op_separate(
        &self,
        _: &mut CallContext,
        face: u32,
        fail: u32,
        zfail: u32,
        zpass: u32,
    ) -> WebIdlResult<()> {
        self.proxy.stencil_op_separate(face, fail, zfail, zpass);
        Ok(())
    }

    pub(crate) fn tex_parameterf(&self, _: &mut CallContext, target: u32, pname: u32, param: f32) -> WebIdlResult<()> {
        self.proxy.tex_parameterf(target, pname, param);
        Ok(())
    }

    pub(crate) fn tex_parameteri(&self, _: &mut CallContext, target: u32, pname: u32, param: i32) -> WebIdlResult<()> {
        self.proxy.tex_parameteri(target, pname, param);
        Ok(())
    }

    pub(crate) fn uniform1f(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: f32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform1f(location_handle, x);
        Ok(())
    }

    pub(crate) fn uniform2f(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: f32,
        y: f32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform2f(location_handle, x, y);
        Ok(())
    }

    pub(crate) fn uniform3f(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: f32,
        y: f32,
        z: f32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform3f(location_handle, x, y, z);
        Ok(())
    }

    pub(crate) fn uniform4f(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: f32,
        y: f32,
        z: f32,
        w: f32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform4f(location_handle, x, y, z, w);
        Ok(())
    }

    pub(crate) fn uniform1i(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: i32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform1i(location_handle, x);
        Ok(())
    }

    pub(crate) fn uniform2i(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: i32,
        y: i32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform2i(location_handle, x, y);
        Ok(())
    }

    pub(crate) fn uniform3i(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: i32,
        y: i32,
        z: i32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform3i(location_handle, x, y, z);
        Ok(())
    }

    pub(crate) fn uniform4i(
        &self,
        _: &mut CallContext,
        location: Option<Ref<WebGLUniformLocation>>,
        x: i32,
        y: i32,
        z: i32,
        w: i32,
    ) -> WebIdlResult<()> {
        let Some(location) = location else {
            return Ok(());
        };
        let Some(location_handle) = self.uniform_location_handle(location) else {
            return Ok(());
        };
        self.proxy.uniform4i(location_handle, x, y, z, w);
        Ok(())
    }

    pub(crate) fn use_program(&self, _: &mut CallContext, program: Option<Ref<WebGLProgram>>) -> WebIdlResult<()> {
        let mut program_handle = 0;
        if let Some(program) = program {
            let Some(handle) = self.handle_of(program) else {
                return Ok(());
            };
            program_handle = handle;
        }

        self.proxy.use_program(program_handle);
        self.current_program.set(program);
        Ok(())
    }

    pub(crate) fn validate_program(&self, _: &mut CallContext, program: Ref<WebGLProgram>) -> WebIdlResult<()> {
        let Some(program_handle) = self.handle_of(program) else {
            return Ok(());
        };
        self.proxy.validate_program(program_handle);
        Ok(())
    }

    pub(crate) fn vertex_attrib1f(&self, _: &mut CallContext, index: u32, x: f32) -> WebIdlResult<()> {
        self.proxy.vertex_attrib1f(index, x);
        Ok(())
    }

    pub(crate) fn vertex_attrib2f(&self, _: &mut CallContext, index: u32, x: f32, y: f32) -> WebIdlResult<()> {
        self.proxy.vertex_attrib2f(index, x, y);
        Ok(())
    }

    pub(crate) fn vertex_attrib3f(&self, _: &mut CallContext, index: u32, x: f32, y: f32, z: f32) -> WebIdlResult<()> {
        self.proxy.vertex_attrib3f(index, x, y, z);
        Ok(())
    }

    pub(crate) fn vertex_attrib4f(
        &self,
        _: &mut CallContext,
        index: u32,
        x: f32,
        y: f32,
        z: f32,
        w: f32,
    ) -> WebIdlResult<()> {
        self.proxy.vertex_attrib4f(index, x, y, z, w);
        Ok(())
    }

    pub(crate) fn vertex_attrib1fv(&self, _: &mut CallContext, index: u32, values: Float32List) -> WebIdlResult<()> {
        let Some(values) = copy_of_first_numbers(&values, 1) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.vertex_attrib1fv(index, &values);
        Ok(())
    }

    pub(crate) fn vertex_attrib2fv(&self, _: &mut CallContext, index: u32, values: Float32List) -> WebIdlResult<()> {
        let Some(values) = copy_of_first_numbers(&values, 2) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.vertex_attrib2fv(index, &values);
        Ok(())
    }

    pub(crate) fn vertex_attrib3fv(&self, _: &mut CallContext, index: u32, values: Float32List) -> WebIdlResult<()> {
        let Some(values) = copy_of_first_numbers(&values, 3) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.vertex_attrib3fv(index, &values);
        Ok(())
    }

    pub(crate) fn vertex_attrib4fv(&self, _: &mut CallContext, index: u32, values: Float32List) -> WebIdlResult<()> {
        let Some(values) = copy_of_first_numbers(&values, 4) else {
            self.set_error(GL_INVALID_VALUE);
            return Ok(());
        };
        self.proxy.vertex_attrib4fv(index, &values);
        Ok(())
    }

    pub(crate) fn vertex_attrib_pointer(
        &self,
        _: &mut CallContext,
        index: u32,
        size: i32,
        r#type: u32,
        normalized: bool,
        stride: i32,
        offset: i64,
    ) -> WebIdlResult<()> {
        // If no WebGLBuffer is bound to the ARRAY_BUFFER target and offset is non-zero, an INVALID_OPERATION error will
        // be generated.
        if self.array_buffer_binding.get().is_none() && offset != 0 {
            self.set_error(GL_INVALID_OPERATION);
            return Ok(());
        }

        self.proxy
            .vertex_attrib_pointer(index, size, r#type, normalized, stride, offset);
        Ok(())
    }

    pub(crate) fn viewport(&self, _: &mut CallContext, x: i32, y: i32, width: i32, height: i32) -> WebIdlResult<()> {
        self.proxy.viewport(x, y, width, height);
        Ok(())
    }
}
