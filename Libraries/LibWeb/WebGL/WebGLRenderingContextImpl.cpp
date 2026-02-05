/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_GLEXT_PROTOTYPES 1

#include <GLES3/gl3.h>
extern "C" {
#include <GLES2/gl2ext.h>
#include <GLES2/gl2ext_angle.h>
}

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLQuery.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>
#include <LibWeb/WebGL/WebGLSampler.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>
#include <LibWeb/WebGL/WebGLSync.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLTransformFeedback.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebGL {

WebGLRenderingContextImpl::WebGLRenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : WebGLRenderingContextBase(realm)
    , m_context(move(context))
{
}

void WebGLRenderingContextImpl::active_texture(WebIDL::UnsignedLong texture)
{
    m_context->make_current();
    glActiveTexture(texture);
}

void WebGLRenderingContextImpl::attach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }

    if (program->attached_vertex_shader() == shader || program->attached_fragment_shader() == shader) {
        dbgln("WebGL: Shader is already attached to program");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (shader->type() == GL_VERTEX_SHADER && program->attached_vertex_shader()) {
        dbgln("WebGL: Not attaching vertex shader to program as it already has a vertex shader attached");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (shader->type() == GL_FRAGMENT_SHADER && program->attached_fragment_shader()) {
        dbgln("WebGL: Not attaching fragment shader to program as it already has a fragment shader attached");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glAttachShader(program_handle, shader_handle);

    switch (shader->type()) {
    case GL_VERTEX_SHADER:
        program->set_attached_vertex_shader(shader.ptr());
        break;
    case GL_FRAGMENT_SHADER:
        program->set_attached_fragment_shader(shader.ptr());
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

void WebGLRenderingContextImpl::bind_attrib_location(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index, String name)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    auto name_null_terminated = null_terminated_string(name);
    glBindAttribLocation(program_handle, index, name_null_terminated.data());
}

void WebGLRenderingContextImpl::bind_buffer(WebIDL::UnsignedLong target, GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    GLuint buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        buffer_handle = handle_or_error.release_value();

        if (!buffer->is_compatible_with(target)) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
    }

    if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        switch (target) {
        case GL_ARRAY_BUFFER:
            m_array_buffer_binding = buffer;
            break;
        case GL_COPY_READ_BUFFER:
            m_copy_read_buffer_binding = buffer;
            break;
        case GL_COPY_WRITE_BUFFER:
            m_copy_write_buffer_binding = buffer;
            break;
        case GL_ELEMENT_ARRAY_BUFFER:
            m_element_array_buffer_binding = buffer;
            break;
        case GL_PIXEL_PACK_BUFFER:
            m_pixel_pack_buffer_binding = buffer;
            break;
        case GL_PIXEL_UNPACK_BUFFER:
            m_pixel_unpack_buffer_binding = buffer;
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER:
            m_transform_feedback_buffer_binding = buffer;
            break;
        case GL_UNIFORM_BUFFER:
            m_uniform_buffer_binding = buffer;
            break;
        default:
            dbgln("Unknown WebGL buffer object binding target for storing current binding: 0x{:04x}", target);
            set_error(GL_INVALID_ENUM);
            return;
        }
    } else {
        switch (target) {
        case GL_ELEMENT_ARRAY_BUFFER:
            m_element_array_buffer_binding = buffer;
            break;
        case GL_ARRAY_BUFFER:
            m_array_buffer_binding = buffer;
            break;
        default:
            dbgln("Unknown WebGL buffer object binding target for storing current binding: 0x{:04x}", target);
            set_error(GL_INVALID_ENUM);
            return;
        }
    }

    glBindBuffer(target, buffer_handle);
}

void WebGLRenderingContextImpl::bind_framebuffer(WebIDL::UnsignedLong target, GC::Root<WebGLFramebuffer> framebuffer)
{
    m_context->make_current();

    GLuint framebuffer_handle = 0;
    if (framebuffer) {
        auto handle_or_error = framebuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        framebuffer_handle = handle_or_error.release_value();
    }

    glBindFramebuffer(target, framebuffer ? framebuffer_handle : m_context->default_framebuffer());
    m_framebuffer_binding = framebuffer;
}

void WebGLRenderingContextImpl::bind_renderbuffer(WebIDL::UnsignedLong target, GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    GLuint renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }

    glBindRenderbuffer(target, renderbuffer ? renderbuffer_handle : m_context->default_renderbuffer());
    m_renderbuffer_binding = renderbuffer;
}

void WebGLRenderingContextImpl::bind_texture(WebIDL::UnsignedLong target, GC::Root<WebGLTexture> texture)
{
    m_context->make_current();

    GLuint texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        texture_handle = handle_or_error.release_value();
    }

    switch (target) {
    case GL_TEXTURE_2D:
        m_texture_binding_2d = texture;
        break;
    case GL_TEXTURE_CUBE_MAP:
        m_texture_binding_cube_map = texture;
        break;

    case GL_TEXTURE_2D_ARRAY:
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            m_texture_binding_2d_array = texture;
            break;
        }

        set_error(GL_INVALID_ENUM);
        return;
    case GL_TEXTURE_3D:
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            m_texture_binding_3d = texture;
            break;
        }

        set_error(GL_INVALID_ENUM);
        return;

    default:
        dbgln("Unknown WebGL texture target for storing current binding: 0x{:04x}", target);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glBindTexture(target, texture_handle);
}

