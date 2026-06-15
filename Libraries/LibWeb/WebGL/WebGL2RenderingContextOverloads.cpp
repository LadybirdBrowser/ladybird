/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES3/gl3.h>
extern "C" {
#include <GLES2/gl2ext.h>
#include <GLES2/gl2ext_angle.h>
}

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/WebGL2RenderingContextOverloads.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace Web::WebGL {

WebGL2RenderingContextOverloads::WebGL2RenderingContextOverloads(JS::Realm& realm, NonnullOwnPtr<WebGLContextProxy> context)
    : WebGL2RenderingContextImpl(realm, move(context))
{
}

void WebGL2RenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    m_context->buffer_data(target, size, 0, usage);
}

void WebGL2RenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, WebIDL::NullableBufferSourceVariant src_data, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    // https://registry.khronos.org/webgl/specs/latest/1.0/#5.14.5
    // If the passed data is null then an INVALID_VALUE error is generated.
    if (src_data.has<Empty>()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto data = MUST(get_offset_span<u8 const>(src_data.downcast<WebIDL::BufferSourceVariant>(), /* src_offset= */ 0));
    m_context->buffer_data(target, static_cast<GLsizeiptr>(data.size()), data.data(), usage);
}

void WebGL2RenderingContextOverloads::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, WebIDL::BufferSource src_data)
{
    m_context->make_current();

    auto data = MUST(get_offset_span<u8 const>(src_data, /* src_offset= */ 0));
    m_context->buffer_sub_data(target, dst_byte_offset, data.size(), data.data());
}

void WebGL2RenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLong usage, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    auto span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset, length), GL_INVALID_VALUE);
    m_context->buffer_data(target, span.size(), span.data(), usage);
}

void WebGL2RenderingContextOverloads::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    auto span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset, length), GL_INVALID_VALUE);
    m_context->buffer_sub_data(target, dst_byte_offset, span.size(), span.data());
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant pixels)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (!pixels.has<Empty>()) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(pixels.downcast<WebIDL::ArrayBufferViewVariant>(), /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    m_context->tex_image2d_robust_angle(target, level, internalformat, width, height, border, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_source_frame = read_texture_image_source(source, format, type);
    if (!maybe_source_frame.has_value())
        return;
    auto source_frame = maybe_source_frame.release_value();
    m_context->tex_image2d_from_bitmap(target, level, internalformat, format, type, move(source_frame.frame), OptionalNone {}, source_frame.flip_y, source_frame.premultiply_alpha);
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant pixels)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (!pixels.has<Empty>()) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(pixels.downcast<WebIDL::ArrayBufferViewVariant>(), /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    m_context->tex_sub_image2d_robust_angle(target, level, xoffset, yoffset, width, height, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_source_frame = read_texture_image_source(source, format, type);
    if (!maybe_source_frame.has_value())
        return;
    auto source_frame = maybe_source_frame.release_value();
    m_context->tex_sub_image2d_from_bitmap(target, level, xoffset, yoffset, format, type, move(source_frame.frame), OptionalNone {}, source_frame.flip_y, source_frame.premultiply_alpha);
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    // https://registry.khronos.org/OpenGL-Refpages/es3.0/html/glTexImage2D.xhtml
    // border: This value must be 0.
    if (border != 0) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto maybe_source_frame = read_texture_image_source(source, format, type);
    if (!maybe_source_frame.has_value())
        return;
    auto source_frame = maybe_source_frame.release_value();
    m_context->tex_image2d_from_bitmap(target, level, internalformat, format, type, move(source_frame.frame), Gfx::IntSize { width, height }, source_frame.flip_y, source_frame.premultiply_alpha);
}

void WebGL2RenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    auto pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset), GL_INVALID_OPERATION);

    m_context->tex_image2d_robust_angle(target, level, internalformat, width, height, border, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_source_frame = read_texture_image_source(source, format, type);
    if (!maybe_source_frame.has_value())
        return;
    auto source_frame = maybe_source_frame.release_value();
    m_context->tex_sub_image2d_from_bitmap(target, level, xoffset, yoffset, format, type, move(source_frame.frame), Gfx::IntSize { width, height }, source_frame.flip_y, source_frame.premultiply_alpha);
}

void WebGL2RenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    auto pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset), GL_INVALID_OPERATION);

    m_context->tex_sub_image2d_robust_angle(target, level, xoffset, yoffset, width, height, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextOverloads::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(internalformat)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    m_context->compressed_tex_image2d_robust_angle(target, level, internalformat, width, height, border, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextOverloads::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    m_context->compressed_tex_sub_image2d_robust_angle(target, level, xoffset, yoffset, width, height, format, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextOverloads::uniform1fv(GC::Ptr<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    m_context->uniform1fv(location_handle, span.size(), span.data());
}

void WebGL2RenderingContextOverloads::uniform2fv(GC::Ptr<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform2fv(location_handle, span.size() / 2, span.data());
}

void WebGL2RenderingContextOverloads::uniform3fv(GC::Ptr<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform3fv(location_handle, span.size() / 3, span.data());
}

void WebGL2RenderingContextOverloads::uniform4fv(GC::Ptr<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform4fv(location_handle, span.size() / 4, span.data());
}

void WebGL2RenderingContextOverloads::uniform1iv(GC::Ptr<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    m_context->uniform1iv(location_handle, span.size(), span.data());
}

void WebGL2RenderingContextOverloads::uniform2iv(GC::Ptr<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform2iv(location_handle, span.size() / 2, span.data());
}

void WebGL2RenderingContextOverloads::uniform3iv(GC::Ptr<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform3iv(location_handle, span.size() / 3, span.data());
}

void WebGL2RenderingContextOverloads::uniform4iv(GC::Ptr<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform4iv(location_handle, span.size() / 4, span.data());
}

void WebGL2RenderingContextOverloads::uniform_matrix2fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextOverloads::uniform_matrix3fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextOverloads::uniform_matrix4fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextOverloads::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant pixels)
{
    m_context->make_current();

    if (pixels.has<Empty>()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto span = MUST(get_offset_span<u8>(pixels.downcast<WebIDL::ArrayBufferViewVariant>(), /* src_offset= */ 0));
    m_context->read_pixels_robust_angle(x, y, width, height, format, type, span.size(), nullptr, nullptr, nullptr, span.data());
}

void WebGL2RenderingContextOverloads::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height,
    WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();

    if (!m_pixel_pack_buffer_binding) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    m_context->read_pixels_into_pixel_pack_buffer(x, y, width, height, format, type, offset);
}

}
