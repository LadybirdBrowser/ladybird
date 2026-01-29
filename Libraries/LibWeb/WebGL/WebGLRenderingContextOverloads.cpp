/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextOverloads.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>

namespace Web::WebGL {

WebGLRenderingContextOverloads::WebGLRenderingContextOverloads(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : WebGLRenderingContextImpl(realm, move(context))
{
}

void WebGLRenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    glBufferData(target, size, 0, usage);
}

void WebGLRenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::BufferSource> data, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    glBufferData(target, span.size(), span.data(), usage);
}

void WebGLRenderingContextOverloads::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong offset, GC::Root<WebIDL::BufferSource> data)
{
    m_context->make_current();

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    glBufferSubData(target, offset, span.size(), span.data());
}

void WebGLRenderingContextOverloads::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> data)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(internalformat)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    glCompressedTexImage2DRobustANGLE(target, level, internalformat, width, height, border, span.size(), span.size(), span.data());
}

void WebGLRenderingContextOverloads::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> data)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    glCompressedTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, span.size(), span.size(), span.data());
}

void WebGLRenderingContextOverloads::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (!pixels) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
    glReadPixelsRobustANGLE(x, y, width, height, format, type, span.size(), nullptr, nullptr, nullptr, span.data());
}

void WebGLRenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (pixels) {
        auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
        glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, span.size(), span.data());
        return;
    }

    Checked<size_t> bytes = 0;
    if (type == GL_UNSIGNED_SHORT_5_6_5 && format != GL_RGB) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if ((type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) && format != GL_RGBA) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    switch (format) {
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA: {
        if (type != GL_UNSIGNED_BYTE) {
            set_error(GL_INVALID_ENUM);
            return;
        }

        bytes = format == GL_LUMINANCE_ALPHA ? 2 : 1;
        break;
    }
    case GL_RGB:
    case GL_RGBA: {
        switch (type) {
        case GL_UNSIGNED_BYTE:
            bytes = format == GL_RGB ? 3 : 4;
            break;
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
        case GL_UNSIGNED_SHORT_5_6_5:
            bytes = 2;
            break;
        default:
            set_error(GL_INVALID_ENUM);
            return;
        }

        break;
    }
    default:
        set_error(GL_INVALID_ENUM);
        return;
    }

    bytes *= width;
    bytes *= height;

    if (bytes.has_overflow()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    auto byte_buffer = MUST(ByteBuffer::create_zeroed(bytes.value_unchecked()));
    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, byte_buffer.size(), byte_buffer.data());
}

void WebGLRenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, 0, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGLRenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, span.size(), span.data());
}

void WebGLRenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGLRenderingContextOverloads::uniform1fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    glUniform1fv(location_handle, span.size(), span.data());
}

void WebGLRenderingContextOverloads::uniform2fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2fv(location_handle, span.size() / 2, span.data());
}

void WebGLRenderingContextOverloads::uniform3fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3fv(location_handle, span.size() / 3, span.data());
}

void WebGLRenderingContextOverloads::uniform4fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4fv(location_handle, span.size() / 4, span.data());
}

void WebGLRenderingContextOverloads::uniform1iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    glUniform1iv(location_handle, span.size(), span.data());
}

void WebGLRenderingContextOverloads::uniform2iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2iv(location_handle, span.size() / 2, span.data());
}

void WebGLRenderingContextOverloads::uniform3iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3iv(location_handle, span.size() / 3, span.data());
}

void WebGLRenderingContextOverloads::uniform4iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4iv(location_handle, span.size() / 4, span.data());
}

void WebGLRenderingContextOverloads::uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 2 * 2;
    auto span = MUST(span_from_float32_list(value, /* src_offset= */ 0));
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGLRenderingContextOverloads::uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 3 * 3;
    auto span = MUST(span_from_float32_list(value, /* src_offset= */ 0));
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGLRenderingContextOverloads::uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 4 * 4;
    auto span = MUST(span_from_float32_list(value, /* src_offset= */ 0));
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

}