void WebGLRenderingContextImpl::blend_color(float red, float green, float blue, float alpha)
{
    m_context->make_current();
    glBlendColor(red, green, blue, alpha);
}

void WebGLRenderingContextImpl::blend_equation(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glBlendEquation(mode);
}

void WebGLRenderingContextImpl::blend_equation_separate(WebIDL::UnsignedLong mode_rgb, WebIDL::UnsignedLong mode_alpha)
{
    m_context->make_current();
    glBlendEquationSeparate(mode_rgb, mode_alpha);
}

void WebGLRenderingContextImpl::blend_func(WebIDL::UnsignedLong sfactor, WebIDL::UnsignedLong dfactor)
{
    m_context->make_current();
    glBlendFunc(sfactor, dfactor);
}

void WebGLRenderingContextImpl::blend_func_separate(WebIDL::UnsignedLong src_rgb, WebIDL::UnsignedLong dst_rgb, WebIDL::UnsignedLong src_alpha, WebIDL::UnsignedLong dst_alpha)
{
    m_context->make_current();
    glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
}

WebIDL::UnsignedLong WebGLRenderingContextImpl::check_framebuffer_status(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    return glCheckFramebufferStatus(target);
}

void WebGLRenderingContextImpl::clear(WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glClear(mask);
}

void WebGLRenderingContextImpl::clear_color(float red, float green, float blue, float alpha)
{
    m_context->make_current();
    glClearColor(red, green, blue, alpha);
}

void WebGLRenderingContextImpl::clear_depth(float depth)
{
    m_context->make_current();
    glClearDepthf(depth);
}

void WebGLRenderingContextImpl::clear_stencil(WebIDL::Long s)
{
    m_context->make_current();
    glClearStencil(s);
}

void WebGLRenderingContextImpl::color_mask(bool red, bool green, bool blue, bool alpha)
{
    m_context->make_current();
    glColorMask(red, green, blue, alpha);
}

void WebGLRenderingContextImpl::compile_shader(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }
    glCompileShader(shader_handle);
}

void WebGLRenderingContextImpl::copy_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border)
{
    m_context->make_current();
    glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void WebGLRenderingContextImpl::copy_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

GC::Root<WebGLBuffer> WebGLRenderingContextImpl::create_buffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenBuffers(1, &handle);
    return WebGLBuffer::create(realm(), *this, handle);
}

GC::Root<WebGLFramebuffer> WebGLRenderingContextImpl::create_framebuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenFramebuffers(1, &handle);
    return WebGLFramebuffer::create(realm(), *this, handle);
}

GC::Root<WebGLProgram> WebGLRenderingContextImpl::create_program()
{
    m_context->make_current();
    return WebGLProgram::create(realm(), *this, glCreateProgram());
}

GC::Root<WebGLRenderbuffer> WebGLRenderingContextImpl::create_renderbuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenRenderbuffers(1, &handle);
    return WebGLRenderbuffer::create(realm(), *this, handle);
}

GC::Root<WebGLShader> WebGLRenderingContextImpl::create_shader(WebIDL::UnsignedLong type)
{
    m_context->make_current();

    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        dbgln("Unknown WebGL shader type: 0x{:04x}", type);
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }

    GLuint handle = glCreateShader(type);
    return WebGLShader::create(realm(), *this, handle, type);
}

GC::Root<WebGLTexture> WebGLRenderingContextImpl::create_texture()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenTextures(1, &handle);
    return WebGLTexture::create(realm(), *this, handle);
}

void WebGLRenderingContextImpl::cull_face(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glCullFace(mode);
}

void WebGLRenderingContextImpl::delete_buffer(GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    GLuint buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        buffer_handle = handle_or_error.release_value();
    }

    glDeleteBuffers(1, &buffer_handle);
}

void WebGLRenderingContextImpl::delete_framebuffer(GC::Root<WebGLFramebuffer> framebuffer)
{
    m_context->make_current();

    GLuint framebuffer_handle = 0;
    if (framebuffer) {
        auto handle_or_error = framebuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        framebuffer_handle = handle_or_error.release_value();
    }

    glDeleteFramebuffers(1, &framebuffer_handle);
}

void WebGLRenderingContextImpl::delete_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }
    glDeleteProgram(program_handle);
    if (m_current_program == program)
        m_current_program = nullptr;
}

void WebGLRenderingContextImpl::delete_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    GLuint renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }

    glDeleteRenderbuffers(1, &renderbuffer_handle);
}

void WebGLRenderingContextImpl::delete_shader(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }
    glDeleteShader(shader_handle);
}

void WebGLRenderingContextImpl::delete_texture(GC::Root<WebGLTexture> texture)
{
    m_context->make_current();

    GLuint texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        texture_handle = handle_or_error.release_value();
    }

    glDeleteTextures(1, &texture_handle);

    if (m_texture_binding_2d == texture)
        m_texture_binding_2d = nullptr;
    if (m_texture_binding_cube_map == texture)
        m_texture_binding_cube_map = nullptr;
    if (m_texture_binding_2d_array == texture)
        m_texture_binding_2d_array = nullptr;
    if (m_texture_binding_3d == texture)
        m_texture_binding_3d = nullptr;
}

void WebGLRenderingContextImpl::depth_func(WebIDL::UnsignedLong func)
{
    m_context->make_current();
    glDepthFunc(func);
}

void WebGLRenderingContextImpl::depth_mask(bool flag)
{
    m_context->make_current();
    glDepthMask(flag);
}

