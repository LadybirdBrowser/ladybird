/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
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
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGL2RenderingContextOverloads.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebGL {

WebGL2RenderingContextOverloads::WebGL2RenderingContextOverloads(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : WebGL2RenderingContextImpl(realm, move(context))
{
}

void WebGL2RenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    glBufferData(target, size, 0, usage);
}

void WebGL2RenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::BufferSource> src_data, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    auto data = MUST(get_offset_span<u8 const>(*src_data, /* src_offset= */ 0));
    glBufferData(target, data.size(), data.data(), usage);
}

void WebGL2RenderingContextOverloads::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, GC::Root<WebIDL::BufferSource> src_data)
{
    m_context->make_current();

    auto data = MUST(get_offset_span<u8 const>(*src_data, /* src_offset= */ 0));
    glBufferSubData(target, dst_byte_offset, data.size(), data.data());
}

void WebGL2RenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLong usage, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    auto span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, length), GL_INVALID_VALUE);
    glBufferData(target, span.size(), span.data(), usage);
}

void WebGL2RenderingContextOverloads::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    auto span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, length), GL_INVALID_VALUE);
    glBufferSubData(target, dst_byte_offset, span.size(), span.data());
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (pixels) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*pixels, /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, 0, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (pixels) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*pixels, /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type, width, height);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, border, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (src_data) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset), GL_INVALID_OPERATION);
    }

    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type, width, height);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (src_data) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset), GL_INVALID_OPERATION);
    }

    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(internalformat)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexImage2DRobustANGLE(target, level, internalformat, width, height, border, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextOverloads::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextOverloads::uniform1fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    glUniform1fv(location_handle, span.size(), span.data());
}

void WebGL2RenderingContextOverloads::uniform2fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2fv(location_handle, span.size() / 2, span.data());
}

void WebGL2RenderingContextOverloads::uniform3fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3fv(location_handle, span.size() / 3, span.data());
}

void WebGL2RenderingContextOverloads::uniform4fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4fv(location_handle, span.size() / 4, span.data());
}

void WebGL2RenderingContextOverloads::uniform1iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    glUniform1iv(location_handle, span.size(), span.data());
}

void WebGL2RenderingContextOverloads::uniform2iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2iv(location_handle, span.size() / 2, span.data());
}

void WebGL2RenderingContextOverloads::uniform3iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3iv(location_handle, span.size() / 3, span.data());
}

void WebGL2RenderingContextOverloads::uniform4iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4iv(location_handle, span.size() / 4, span.data());
}

void WebGL2RenderingContextOverloads::uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 2 * 2;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextOverloads::uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 3 * 3;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextOverloads::uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 4 * 4;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextOverloads::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (!pixels) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
    glReadPixelsRobustANGLE(x, y, width, height, format, type, span.size(), nullptr, nullptr, nullptr, span.data());
}

void WebGL2RenderingContextOverloads::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height,
    WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();

    if (!m_pixel_pack_buffer_binding) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glReadPixelsRobustANGLE(x, y, width, height, format, type, 0, nullptr, nullptr, nullptr, reinterpret_cast<void*>(offset));
}

}