void WebGLRenderingContextImpl::depth_range(float z_near, float z_far)
{
    m_context->make_current();
    glDepthRangef(z_near, z_far);
}

void WebGLRenderingContextImpl::detach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }

    glDetachShader(program_handle, shader_handle);

    switch (shader->type()) {
    case GL_VERTEX_SHADER:
        program->set_attached_vertex_shader(nullptr);
        break;
    case GL_FRAGMENT_SHADER:
        program->set_attached_fragment_shader(nullptr);
        break;
    }
}

void WebGLRenderingContextImpl::disable(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    glDisable(cap);
}

void WebGLRenderingContextImpl::disable_vertex_attrib_array(WebIDL::UnsignedLong index)
{
    m_context->make_current();
    glDisableVertexAttribArray(index);
}

void WebGLRenderingContextImpl::draw_arrays(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glDrawArrays(mode, first, count);
}

void WebGLRenderingContextImpl::draw_elements(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    glDrawElements(mode, count, type, reinterpret_cast<void*>(offset));
    needs_to_present();
}

void WebGLRenderingContextImpl::enable(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    glEnable(cap);
}

void WebGLRenderingContextImpl::enable_vertex_attrib_array(WebIDL::UnsignedLong index)
{
    m_context->make_current();
    glEnableVertexAttribArray(index);
}

void WebGLRenderingContextImpl::finish()
{
    m_context->make_current();
    glFinish();
}

void WebGLRenderingContextImpl::flush()
{
    m_context->make_current();
    glFlush();
}

void WebGLRenderingContextImpl::framebuffer_renderbuffer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong renderbuffertarget, GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    auto renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer_handle);
}

void WebGLRenderingContextImpl::framebuffer_texture2d(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong textarget, GC::Root<WebGLTexture> texture, WebIDL::Long level)
{
    m_context->make_current();

    auto texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        texture_handle = handle_or_error.release_value();
    }
    glFramebufferTexture2D(target, attachment, textarget, texture_handle, level);
}

void WebGLRenderingContextImpl::front_face(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glFrontFace(mode);
}

void WebGLRenderingContextImpl::generate_mipmap(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    glGenerateMipmap(target);
}

GC::Root<WebGLActiveInfo> WebGLRenderingContextImpl::get_active_attrib(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveAttrib(program_handle, index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(realm(), String::from_utf8_without_validation(readonly_bytes), type, size);
}

GC::Root<WebGLActiveInfo> WebGLRenderingContextImpl::get_active_uniform(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveUniform(program_handle, index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(realm(), String::from_utf8_without_validation(readonly_bytes), type, size);
}

Optional<Vector<GC::Root<WebGLShader>>> WebGLRenderingContextImpl::get_attached_shaders(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return OptionalNone {};
        }
        program_handle = handle_or_error.release_value();
    }

    (void)program_handle;

    Vector<GC::Root<WebGLShader>> result;

    if (program->attached_vertex_shader())
        result.append(GC::make_root(*program->attached_vertex_shader()));

    if (program->attached_fragment_shader())
        result.append(GC::make_root(*program->attached_fragment_shader()));

    return result;
}

WebIDL::Long WebGLRenderingContextImpl::get_attrib_location(GC::Root<WebGLProgram> program, String name)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return -1;
        }
        program_handle = handle_or_error.release_value();
    }

    auto name_null_terminated = null_terminated_string(name);
    return glGetAttribLocation(program_handle, name_null_terminated.data());
}

JS::Value WebGLRenderingContextImpl::get_buffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    m_context->make_current();
    switch (pname) {
    case GL_BUFFER_SIZE: {
        GLint result { 0 };
        glGetBufferParameterivRobustANGLE(target, GL_BUFFER_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }

    case GL_BUFFER_USAGE: {
        GLint result { 0 };
        glGetBufferParameterivRobustANGLE(target, GL_BUFFER_USAGE, 1, nullptr, &result);
        return JS::Value(result);
    }

    default:
        dbgln("Unknown WebGL buffer parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

WebIDL::ExceptionOr<JS::Value> WebGLRenderingContextImpl::get_parameter(WebIDL::UnsignedLong pname)
{
    m_context->make_current();
    switch (pname) {
    case GL_ACTIVE_TEXTURE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_ALIASED_LINE_WIDTH_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_ALIASED_LINE_WIDTH_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 2, array_buffer);
    }
    case GL_ALIASED_POINT_SIZE_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_ALIASED_POINT_SIZE_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 2, array_buffer);
    }
    case GL_ALPHA_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_ALPHA_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_ARRAY_BUFFER_BINDING: {
        if (!m_array_buffer_binding)
            return JS::js_null();
        return JS::Value(m_array_buffer_binding);
    }
    case GL_BLEND: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_BLEND, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_BLEND_COLOR: {
        Array<GLfloat, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_BLEND_COLOR, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 4, array_buffer);
    }
    case GL_BLEND_DST_ALPHA: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_DST_ALPHA, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_DST_RGB: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_DST_RGB, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_EQUATION_ALPHA: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_EQUATION_ALPHA, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_EQUATION_RGB: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_EQUATION_RGB, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_SRC_ALPHA: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_SRC_ALPHA, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_SRC_RGB: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_SRC_RGB, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLUE_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLUE_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_COLOR_CLEAR_VALUE: {
        Array<GLfloat, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_COLOR_CLEAR_VALUE, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 4, array_buffer);
    }
    case GL_COLOR_WRITEMASK: {
        Array<GLboolean, 4> result;
        result.fill(0);
        glGetBooleanvRobustANGLE(GL_COLOR_WRITEMASK, 4, nullptr, result.data());

        auto sequence = TRY(JS::Array::create(realm(), 4));
        for (int i = 0; i < 4; i++) {
            TRY(sequence->create_data_property(JS::PropertyKey(i), JS::Value(static_cast<WebIDL::Boolean>(result[i]))));
        }

        return JS::Value(sequence);
    }
    case GL_CULL_FACE: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_CULL_FACE, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_CULL_FACE_MODE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_CULL_FACE_MODE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_CURRENT_PROGRAM: {
        if (!m_current_program)
            return JS::js_null();
        return JS::Value(m_current_program);
    }
    case GL_DEPTH_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_DEPTH_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_DEPTH_CLEAR_VALUE: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_DEPTH_CLEAR_VALUE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_DEPTH_FUNC: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_DEPTH_FUNC, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_DEPTH_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_DEPTH_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 2, array_buffer);
    }
    case GL_DEPTH_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_DEPTH_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_DEPTH_WRITEMASK: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_DEPTH_WRITEMASK, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_DITHER: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_DITHER, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: {
        if (!m_element_array_buffer_binding)
            return JS::js_null();
        return JS::Value(m_element_array_buffer_binding);
    }
    case GL_FRAMEBUFFER_BINDING: {
        if (!m_framebuffer_binding)
            return JS::js_null();
        return JS::Value(m_framebuffer_binding);
    }
    case GL_FRONT_FACE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_FRONT_FACE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_GENERATE_MIPMAP_HINT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_GENERATE_MIPMAP_HINT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_GREEN_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_GREEN_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_IMPLEMENTATION_COLOR_READ_FORMAT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_IMPLEMENTATION_COLOR_READ_TYPE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_LINE_WIDTH: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_LINE_WIDTH, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_CUBE_MAP_TEXTURE_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_RENDERBUFFER_SIZE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_RENDERBUFFER_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_TEXTURE_IMAGE_UNITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_TEXTURE_IMAGE_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_TEXTURE_SIZE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_TEXTURE_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VARYING_VECTORS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VARYING_VECTORS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VERTEX_ATTRIBS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VERTEX_ATTRIBS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VERTEX_UNIFORM_VECTORS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VERTEX_UNIFORM_VECTORS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VIEWPORT_DIMS: {
        Array<GLint, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_MAX_VIEWPORT_DIMS, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Int32Array::create(realm(), 2, array_buffer);
    }
    case GL_PACK_ALIGNMENT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_PACK_ALIGNMENT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_POLYGON_OFFSET_FACTOR: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_POLYGON_OFFSET_FACTOR, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_POLYGON_OFFSET_FILL: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_POLYGON_OFFSET_FILL, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_POLYGON_OFFSET_UNITS: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_POLYGON_OFFSET_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_RED_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_RED_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_RENDERBUFFER_BINDING: {
        if (!m_renderbuffer_binding)
            return JS::js_null();
        return JS::Value(m_renderbuffer_binding);
    }
    case GL_RENDERER: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_SAMPLE_ALPHA_TO_COVERAGE: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SAMPLE_ALPHA_TO_COVERAGE, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SAMPLE_BUFFERS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_SAMPLE_BUFFERS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SAMPLE_COVERAGE: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SAMPLE_COVERAGE, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SAMPLE_COVERAGE_INVERT: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SAMPLE_COVERAGE_INVERT, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SAMPLE_COVERAGE_VALUE: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_SAMPLE_COVERAGE_VALUE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SAMPLES: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_SAMPLES, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SCISSOR_BOX: {
        Array<GLint, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_SCISSOR_BOX, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Int32Array::create(realm(), 4, array_buffer);
    }
    case GL_SCISSOR_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SCISSOR_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SHADING_LANGUAGE_VERSION: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_STENCIL_BACK_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_FUNC: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_FUNC, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_PASS_DEPTH_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_PASS_DEPTH_PASS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_PASS_DEPTH_PASS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_REF: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_REF, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_VALUE_MASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_VALUE_MASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_WRITEMASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_WRITEMASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_CLEAR_VALUE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_CLEAR_VALUE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_FUNC: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_FUNC, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_PASS_DEPTH_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_PASS_DEPTH_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_PASS_DEPTH_PASS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_PASS_DEPTH_PASS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_REF: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_REF, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_STENCIL_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_STENCIL_VALUE_MASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_VALUE_MASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_WRITEMASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_WRITEMASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SUBPIXEL_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_SUBPIXEL_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_TEXTURE_BINDING_2D: {
        if (!m_texture_binding_2d)
            return JS::js_null();
        return JS::Value(m_texture_binding_2d);
    }
    case GL_TEXTURE_BINDING_CUBE_MAP: {
        if (!m_texture_binding_cube_map)
            return JS::js_null();
        return JS::Value(m_texture_binding_cube_map);
    }
    case GL_UNPACK_ALIGNMENT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_UNPACK_ALIGNMENT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_VENDOR: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_VENDOR));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_VERSION: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_VERSION));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_VIEWPORT: {
        Array<GLint, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Int32Array::create(realm(), 4, array_buffer);
    }

    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: { // NOTE: This has the same value as GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES
        if (oes_standard_derivatives_extension_enabled() || m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_MAX_COLOR_ATTACHMENTS: { // NOTE: This has the same value as MAX_COLOR_ATTACHMENTS_WEBGL
        if (webgl_draw_buffers_extension_enabled() || m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_COLOR_ATTACHMENTS, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_MAX_DRAW_BUFFERS: {
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) { // FIXME: Allow this code path for MAX_DRAW_BUFFERS_WEBGL
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_DRAW_BUFFERS, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (ext_texture_filter_anisotropic_extension_enabled()) {
            GLfloat result { 0.0f };
            glGetFloatvRobustANGLE(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }

    case COMPRESSED_TEXTURE_FORMATS: {
        auto formats = enabled_compressed_texture_formats();
        auto byte_buffer = MUST(ByteBuffer::copy(formats.data(), formats.reinterpret<u8 const>().size()));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Uint32Array::create(realm(), formats.size(), array_buffer);
    }
    case UNPACK_FLIP_Y_WEBGL:
        return JS::Value(m_unpack_flip_y);
    case UNPACK_PREMULTIPLY_ALPHA_WEBGL:
        return JS::Value(m_unpack_premultiply_alpha);
    case UNPACK_COLORSPACE_CONVERSION_WEBGL:
        return JS::Value(m_unpack_colorspace_conversion);
    }

    if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        switch (pname) {
        case GL_COPY_READ_BUFFER_BINDING: {
            if (!m_copy_read_buffer_binding)
                return JS::js_null();
            return JS::Value(m_copy_read_buffer_binding);
        }
        case GL_COPY_WRITE_BUFFER_BINDING: {
            if (!m_copy_write_buffer_binding)
                return JS::js_null();
            return JS::Value(m_copy_write_buffer_binding);
        }
        case GL_MAX_SAMPLES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_SAMPLES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_3D_TEXTURE_SIZE: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_3D_TEXTURE_SIZE, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_ARRAY_TEXTURE_LAYERS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_ARRAY_TEXTURE_LAYERS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VERTEX_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_UNIFORM_BLOCK_SIZE: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_UNIFORM_BLOCK_SIZE, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_UNIFORM_BUFFER_BINDINGS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_UNIFORM_BUFFER_BINDINGS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VERTEX_UNIFORM_BLOCKS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VERTEX_UNIFORM_BLOCKS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_INPUT_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_COMBINED_UNIFORM_BLOCKS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_COMBINED_UNIFORM_BLOCKS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_ELEMENT_INDEX: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_ELEMENT_INDEX, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VARYING_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VARYING_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_ELEMENTS_INDICES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_ELEMENTS_INDICES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_ELEMENTS_VERTICES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_ELEMENTS_VERTICES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TEXTURE_LOD_BIAS: {
            GLfloat result { 0.0f };
            glGetFloatvRobustANGLE(GL_MAX_TEXTURE_LOD_BIAS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MIN_PROGRAM_TEXEL_OFFSET: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MIN_PROGRAM_TEXEL_OFFSET, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_PROGRAM_TEXEL_OFFSET: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_PROGRAM_TEXEL_OFFSET, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VERTEX_OUTPUT_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_SERVER_WAIT_TIMEOUT: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_SERVER_WAIT_TIMEOUT, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_PACK_ROW_LENGTH: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_PACK_ROW_LENGTH, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_PACK_SKIP_ROWS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_PACK_SKIP_ROWS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_PACK_SKIP_PIXELS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_PACK_SKIP_PIXELS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_PIXEL_PACK_BUFFER_BINDING: {
            if (!m_pixel_pack_buffer_binding)
                return JS::js_null();
            return JS::Value(m_pixel_pack_buffer_binding);
        }
        case GL_PIXEL_UNPACK_BUFFER_BINDING: {
            if (!m_pixel_unpack_buffer_binding)
                return JS::js_null();
            return JS::Value(m_pixel_unpack_buffer_binding);
        }
        case GL_TEXTURE_BINDING_2D_ARRAY: {
            if (!m_texture_binding_2d_array)
                return JS::js_null();
            return JS::Value(m_texture_binding_2d_array);
        }
        case GL_TRANSFORM_FEEDBACK_ACTIVE: {
            GLboolean result { GL_FALSE };
            glGetBooleanvRobustANGLE(GL_TRANSFORM_FEEDBACK_ACTIVE, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_TRANSFORM_FEEDBACK_BINDING: {
            if (!m_transform_feedback_binding)
                return JS::js_null();
            return JS::Value(m_transform_feedback_binding);
        }
        case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: {
            if (!m_transform_feedback_buffer_binding)
                return JS::js_null();
            return JS::Value(m_transform_feedback_buffer_binding);
        }
        case GL_TRANSFORM_FEEDBACK_PAUSED: {
            GLboolean result { GL_FALSE };
            glGetBooleanvRobustANGLE(GL_TRANSFORM_FEEDBACK_PAUSED, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_RASTERIZER_DISCARD: {
            GLboolean result { GL_FALSE };
            glGetBooleanvRobustANGLE(GL_RASTERIZER_DISCARD, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_SAMPLER_BINDING: {
            GLint handle { 0 };
            glGetIntegervRobustANGLE(GL_SAMPLER_BINDING, 1, nullptr, &handle);
            return WebGLSampler::create(realm(), *this, handle);
        }
        case GL_UNIFORM_BUFFER_BINDING: {
            if (!m_uniform_buffer_binding)
                return JS::js_null();
            return JS::Value(m_uniform_buffer_binding);
        }
        case GL_UNPACK_IMAGE_HEIGHT: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_IMAGE_HEIGHT, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_ROW_LENGTH: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_ROW_LENGTH, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_SKIP_IMAGES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_SKIP_IMAGES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_SKIP_PIXELS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_SKIP_PIXELS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_SKIP_ROWS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_SKIP_ROWS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_VERTEX_ARRAY_BINDING: { // FIXME: Allow this for VERTEX_ARRAY_BINDING_OES
            if (!m_current_vertex_array)
                return JS::js_null();
            return JS::Value(m_current_vertex_array);
        }
        case MAX_CLIENT_WAIT_TIMEOUT_WEBGL:
            // FIXME: Make this an actual limit
            return JS::js_infinity();
        }
    }

    dbgln("Unknown WebGL parameter name: {:x}", pname);
    set_error(GL_INVALID_ENUM);
    return JS::js_null();
}

WebIDL::UnsignedLong WebGLRenderingContextImpl::get_error()
{
    m_context->make_current();
    return get_error_value();
}

JS::Value WebGLRenderingContextImpl::get_program_parameter(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return JS::js_null();
        }
        program_handle = handle_or_error.release_value();
    }

    GLint result = 0;
    glGetProgramivRobustANGLE(program_handle, pname, 1, nullptr, &result);
    switch (pname) {
    case GL_ATTACHED_SHADERS:
    case GL_ACTIVE_ATTRIBUTES:
    case GL_ACTIVE_UNIFORMS:
        return JS::Value(result);

    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
    case GL_TRANSFORM_FEEDBACK_VARYINGS:
    case GL_ACTIVE_UNIFORM_BLOCKS:
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2)
            return JS::Value(result);

        set_error(GL_INVALID_ENUM);
        return JS::js_null();

    case GL_DELETE_STATUS:
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:
        return JS::Value(result == GL_TRUE);
    default:
        dbgln("Unknown WebGL program parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

Optional<String> WebGLRenderingContextImpl::get_program_info_log(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    GLint info_log_length = 0;
    glGetProgramiv(program_handle, GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetProgramInfoLog(program_handle, info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
}

JS::Value WebGLRenderingContextImpl::get_renderbuffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    switch (pname) {
    case GL_RENDERBUFFER_WIDTH:
    case GL_RENDERBUFFER_HEIGHT:
    case GL_RENDERBUFFER_INTERNAL_FORMAT:
    case GL_RENDERBUFFER_RED_SIZE:
    case GL_RENDERBUFFER_GREEN_SIZE:
    case GL_RENDERBUFFER_BLUE_SIZE:
    case GL_RENDERBUFFER_ALPHA_SIZE:
    case GL_RENDERBUFFER_DEPTH_SIZE:
    case GL_RENDERBUFFER_SAMPLES:
    case GL_RENDERBUFFER_STENCIL_SIZE: {
        GLint result = 0;
        glGetRenderbufferParameterivRobustANGLE(target, pname, 1, nullptr, &result);
        return JS::Value(result);
    }
    default:
        // If pname is not in the table above, generates an INVALID_ENUM error.
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

JS::Value WebGLRenderingContextImpl::get_shader_parameter(GC::Root<WebGLShader> shader, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return JS::js_null();
        }
        shader_handle = handle_or_error.release_value();
    }

    GLint result = 0;
    glGetShaderivRobustANGLE(shader_handle, pname, 1, nullptr, &result);
    switch (pname) {
    case GL_SHADER_TYPE:
        return JS::Value(result);
    case GL_DELETE_STATUS:
    case GL_COMPILE_STATUS:
        return JS::Value(result == GL_TRUE);
    default:
        dbgln("Unknown WebGL shader parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

GC::Root<WebGLShaderPrecisionFormat> WebGLRenderingContextImpl::get_shader_precision_format(WebIDL::UnsignedLong shadertype, WebIDL::UnsignedLong precisiontype)
{
    m_context->make_current();

    GLint range[2];
    GLint precision;
    glGetShaderPrecisionFormat(shadertype, precisiontype, range, &precision);
    return WebGLShaderPrecisionFormat::create(realm(), range[0], range[1], precision);
}

Optional<String> WebGLRenderingContextImpl::get_shader_info_log(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        shader_handle = handle_or_error.release_value();
    }

    GLint info_log_length = 0;
    glGetShaderiv(shader_handle, GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetShaderInfoLog(shader_handle, info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
}

Optional<String> WebGLRenderingContextImpl::get_shader_source(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        shader_handle = handle_or_error.release_value();
    }

    GLint shader_source_length = 0;
    glGetShaderiv(shader_handle, GL_SHADER_SOURCE_LENGTH, &shader_source_length);
    if (!shader_source_length)
        return String {};

    auto shader_source = MUST(ByteBuffer::create_uninitialized(shader_source_length));
    glGetShaderSource(shader_handle, shader_source_length, nullptr, reinterpret_cast<GLchar*>(shader_source.data()));
    return String::from_utf8_without_validation(ReadonlyBytes { shader_source.data(), static_cast<size_t>(shader_source_length - 1) });
}

JS::Value WebGLRenderingContextImpl::get_tex_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    switch (pname) {
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T: {
        GLint result { 0 };
        glGetTexParameterivRobustANGLE(target, pname, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (ext_texture_filter_anisotropic_extension_enabled()) {
            GLint result { 0 };
            glGetTexParameterivRobustANGLE(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    }

    if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        switch (pname) {
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_COMPARE_FUNC:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_IMMUTABLE_LEVELS:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_WRAP_R: {
            GLint result { 0 };
            glGetTexParameterivRobustANGLE(target, pname, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_TEXTURE_IMMUTABLE_FORMAT: {
            GLint result { 0 };
            glGetTexParameterivRobustANGLE(target, GL_TEXTURE_IMMUTABLE_FORMAT, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_MIN_LOD: {
            GLfloat result { 0.0f };
            glGetTexParameterfvRobustANGLE(target, GL_TEXTURE_IMMUTABLE_FORMAT, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        }
    }

    set_error(GL_INVALID_ENUM);
    return JS::js_null();
}

JS::Value WebGLRenderingContextImpl::get_uniform(GC::Root<WebGLProgram>, GC::Root<WebGLUniformLocation>)
{
    dbgln("FIXME: Implement get_uniform");
    return JS::Value(0);
}

GC::Root<WebGLUniformLocation> WebGLRenderingContextImpl::get_uniform_location(GC::Root<WebGLProgram> program, String name)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    auto name_null_terminated = null_terminated_string(name);

    // "This function returns -1 if name does not correspond to an active uniform variable in program or if name starts
    //  with the reserved prefix "gl_"."
    // WebGL Spec: The return value is null if name does not correspond to an active uniform variable in the passed program.
    auto location = glGetUniformLocation(program_handle, name_null_terminated.data());
    if (location == -1)
        return nullptr;

    return WebGLUniformLocation::create(realm(), location, program.ptr());
}

JS::Value WebGLRenderingContextImpl::get_vertex_attrib(WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname)
{
    switch (pname) {
    case GL_CURRENT_VERTEX_ATTRIB: {
        Array<GLfloat, 4> result;
        result.fill(0);
        glGetVertexAttribfvRobustANGLE(index, GL_CURRENT_VERTEX_ATTRIB, result.size(), nullptr, result.data());

        auto byte_buffer = MUST(ByteBuffer::copy(result.span().reinterpret<u8>()));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), result.size(), array_buffer);
    }
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: {
        GLint handle { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 1, nullptr, &handle);
        return WebGLBuffer::create(realm(), *this, handle);
    }
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: { // NOTE: This has the same value as GL_VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE
        if (angle_instanced_arrays_extension_enabled() || m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: {
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_INTEGER, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &result);
        return JS::Value(result);
    }
    default:
        dbgln("Unknown WebGL vertex attrib name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

WebIDL::LongLong WebGLRenderingContextImpl::get_vertex_attrib_offset(WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname)
{
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        set_error(GL_INVALID_ENUM);
        return 0;
    }

    GLintptr result { 0 };
    glGetVertexAttribPointervRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, 1, nullptr, reinterpret_cast<void**>(&result));
    return result;
}

void WebGLRenderingContextImpl::hint(WebIDL::UnsignedLong target, WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glHint(target, mode);
}

bool WebGLRenderingContextImpl::is_buffer(GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    auto buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        buffer_handle = handle_or_error.release_value();
    }
    return glIsBuffer(buffer_handle);
}

bool WebGLRenderingContextImpl::is_enabled(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    return glIsEnabled(cap);
}

bool WebGLRenderingContextImpl::is_framebuffer(GC::Root<WebGLFramebuffer> framebuffer)
{
    m_context->make_current();

    auto framebuffer_handle = 0;
    if (framebuffer) {
        auto handle_or_error = framebuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        framebuffer_handle = handle_or_error.release_value();
    }
    return glIsFramebuffer(framebuffer_handle);
}

bool WebGLRenderingContextImpl::is_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        program_handle = handle_or_error.release_value();
    }
    return glIsProgram(program_handle);
}

bool WebGLRenderingContextImpl::is_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    auto renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }
    return glIsRenderbuffer(renderbuffer_handle);
}

bool WebGLRenderingContextImpl::is_shader(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        shader_handle = handle_or_error.release_value();
    }
    return glIsShader(shader_handle);
}

bool WebGLRenderingContextImpl::is_texture(GC::Root<WebGLTexture> texture)
{
    m_context->make_current();

    auto texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        texture_handle = handle_or_error.release_value();
    }
    return glIsTexture(texture_handle);
}

void WebGLRenderingContextImpl::line_width(float width)
{
    m_context->make_current();
    glLineWidth(width);
}

void WebGLRenderingContextImpl::link_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }
    glLinkProgram(program_handle);
}

void WebGLRenderingContextImpl::pixel_storei(WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();

    switch (pname) {
    case UNPACK_FLIP_Y_WEBGL:
        m_unpack_flip_y = param != GL_FALSE;
        return;
    case UNPACK_PREMULTIPLY_ALPHA_WEBGL:
        m_unpack_premultiply_alpha = param != GL_FALSE;
        return;
    case UNPACK_COLORSPACE_CONVERSION_WEBGL:
        m_unpack_colorspace_conversion = param;
        return;
    }

    glPixelStorei(pname, param);
}

void WebGLRenderingContextImpl::polygon_offset(float factor, float units)
{
    m_context->make_current();
    glPolygonOffset(factor, units);
}

void WebGLRenderingContextImpl::renderbuffer_storage(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();

    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;

    glRenderbufferStorage(target, internalformat, width, height);
}

void WebGLRenderingContextImpl::sample_coverage(float value, bool invert)
{
    m_context->make_current();
    glSampleCoverage(value, invert);
}

void WebGLRenderingContextImpl::scissor(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glScissor(x, y, width, height);
}

void WebGLRenderingContextImpl::shader_source(GC::Root<WebGLShader> shader, String source)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }

    Vector<GLchar*> strings;
    auto string = null_terminated_string(source);
    strings.append(string.data());
    Vector<GLint> length;
    length.append(source.bytes().size());
    glShaderSource(shader_handle, 1, strings.data(), length.data());
}

void WebGLRenderingContextImpl::stencil_func(WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilFunc(func, ref, mask);
}

void WebGLRenderingContextImpl::stencil_func_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilFuncSeparate(face, func, ref, mask);
}

void WebGLRenderingContextImpl::stencil_mask(WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilMask(mask);
}

void WebGLRenderingContextImpl::stencil_mask_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilMaskSeparate(face, mask);
}

void WebGLRenderingContextImpl::stencil_op(WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass)
{
    m_context->make_current();
    glStencilOp(fail, zfail, zpass);
}

void WebGLRenderingContextImpl::stencil_op_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass)
{
    m_context->make_current();
    glStencilOpSeparate(face, fail, zfail, zpass);
}

void WebGLRenderingContextImpl::tex_parameterf(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, float param)
{
    m_context->make_current();
    glTexParameterf(target, pname, param);
}

void WebGLRenderingContextImpl::tex_parameteri(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();
    glTexParameteri(target, pname, param);
}

void WebGLRenderingContextImpl::uniform1f(GC::Root<WebGLUniformLocation> location, float x)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform1f(location_handle, x);
}

void WebGLRenderingContextImpl::uniform2f(GC::Root<WebGLUniformLocation> location, float x, float y)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform2f(location_handle, x, y);
}

void WebGLRenderingContextImpl::uniform3f(GC::Root<WebGLUniformLocation> location, float x, float y, float z)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform3f(location_handle, x, y, z);
}

void WebGLRenderingContextImpl::uniform4f(GC::Root<WebGLUniformLocation> location, float x, float y, float z, float w)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform4f(location_handle, x, y, z, w);
}

void WebGLRenderingContextImpl::uniform1i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform1i(location_handle, x);
}

void WebGLRenderingContextImpl::uniform2i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform2i(location_handle, x, y);
}

void WebGLRenderingContextImpl::uniform3i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform3i(location_handle, x, y, z);
}

void WebGLRenderingContextImpl::uniform4i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform4i(location_handle, x, y, z, w);
}

void WebGLRenderingContextImpl::use_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    glUseProgram(program_handle);
    m_current_program = program;
}

void WebGLRenderingContextImpl::validate_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }
    glValidateProgram(program_handle);
}

void WebGLRenderingContextImpl::vertex_attrib1f(WebIDL::UnsignedLong index, float x)
{
    m_context->make_current();
    glVertexAttrib1f(index, x);
}

void WebGLRenderingContextImpl::vertex_attrib2f(WebIDL::UnsignedLong index, float x, float y)
{
    m_context->make_current();
    glVertexAttrib2f(index, x, y);
}

void WebGLRenderingContextImpl::vertex_attrib3f(WebIDL::UnsignedLong index, float x, float y, float z)
{
    m_context->make_current();
    glVertexAttrib3f(index, x, y, z);
}

void WebGLRenderingContextImpl::vertex_attrib4f(WebIDL::UnsignedLong index, float x, float y, float z, float w)
{
    m_context->make_current();
    glVertexAttrib4f(index, x, y, z, w);
}

void WebGLRenderingContextImpl::vertex_attrib1fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 1) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib1fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib2fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 2) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib2fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib3fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 3) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib3fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib4fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib4fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, bool normalized, WebIDL::Long stride, WebIDL::LongLong offset)
{
    m_context->make_current();

    // If no WebGLBuffer is bound to the ARRAY_BUFFER target and offset is non-zero, an INVALID_OPERATION error will be generated.
    if (!m_array_buffer_binding && offset != 0) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<void*>(offset));
}

void WebGLRenderingContextImpl::viewport(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glViewport(x, y, width, height);
}

void WebGLRenderingContextImpl::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_array_buffer_binding);
    visitor.visit(m_element_array_buffer_binding);
    visitor.visit(m_current_program);
    visitor.visit(m_framebuffer_binding);
    visitor.visit(m_renderbuffer_binding);
    visitor.visit(m_texture_binding_2d);
    visitor.visit(m_texture_binding_cube_map);

    visitor.visit(m_uniform_buffer_binding);
    visitor.visit(m_copy_read_buffer_binding);
    visitor.visit(m_copy_write_buffer_binding);
    visitor.visit(m_transform_feedback_buffer_binding);
    visitor.visit(m_texture_binding_2d_array);
    visitor.visit(m_texture_binding_3d);
    visitor.visit(m_transform_feedback_binding);
    visitor.visit(m_pixel_pack_buffer_binding);
    visitor.visit(m_pixel_unpack_buffer_binding);
    visitor.visit(m_current_vertex_array);
    visitor.visit(m_any_samples_passed);
    visitor.visit(m_any_samples_passed_conservative);
    visitor.visit(m_transform_feedback_primitives_written);
}

}
