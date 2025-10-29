/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
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
#include <LibWeb/WebGL/WebGL2RenderingContextImpl.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLQuery.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLSampler.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>
#include <LibWeb/WebGL/WebGLSync.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLTransformFeedback.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>
#include <LibWeb/WebIDL/Buffers.h>

#define SET_ERROR_VALUE_IF_ERROR(expression, error_value) \
    ({                                                    \
        auto maybe_error = expression;                    \
        if (maybe_error.is_error()) [[unlikely]] {        \
            set_error(error_value);                       \
            return;                                       \
        }                                                 \
        maybe_error.release_value();                      \
    })

namespace Web::WebGL {

static Vector<GLchar> null_terminated_string(StringView string)
{
    Vector<GLchar> result;
    for (auto c : string.bytes())
        result.append(c);
    result.append('\0');
    return result;
}

WebGL2RenderingContextImpl::WebGL2RenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : m_realm(realm)
    , m_context(move(context))
{
}

void WebGL2RenderingContextImpl::copy_buffer_sub_data(WebIDL::UnsignedLong read_target, WebIDL::UnsignedLong write_target, WebIDL::LongLong read_offset, WebIDL::LongLong write_offset, WebIDL::LongLong size)
{
    m_context->make_current();
    glCopyBufferSubData(read_target, write_target, read_offset, write_offset, size);
}

// https://registry.khronos.org/webgl/specs/latest/2.0/#3.7.3
void WebGL2RenderingContextImpl::get_buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong src_byte_offset,
    GC::Root<WebIDL::ArrayBufferView> dst_buffer, WebIDL::UnsignedLongLong dst_offset, WebIDL::UnsignedLong length)
{
    // If dstBuffer is a DataView, let elementSize be 1; otherwise, let elementSize be dstBuffer.BYTES_PER_ELEMENT.
    size_t element_size = dst_buffer->element_size();

    // If length is 0:
    size_t copy_length;
    if (length == 0) {
        // If dstBuffer is a DataView, let copyLength be dstBuffer.byteLength - dstOffset; the typed elements in the
        // text below are bytes. Otherwise, let copyLength be dstBuffer.length - dstOffset.
        copy_length = dst_buffer->byte_length() / element_size - dst_offset;
    }

    // Otherwise, let copyLength be length.
    else {
        copy_length = length;
    }

    // If copyLength is 0, no data is written to dstBuffer, but this does not cause a GL error to be generated.
    if (copy_length == 0)
        return;

    // If dstOffset is greater than dstBuffer.length (or dstBuffer.byteLength in the case of DataView), generates an
    // INVALID_VALUE error.
    size_t dst_offset_in_bytes = dst_offset * element_size;
    if (dst_offset_in_bytes > dst_buffer->byte_length()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    // If dstOffset + copyLength is greater than dstBuffer.length (or dstBuffer.byteLength in the case of DataView),
    // generates an INVALID_VALUE error.
    size_t copy_bytes = copy_length * element_size;
    if (dst_offset_in_bytes + copy_bytes > dst_buffer->byte_length()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    // If copyLength is greater than zero, copy copyLength typed elements (each of size elementSize) from buf into
    // dstBuffer, reading buf starting at byte index srcByteOffset and writing into dstBuffer starting at element
    // index dstOffset.
    auto* buffer_data = glMapBufferRange(target, src_byte_offset, copy_bytes, GL_MAP_READ_BIT);
    if (!buffer_data)
        return;

    dst_buffer->write({ buffer_data, copy_bytes }, dst_offset_in_bytes);

    glUnmapBuffer(target);
}

void WebGL2RenderingContextImpl::blit_framebuffer(WebIDL::Long src_x0, WebIDL::Long src_y0, WebIDL::Long src_x1, WebIDL::Long src_y1, WebIDL::Long dst_x0, WebIDL::Long dst_y0, WebIDL::Long dst_x1, WebIDL::Long dst_y1, WebIDL::UnsignedLong mask, WebIDL::UnsignedLong filter)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

void WebGL2RenderingContextImpl::framebuffer_texture_layer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, GC::Root<WebGLTexture> texture, WebIDL::Long level, WebIDL::Long layer)
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

    glFramebufferTextureLayer(target, attachment, texture_handle, level, layer);
}

void WebGL2RenderingContextImpl::invalidate_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    glInvalidateFramebuffer(target, attachments.size(), attachments.data());
    needs_to_present();
}

void WebGL2RenderingContextImpl::invalidate_sub_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    glInvalidateSubFramebuffer(target, attachments.size(), attachments.data(), x, y, width, height);
    needs_to_present();
}

void WebGL2RenderingContextImpl::read_buffer(WebIDL::UnsignedLong src)
{
    m_context->make_current();
    glReadBuffer(src);
}

JS::Value WebGL2RenderingContextImpl::get_internalformat_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    switch (pname) {
    case GL_SAMPLES: {
        GLint num_sample_counts { 0 };
        glGetInternalformativRobustANGLE(target, internalformat, GL_NUM_SAMPLE_COUNTS, 1, nullptr, &num_sample_counts);
        size_t buffer_size = num_sample_counts * sizeof(GLint);
        auto samples_buffer = MUST(ByteBuffer::create_zeroed(buffer_size));
        glGetInternalformativRobustANGLE(target, internalformat, GL_SAMPLES, buffer_size, nullptr, reinterpret_cast<GLint*>(samples_buffer.data()));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(samples_buffer));
        return JS::Int32Array::create(m_realm, num_sample_counts, array_buffer);
    }
    default:
        dbgln("Unknown WebGL internal format parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

void WebGL2RenderingContextImpl::renderbuffer_storage_multisample(WebIDL::UnsignedLong target, WebIDL::Long samples, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glRenderbufferStorageMultisample(target, samples, internalformat, width, height);
}

void WebGL2RenderingContextImpl::tex_storage2d(WebIDL::UnsignedLong target, WebIDL::Long levels, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();

    glTexStorage2D(target, levels, internalformat, width, height);
}

void WebGL2RenderingContextImpl::tex_storage3d(WebIDL::UnsignedLong target, WebIDL::Long levels, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth)
{
    m_context->make_current();
    glTexStorage3D(target, levels, internalformat, width, height, depth);
}

void WebGL2RenderingContextImpl::tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data)
{
    m_context->make_current();

    ReadonlyBytes src_data_span;
    if (src_data) {
        src_data_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    glTexImage3DRobustANGLE(target, level, internalformat, width, height, depth, border, format, type, src_data_span.size(), src_data_span.data());
}

void WebGL2RenderingContextImpl::tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes src_data_span;
    if (src_data) {
        src_data_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset), GL_INVALID_OPERATION);
    }

    glTexImage3DRobustANGLE(target, level, internalformat, width, height, depth, border, format, type, src_data_span.size(), src_data_span.data());
}

void WebGL2RenderingContextImpl::tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes src_data_span;
    if (src_data) {
        src_data_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset), GL_INVALID_OPERATION);
    }

    glTexSubImage3DRobustANGLE(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, src_data_span.size(), src_data_span.data());
}

void WebGL2RenderingContextImpl::uniform1ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0)
{
    m_context->make_current();
    glUniform1ui(location ? location->handle() : 0, v0);
}

void WebGL2RenderingContextImpl::uniform2ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1)
{
    m_context->make_current();
    glUniform2ui(location ? location->handle() : 0, v0, v1);
}

void WebGL2RenderingContextImpl::uniform3ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2)
{
    m_context->make_current();
    glUniform3ui(location ? location->handle() : 0, v0, v1, v2);
}

void WebGL2RenderingContextImpl::uniform4ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2, WebIDL::UnsignedLong v3)
{
    m_context->make_current();
    glUniform4ui(location ? location->handle() : 0, v0, v1, v2, v3);
}

void WebGL2RenderingContextImpl::uniform1uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    glUniform1uiv(location->handle(), span.size(), span.data());
}

void WebGL2RenderingContextImpl::uniform2uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2uiv(location->handle(), span.size() / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3uiv(location->handle(), span.size() / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4uiv(location->handle(), span.size() / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3x2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 3 * 2;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3x2fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4x2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 4 * 2;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4x2fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2x3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 2 * 3;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2x3fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4x3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 4 * 3;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4x3fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2x4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 2 * 4;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2x4fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3x4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 3 * 4;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3x4fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_i4i(WebIDL::UnsignedLong index, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w)
{
    m_context->make_current();
    glVertexAttribI4i(index, x, y, z, w);
}

void WebGL2RenderingContextImpl::vertex_attrib_i4iv(WebIDL::UnsignedLong index, Int32List values)
{
    m_context->make_current();
    auto span = MUST(span_from_int32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttribI4iv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_i4ui(WebIDL::UnsignedLong index, WebIDL::UnsignedLong x, WebIDL::UnsignedLong y, WebIDL::UnsignedLong z, WebIDL::UnsignedLong w)
{
    m_context->make_current();
    glVertexAttribI4ui(index, x, y, z, w);
}

void WebGL2RenderingContextImpl::vertex_attrib_i4uiv(WebIDL::UnsignedLong index, Uint32List values)
{
    m_context->make_current();
    auto span = MUST(span_from_uint32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttribI4uiv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_i_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, WebIDL::Long stride, WebIDL::LongLong offset)
{
    m_context->make_current();

    glVertexAttribIPointer(index, size, type, stride, reinterpret_cast<void*>(offset));
}

void WebGL2RenderingContextImpl::vertex_attrib_divisor(WebIDL::UnsignedLong index, WebIDL::UnsignedLong divisor)
{
    m_context->make_current();
    glVertexAttribDivisor(index, divisor);
}

void WebGL2RenderingContextImpl::draw_arrays_instanced(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count, WebIDL::Long instance_count)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glDrawArraysInstanced(mode, first, count, instance_count);
}

void WebGL2RenderingContextImpl::draw_elements_instanced(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset, WebIDL::Long instance_count)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    glDrawElementsInstanced(mode, count, type, reinterpret_cast<void*>(offset), instance_count);
    needs_to_present();
}

void WebGL2RenderingContextImpl::draw_range_elements(WebIDL::UnsignedLong mode, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glDrawRangeElements(mode, start, end, count, type, reinterpret_cast<void*>(offset));
}

void WebGL2RenderingContextImpl::draw_buffers(Vector<WebIDL::UnsignedLong> buffers)
{
    m_context->make_current();

    glDrawBuffers(buffers.size(), buffers.data());
}

void WebGL2RenderingContextImpl::clear_bufferfv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Float32List values, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset), GL_INVALID_VALUE);

    switch (buffer) {
    case GL_COLOR:
        if (span.size() < 4) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (span.size() < 1) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    glClearBufferfv(buffer, drawbuffer, span.data());
    needs_to_present();
}

void WebGL2RenderingContextImpl::clear_bufferiv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Int32List values, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset), GL_INVALID_VALUE);

    switch (buffer) {
    case GL_COLOR:
        if (span.size() < 4) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (span.size() < 1) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    glClearBufferiv(buffer, drawbuffer, span.data());
    needs_to_present();
}

void WebGL2RenderingContextImpl::clear_bufferuiv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Uint32List values, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset), GL_INVALID_VALUE);
    switch (buffer) {
    case GL_COLOR:
        if (span.size() < 4) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (span.size() < 1) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    glClearBufferuiv(buffer, drawbuffer, span.data());
    needs_to_present();
}

void WebGL2RenderingContextImpl::clear_bufferfi(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, float depth, WebIDL::Long stencil)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glClearBufferfi(buffer, drawbuffer, depth, stencil);
}

GC::Root<WebGLQuery> WebGL2RenderingContextImpl::create_query()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenQueries(1, &handle);
    return WebGLQuery::create(m_realm, *this, handle);
}

void WebGL2RenderingContextImpl::delete_query(GC::Root<WebGLQuery> query)
{
    m_context->make_current();

    GLuint query_handle = 0;
    if (query) {
        auto handle_or_error = query->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        query_handle = handle_or_error.release_value();
    }

    glDeleteQueries(1, &query_handle);
}

void WebGL2RenderingContextImpl::begin_query(WebIDL::UnsignedLong target, GC::Root<WebGLQuery> query)
{
    m_context->make_current();

    GLuint query_handle = 0;
    if (query) {
        auto handle_or_error = query->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        query_handle = handle_or_error.release_value();
    }

    glBeginQuery(target, query_handle);
}

void WebGL2RenderingContextImpl::end_query(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    glEndQuery(target);
}

JS::Value WebGL2RenderingContextImpl::get_query_parameter(GC::Root<WebGLQuery> query, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLuint query_handle = 0;
    if (query) {
        auto handle_or_error = query->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return JS::js_null();
        }
        query_handle = handle_or_error.release_value();
    }

    GLuint result { 0 };
    glGetQueryObjectuivRobustANGLE(query_handle, pname, 1, nullptr, &result);

    switch (pname) {
    case GL_QUERY_RESULT:
        return JS::Value(result);
    case GL_QUERY_RESULT_AVAILABLE:
        return JS::Value(result == GL_TRUE);
    default:
        return JS::js_null();
    }
}

GC::Root<WebGLSampler> WebGL2RenderingContextImpl::create_sampler()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenSamplers(1, &handle);
    return WebGLSampler::create(m_realm, *this, handle);
}

void WebGL2RenderingContextImpl::delete_sampler(GC::Root<WebGLSampler> sampler)
{
    m_context->make_current();

    GLuint sampler_handle = 0;
    if (sampler) {
        auto handle_or_error = sampler->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        sampler_handle = handle_or_error.release_value();
    }

    glDeleteSamplers(1, &sampler_handle);
}

void WebGL2RenderingContextImpl::bind_sampler(WebIDL::UnsignedLong unit, GC::Root<WebGLSampler> sampler)
{
    m_context->make_current();

    auto sampler_handle = 0;
    if (sampler) {
        auto handle_or_error = sampler->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        sampler_handle = handle_or_error.release_value();
    }
    glBindSampler(unit, sampler_handle);
}

void WebGL2RenderingContextImpl::sampler_parameteri(GC::Root<WebGLSampler> sampler, WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();

    GLuint sampler_handle = 0;
    if (sampler) {
        auto handle_or_error = sampler->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        sampler_handle = handle_or_error.release_value();
    }

    switch (pname) {
    case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_MIN_LOD:
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (ext_texture_filter_anisotropic_extension_enabled())
            break;

        set_error(GL_INVALID_ENUM);
        return;
    }
    default:
        dbgln("Unknown WebGL sampler parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glSamplerParameteri(sampler_handle, pname, param);
}

void WebGL2RenderingContextImpl::sampler_parameterf(GC::Root<WebGLSampler> sampler, WebIDL::UnsignedLong pname, float param)
{
    m_context->make_current();

    GLuint sampler_handle = 0;
    if (sampler) {
        auto handle_or_error = sampler->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        sampler_handle = handle_or_error.release_value();
    }

    switch (pname) {
    case GL_TEXTURE_COMPARE_FUNC:
    case GL_TEXTURE_COMPARE_MODE:
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MAX_LOD:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_MIN_LOD:
    case GL_TEXTURE_WRAP_R:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        break;
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (ext_texture_filter_anisotropic_extension_enabled())
            break;

        set_error(GL_INVALID_ENUM);
        return;
    }
    default:
        dbgln("Unknown WebGL sampler parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glSamplerParameterf(sampler_handle, pname, param);
}

GC::Root<WebGLSync> WebGL2RenderingContextImpl::fence_sync(WebIDL::UnsignedLong condition, WebIDL::UnsignedLong flags)
{
    m_context->make_current();

    GLsync handle = glFenceSync(condition, flags);
    return WebGLSync::create(m_realm, *this, handle);
}

void WebGL2RenderingContextImpl::delete_sync(GC::Root<WebGLSync> sync)
{
    m_context->make_current();

    GLsync sync_handle = nullptr;
    if (sync) {
        auto handle_or_error = sync->sync_handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        sync_handle = static_cast<GLsync>(handle_or_error.release_value());
    }

    glDeleteSync(sync_handle);
}

WebIDL::UnsignedLong WebGL2RenderingContextImpl::client_wait_sync(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout)
{
    m_context->make_current();

    GLsync sync_handle = nullptr;
    if (sync) {
        auto handle_or_error = sync->sync_handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return GL_WAIT_FAILED;
        }
        sync_handle = static_cast<GLsync>(handle_or_error.release_value());
    }

    return glClientWaitSync(sync_handle, flags, timeout);
}

void WebGL2RenderingContextImpl::wait_sync(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout)
{
    m_context->make_current();

    GLsync sync_handle = nullptr;
    if (sync) {
        auto handle_or_error = sync->sync_handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        sync_handle = static_cast<GLsync>(handle_or_error.release_value());
    }

    glWaitSync(sync_handle, flags, timeout);
}

JS::Value WebGL2RenderingContextImpl::get_sync_parameter(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLsync sync_handle = nullptr;
    if (sync) {
        auto handle_or_error = sync->sync_handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return JS::js_null();
        }
        sync_handle = static_cast<GLsync>(handle_or_error.release_value());
    }

    GLint result = 0;
    glGetSynciv(sync_handle, pname, 1, nullptr, &result);
    return JS::Value(result);
}

GC::Root<WebGLTransformFeedback> WebGL2RenderingContextImpl::create_transform_feedback()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenTransformFeedbacks(1, &handle);
    return WebGLTransformFeedback::create(m_realm, *this, handle);
}

void WebGL2RenderingContextImpl::delete_transform_feedback(GC::Root<WebGLTransformFeedback> transform_feedback)
{
    m_context->make_current();

    GLuint transform_feedback_handle = 0;
    if (transform_feedback) {
        auto handle_or_error = transform_feedback->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        transform_feedback_handle = handle_or_error.release_value();
    }

    glDeleteTransformFeedbacks(1, &transform_feedback_handle);
}

void WebGL2RenderingContextImpl::bind_transform_feedback(WebIDL::UnsignedLong target, GC::Root<WebGLTransformFeedback> transform_feedback)
{
    m_context->make_current();

    GLuint transform_feedback_handle = 0;
    if (transform_feedback) {
        auto handle_or_error = transform_feedback->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        transform_feedback_handle = handle_or_error.release_value();
    }

    glBindTransformFeedback(target, transform_feedback_handle);
}

void WebGL2RenderingContextImpl::begin_transform_feedback(WebIDL::UnsignedLong primitive_mode)
{
    m_context->make_current();
    glBeginTransformFeedback(primitive_mode);
}

void WebGL2RenderingContextImpl::end_transform_feedback()
{
    m_context->make_current();
    glEndTransformFeedback();
}

void WebGL2RenderingContextImpl::transform_feedback_varyings(GC::Root<WebGLProgram> program, Vector<String> const& varyings, WebIDL::UnsignedLong buffer_mode)
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

    Vector<Vector<GLchar>> varying_strings;
    varying_strings.ensure_capacity(varyings.size());
    for (auto const& varying : varyings) {
        varying_strings.unchecked_append(null_terminated_string(varying));
    }

    Vector<GLchar const*> varying_strings_characters;
    varying_strings.ensure_capacity(varying_strings.size());
    for (auto const& varying_string : varying_strings) {
        varying_strings_characters.append(varying_string.data());
    }

    glTransformFeedbackVaryings(program_handle, varying_strings_characters.size(), varying_strings_characters.data(), buffer_mode);
}

void WebGL2RenderingContextImpl::pause_transform_feedback()
{
    m_context->make_current();
    glPauseTransformFeedback();
}

void WebGL2RenderingContextImpl::resume_transform_feedback()
{
    m_context->make_current();
    glResumeTransformFeedback();
}

void WebGL2RenderingContextImpl::bind_buffer_base(WebIDL::UnsignedLong target, WebIDL::UnsignedLong index, GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    auto buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        buffer_handle = handle_or_error.release_value();
    }
    glBindBufferBase(target, index, buffer_handle);
}

void WebGL2RenderingContextImpl::bind_buffer_range(WebIDL::UnsignedLong target, WebIDL::UnsignedLong index, GC::Root<WebGLBuffer> buffer, WebIDL::LongLong offset, WebIDL::LongLong size)
{
    m_context->make_current();

    auto buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        buffer_handle = handle_or_error.release_value();
    }
    glBindBufferRange(target, index, buffer_handle, offset, size);
}

JS::Value WebGL2RenderingContextImpl::get_active_uniforms(GC::Root<WebGLProgram> program, Vector<WebIDL::UnsignedLong> uniform_indices, WebIDL::UnsignedLong pname)
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

    auto params = MUST(ByteBuffer::create_zeroed(uniform_indices.size() * sizeof(GLint)));
    Span<GLint> params_span(reinterpret_cast<GLint*>(params.data()), uniform_indices.size());
    glGetActiveUniformsiv(program_handle, uniform_indices.size(), uniform_indices.data(), pname, params_span.data());

    Vector<JS::Value> params_as_values;
    params_as_values.ensure_capacity(params.size());
    for (GLint param : params_span) {
        switch (pname) {
        case GL_UNIFORM_TYPE:
            params_as_values.unchecked_append(JS::Value(static_cast<GLenum>(param)));
            break;
        case GL_UNIFORM_SIZE:
            params_as_values.unchecked_append(JS::Value(static_cast<GLuint>(param)));
            break;
        case GL_UNIFORM_BLOCK_INDEX:
        case GL_UNIFORM_OFFSET:
        case GL_UNIFORM_ARRAY_STRIDE:
        case GL_UNIFORM_MATRIX_STRIDE:
            params_as_values.unchecked_append(JS::Value(param));
            break;
        case GL_UNIFORM_IS_ROW_MAJOR:
            params_as_values.unchecked_append(JS::Value(param == GL_TRUE));
            break;
        default:
            dbgln("Unknown WebGL uniform parameter name in getActiveUniforms: 0x{:04x}", pname);
            set_error(GL_INVALID_ENUM);
            return JS::js_null();
        }
    }

    return JS::Array::create_from(m_realm, params_as_values);
}

WebIDL::UnsignedLong WebGL2RenderingContextImpl::get_uniform_block_index(GC::Root<WebGLProgram> program, String uniform_block_name)
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

    auto uniform_block_name_null_terminated = null_terminated_string(uniform_block_name);
    return glGetUniformBlockIndex(program_handle, uniform_block_name_null_terminated.data());
}

JS::Value WebGL2RenderingContextImpl::get_active_uniform_block_parameter(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong pname)
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

    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING:
    case GL_UNIFORM_BLOCK_DATA_SIZE:
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
        GLint result = 0;
        glGetActiveUniformBlockivRobustANGLE(program_handle, uniform_block_index, pname, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
        GLint num_active_uniforms = 0;
        glGetActiveUniformBlockivRobustANGLE(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, sizeof(GLint), nullptr, &num_active_uniforms);
        size_t buffer_size = num_active_uniforms * sizeof(GLint);
        auto active_uniform_indices_buffer = MUST(ByteBuffer::create_zeroed(buffer_size));
        glGetActiveUniformBlockivRobustANGLE(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, num_active_uniforms, nullptr, reinterpret_cast<GLint*>(active_uniform_indices_buffer.data()));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(active_uniform_indices_buffer));
        return JS::Uint32Array::create(m_realm, num_active_uniforms, array_buffer);
    }
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: {
        GLint result = 0;
        glGetActiveUniformBlockivRobustANGLE(program_handle, uniform_block_index, pname, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    default:
        dbgln("Unknown WebGL active uniform block parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

Optional<String> WebGL2RenderingContextImpl::get_active_uniform_block_name(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index)
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

    GLint uniform_block_name_length = 0;
    glGetActiveUniformBlockivRobustANGLE(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_NAME_LENGTH, 1, nullptr, &uniform_block_name_length);
    Vector<GLchar> uniform_block_name;
    uniform_block_name.resize(uniform_block_name_length);
    if (!uniform_block_name_length)
        return String {};
    glGetActiveUniformBlockName(program_handle, uniform_block_index, uniform_block_name_length, nullptr, uniform_block_name.data());
    return String::from_utf8_without_validation(ReadonlyBytes { uniform_block_name.data(), static_cast<size_t>(uniform_block_name_length - 1) });
}

void WebGL2RenderingContextImpl::uniform_block_binding(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong uniform_block_binding)
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
    glUniformBlockBinding(program_handle, uniform_block_index, uniform_block_binding);
}

GC::Root<WebGLVertexArrayObject> WebGL2RenderingContextImpl::create_vertex_array()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenVertexArrays(1, &handle);
    return WebGLVertexArrayObject::create(m_realm, *this, handle);
}

void WebGL2RenderingContextImpl::delete_vertex_array(GC::Root<WebGLVertexArrayObject> vertex_array)
{
    m_context->make_current();

    GLuint vertex_array_handle = 0;
    if (vertex_array) {
        auto handle_or_error = vertex_array->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        vertex_array_handle = handle_or_error.release_value();
    }

    glDeleteVertexArrays(1, &vertex_array_handle);
}

bool WebGL2RenderingContextImpl::is_vertex_array(GC::Root<WebGLVertexArrayObject> vertex_array)
{
    m_context->make_current();

    auto vertex_array_handle = 0;
    if (vertex_array) {
        auto handle_or_error = vertex_array->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        vertex_array_handle = handle_or_error.release_value();
    }
    return glIsVertexArray(vertex_array_handle);
}

void WebGL2RenderingContextImpl::bind_vertex_array(GC::Root<WebGLVertexArrayObject> array)
{
    m_context->make_current();

    auto array_handle = 0;
    if (array) {
        auto handle_or_error = array->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        array_handle = handle_or_error.release_value();
    }
    glBindVertexArray(array_handle);
}

void WebGL2RenderingContextImpl::buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    glBufferData(target, size, 0, usage);
}

void WebGL2RenderingContextImpl::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::BufferSource> src_data, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    auto data = MUST(get_offset_span<u8 const>(*src_data, /* src_offset= */ 0));
    glBufferData(target, data.size(), data.data(), usage);
}

void WebGL2RenderingContextImpl::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, GC::Root<WebIDL::BufferSource> src_data)
{
    m_context->make_current();

    auto data = MUST(get_offset_span<u8 const>(*src_data, /* src_offset= */ 0));
    glBufferSubData(target, dst_byte_offset, data.size(), data.data());
}

void WebGL2RenderingContextImpl::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLong usage, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    auto span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, length), GL_INVALID_VALUE);
    glBufferData(target, span.size(), span.data(), usage);
}

void WebGL2RenderingContextImpl::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    auto span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, length), GL_INVALID_VALUE);
    glBufferSubData(target, dst_byte_offset, span.size(), span.data());
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (pixels) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*pixels, /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, 0, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (pixels) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*pixels, /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type, width, height);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, border, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (src_data) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset), GL_INVALID_OPERATION);
    }

    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type, width, height);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes pixels_span;
    if (src_data) {
        pixels_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset), GL_INVALID_OPERATION);
    }

    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, pixels_span.size(), pixels_span.data());
}

void WebGL2RenderingContextImpl::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexImage2DRobustANGLE(target, level, internalformat, width, height, border, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextImpl::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextImpl::compressed_tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexImage3DRobustANGLE(target, level, internalformat, width, height, depth, border, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextImpl::compressed_tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexSubImage3DRobustANGLE(target, level, xoffset, yoffset, zoffset, width, height, depth, format, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextImpl::uniform1fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    glUniform1fv(location->handle(), span.size(), span.data());
}

void WebGL2RenderingContextImpl::uniform2fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2fv(location->handle(), span.size() / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3fv(location->handle(), span.size() / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4fv(location->handle(), span.size() / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform1iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    glUniform1iv(location->handle(), span.size(), span.data());
}

void WebGL2RenderingContextImpl::uniform2iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2iv(location->handle(), span.size() / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3iv(location->handle(), span.size() / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4iv(GC::Root<WebGLUniformLocation> location, Int32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_int32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4iv(location->handle(), span.size() / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 2 * 2;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 3 * 3;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    constexpr auto matrix_size = 4 * 4;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4fv(location->handle(), span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (!pixels) {
        return;
    }

    auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
    glReadPixelsRobustANGLE(x, y, width, height, format, type, span.size(), nullptr, nullptr, nullptr, span.data());
}

void WebGL2RenderingContextImpl::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height,
    WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();

    if (!m_pixel_pack_buffer_binding) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glReadPixelsRobustANGLE(x, y, width, height, format, type, 0, nullptr, nullptr, nullptr, reinterpret_cast<void*>(offset));
}

void WebGL2RenderingContextImpl::active_texture(WebIDL::UnsignedLong texture)
{
    m_context->make_current();
    glActiveTexture(texture);
}

void WebGL2RenderingContextImpl::attach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader)
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

void WebGL2RenderingContextImpl::bind_attrib_location(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index, String name)
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

void WebGL2RenderingContextImpl::bind_buffer(WebIDL::UnsignedLong target, GC::Root<WebGLBuffer> buffer)
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

    glBindBuffer(target, buffer_handle);
}

void WebGL2RenderingContextImpl::bind_framebuffer(WebIDL::UnsignedLong target, GC::Root<WebGLFramebuffer> framebuffer)
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

void WebGL2RenderingContextImpl::bind_renderbuffer(WebIDL::UnsignedLong target, GC::Root<WebGLRenderbuffer> renderbuffer)
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

void WebGL2RenderingContextImpl::bind_texture(WebIDL::UnsignedLong target, GC::Root<WebGLTexture> texture)
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
        m_texture_binding_2d_array = texture;
        break;
    case GL_TEXTURE_3D:
        m_texture_binding_3d = texture;
        break;

    default:
        dbgln("Unknown WebGL texture target for storing current binding: 0x{:04x}", target);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glBindTexture(target, texture_handle);
}

void WebGL2RenderingContextImpl::blend_color(float red, float green, float blue, float alpha)
{
    m_context->make_current();
    glBlendColor(red, green, blue, alpha);
}

void WebGL2RenderingContextImpl::blend_equation(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glBlendEquation(mode);
}

void WebGL2RenderingContextImpl::blend_equation_separate(WebIDL::UnsignedLong mode_rgb, WebIDL::UnsignedLong mode_alpha)
{
    m_context->make_current();
    glBlendEquationSeparate(mode_rgb, mode_alpha);
}

void WebGL2RenderingContextImpl::blend_func(WebIDL::UnsignedLong sfactor, WebIDL::UnsignedLong dfactor)
{
    m_context->make_current();
    glBlendFunc(sfactor, dfactor);
}

void WebGL2RenderingContextImpl::blend_func_separate(WebIDL::UnsignedLong src_rgb, WebIDL::UnsignedLong dst_rgb, WebIDL::UnsignedLong src_alpha, WebIDL::UnsignedLong dst_alpha)
{
    m_context->make_current();
    glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
}

WebIDL::UnsignedLong WebGL2RenderingContextImpl::check_framebuffer_status(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    return glCheckFramebufferStatus(target);
}

void WebGL2RenderingContextImpl::clear(WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glClear(mask);
}

void WebGL2RenderingContextImpl::clear_color(float red, float green, float blue, float alpha)
{
    m_context->make_current();
    glClearColor(red, green, blue, alpha);
}

void WebGL2RenderingContextImpl::clear_depth(float depth)
{
    m_context->make_current();
    glClearDepthf(depth);
}

void WebGL2RenderingContextImpl::clear_stencil(WebIDL::Long s)
{
    m_context->make_current();
    glClearStencil(s);
}

void WebGL2RenderingContextImpl::color_mask(bool red, bool green, bool blue, bool alpha)
{
    m_context->make_current();
    glColorMask(red, green, blue, alpha);
}

void WebGL2RenderingContextImpl::compile_shader(GC::Root<WebGLShader> shader)
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

void WebGL2RenderingContextImpl::copy_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border)
{
    m_context->make_current();
    glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void WebGL2RenderingContextImpl::copy_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

GC::Root<WebGLBuffer> WebGL2RenderingContextImpl::create_buffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenBuffers(1, &handle);
    return WebGLBuffer::create(m_realm, *this, handle);
}

GC::Root<WebGLFramebuffer> WebGL2RenderingContextImpl::create_framebuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenFramebuffers(1, &handle);
    return WebGLFramebuffer::create(m_realm, *this, handle);
}

GC::Root<WebGLProgram> WebGL2RenderingContextImpl::create_program()
{
    m_context->make_current();
    return WebGLProgram::create(m_realm, *this, glCreateProgram());
}

GC::Root<WebGLRenderbuffer> WebGL2RenderingContextImpl::create_renderbuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenRenderbuffers(1, &handle);
    return WebGLRenderbuffer::create(m_realm, *this, handle);
}

GC::Root<WebGLShader> WebGL2RenderingContextImpl::create_shader(WebIDL::UnsignedLong type)
{
    m_context->make_current();

    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        dbgln("Unknown WebGL shader type: 0x{:04x}", type);
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }

    GLuint handle = glCreateShader(type);
    return WebGLShader::create(m_realm, *this, handle, type);
}

GC::Root<WebGLTexture> WebGL2RenderingContextImpl::create_texture()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenTextures(1, &handle);
    return WebGLTexture::create(m_realm, *this, handle);
}

void WebGL2RenderingContextImpl::cull_face(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glCullFace(mode);
}

void WebGL2RenderingContextImpl::delete_buffer(GC::Root<WebGLBuffer> buffer)
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

void WebGL2RenderingContextImpl::delete_framebuffer(GC::Root<WebGLFramebuffer> framebuffer)
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

void WebGL2RenderingContextImpl::delete_program(GC::Root<WebGLProgram> program)
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
}

void WebGL2RenderingContextImpl::delete_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer)
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

void WebGL2RenderingContextImpl::delete_shader(GC::Root<WebGLShader> shader)
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

void WebGL2RenderingContextImpl::delete_texture(GC::Root<WebGLTexture> texture)
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
}

void WebGL2RenderingContextImpl::depth_func(WebIDL::UnsignedLong func)
{
    m_context->make_current();
    glDepthFunc(func);
}

void WebGL2RenderingContextImpl::depth_mask(bool flag)
{
    m_context->make_current();
    glDepthMask(flag);
}

void WebGL2RenderingContextImpl::depth_range(float z_near, float z_far)
{
    m_context->make_current();
    glDepthRangef(z_near, z_far);
}

void WebGL2RenderingContextImpl::detach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader)
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
}

void WebGL2RenderingContextImpl::disable(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    glDisable(cap);
}

void WebGL2RenderingContextImpl::disable_vertex_attrib_array(WebIDL::UnsignedLong index)
{
    m_context->make_current();
    glDisableVertexAttribArray(index);
}

void WebGL2RenderingContextImpl::draw_arrays(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glDrawArrays(mode, first, count);
}

void WebGL2RenderingContextImpl::draw_elements(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    glDrawElements(mode, count, type, reinterpret_cast<void*>(offset));
    needs_to_present();
}

void WebGL2RenderingContextImpl::enable(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    glEnable(cap);
}

void WebGL2RenderingContextImpl::enable_vertex_attrib_array(WebIDL::UnsignedLong index)
{
    m_context->make_current();
    glEnableVertexAttribArray(index);
}

void WebGL2RenderingContextImpl::finish()
{
    m_context->make_current();
    glFinish();
}

void WebGL2RenderingContextImpl::flush()
{
    m_context->make_current();
    glFlush();
}

void WebGL2RenderingContextImpl::framebuffer_renderbuffer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong renderbuffertarget, GC::Root<WebGLRenderbuffer> renderbuffer)
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

void WebGL2RenderingContextImpl::framebuffer_texture2d(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong textarget, GC::Root<WebGLTexture> texture, WebIDL::Long level)
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

void WebGL2RenderingContextImpl::front_face(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glFrontFace(mode);
}

void WebGL2RenderingContextImpl::generate_mipmap(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    glGenerateMipmap(target);
}

GC::Root<WebGLActiveInfo> WebGL2RenderingContextImpl::get_active_attrib(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index)
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
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
}

GC::Root<WebGLActiveInfo> WebGL2RenderingContextImpl::get_active_uniform(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index)
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
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
}

Optional<Vector<GC::Root<WebGLShader>>> WebGL2RenderingContextImpl::get_attached_shaders(GC::Root<WebGLProgram> program)
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

WebIDL::Long WebGL2RenderingContextImpl::get_attrib_location(GC::Root<WebGLProgram> program, String name)
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

JS::Value WebGL2RenderingContextImpl::get_buffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
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

JS::Value WebGL2RenderingContextImpl::get_parameter(WebIDL::UnsignedLong pname)
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
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Float32Array::create(m_realm, 2, array_buffer);
    }
    case GL_ALIASED_POINT_SIZE_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_ALIASED_POINT_SIZE_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Float32Array::create(m_realm, 2, array_buffer);
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
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Float32Array::create(m_realm, 4, array_buffer);
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
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Float32Array::create(m_realm, 4, array_buffer);
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
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Float32Array::create(m_realm, 2, array_buffer);
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
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Int32Array::create(m_realm, 2, array_buffer);
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
        return JS::PrimitiveString::create(m_realm->vm(), ByteString { result });
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
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Int32Array::create(m_realm, 4, array_buffer);
    }
    case GL_SCISSOR_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SCISSOR_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SHADING_LANGUAGE_VERSION: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        return JS::PrimitiveString::create(m_realm->vm(), ByteString { result });
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
        return JS::PrimitiveString::create(m_realm->vm(), ByteString { result });
    }
    case GL_VERSION: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_VERSION));
        return JS::PrimitiveString::create(m_realm->vm(), ByteString { result });
    }
    case GL_VIEWPORT: {
        Array<GLint, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(m_realm, move(byte_buffer));
        return JS::Int32Array::create(m_realm, 4, array_buffer);
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
    case GL_MAX_COLOR_ATTACHMENTS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_COLOR_ATTACHMENTS, 1, nullptr, &result);
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
    case GL_MAX_DRAW_BUFFERS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_DRAW_BUFFERS, 1, nullptr, &result);
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
    case GL_UNIFORM_BUFFER_BINDING: {
        if (!m_uniform_buffer_binding)
            return JS::js_null();
        return JS::Value(m_uniform_buffer_binding);
    }
    case GL_TEXTURE_BINDING_2D_ARRAY: {
        if (!m_texture_binding_2d_array)
            return JS::js_null();
        return JS::Value(m_texture_binding_2d_array);
    }
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
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (ext_texture_filter_anisotropic_extension_enabled()) {
            GLfloat result { 0.0f };
            glGetFloatvRobustANGLE(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
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
    case GL_TRANSFORM_FEEDBACK_PAUSED: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_TRANSFORM_FEEDBACK_PAUSED, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case UNPACK_FLIP_Y_WEBGL:
        return JS::Value(m_unpack_flip_y);
    default:
        dbgln("Unknown WebGL parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

WebIDL::UnsignedLong WebGL2RenderingContextImpl::get_error()
{
    m_context->make_current();
    return glGetError();
}

JS::Value WebGL2RenderingContextImpl::get_program_parameter(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong pname)
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

    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
    case GL_TRANSFORM_FEEDBACK_VARYINGS:
    case GL_ACTIVE_UNIFORM_BLOCKS:

        return JS::Value(result);
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

Optional<String> WebGL2RenderingContextImpl::get_program_info_log(GC::Root<WebGLProgram> program)
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

JS::Value WebGL2RenderingContextImpl::get_shader_parameter(GC::Root<WebGLShader> shader, WebIDL::UnsignedLong pname)
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

GC::Root<WebGLShaderPrecisionFormat> WebGL2RenderingContextImpl::get_shader_precision_format(WebIDL::UnsignedLong shadertype, WebIDL::UnsignedLong precisiontype)
{
    m_context->make_current();

    GLint range[2];
    GLint precision;
    glGetShaderPrecisionFormat(shadertype, precisiontype, range, &precision);
    return WebGLShaderPrecisionFormat::create(m_realm, range[0], range[1], precision);
}

Optional<String> WebGL2RenderingContextImpl::get_shader_info_log(GC::Root<WebGLShader> shader)
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

GC::Root<WebGLUniformLocation> WebGL2RenderingContextImpl::get_uniform_location(GC::Root<WebGLProgram> program, String name)
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

    return WebGLUniformLocation::create(m_realm, location);
}

void WebGL2RenderingContextImpl::hint(WebIDL::UnsignedLong target, WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glHint(target, mode);
}

bool WebGL2RenderingContextImpl::is_buffer(GC::Root<WebGLBuffer> buffer)
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

bool WebGL2RenderingContextImpl::is_enabled(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    return glIsEnabled(cap);
}

bool WebGL2RenderingContextImpl::is_framebuffer(GC::Root<WebGLFramebuffer> framebuffer)
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

bool WebGL2RenderingContextImpl::is_program(GC::Root<WebGLProgram> program)
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

bool WebGL2RenderingContextImpl::is_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer)
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

bool WebGL2RenderingContextImpl::is_shader(GC::Root<WebGLShader> shader)
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

bool WebGL2RenderingContextImpl::is_texture(GC::Root<WebGLTexture> texture)
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

void WebGL2RenderingContextImpl::line_width(float width)
{
    m_context->make_current();
    glLineWidth(width);
}

void WebGL2RenderingContextImpl::link_program(GC::Root<WebGLProgram> program)
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

void WebGL2RenderingContextImpl::pixel_storei(WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();

    switch (pname) {
    case UNPACK_FLIP_Y_WEBGL:
        m_unpack_flip_y = param != GL_FALSE;
        return;
    }

    glPixelStorei(pname, param);
}

void WebGL2RenderingContextImpl::polygon_offset(float factor, float units)
{
    m_context->make_current();
    glPolygonOffset(factor, units);
}

void WebGL2RenderingContextImpl::renderbuffer_storage(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();

    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;

    glRenderbufferStorage(target, internalformat, width, height);
}

void WebGL2RenderingContextImpl::sample_coverage(float value, bool invert)
{
    m_context->make_current();
    glSampleCoverage(value, invert);
}

void WebGL2RenderingContextImpl::scissor(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glScissor(x, y, width, height);
}

void WebGL2RenderingContextImpl::shader_source(GC::Root<WebGLShader> shader, String source)
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

void WebGL2RenderingContextImpl::stencil_func(WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilFunc(func, ref, mask);
}

void WebGL2RenderingContextImpl::stencil_func_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilFuncSeparate(face, func, ref, mask);
}

void WebGL2RenderingContextImpl::stencil_mask(WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilMask(mask);
}

void WebGL2RenderingContextImpl::stencil_mask_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilMaskSeparate(face, mask);
}

void WebGL2RenderingContextImpl::stencil_op(WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass)
{
    m_context->make_current();
    glStencilOp(fail, zfail, zpass);
}

void WebGL2RenderingContextImpl::stencil_op_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass)
{
    m_context->make_current();
    glStencilOpSeparate(face, fail, zfail, zpass);
}

void WebGL2RenderingContextImpl::tex_parameterf(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, float param)
{
    m_context->make_current();
    glTexParameterf(target, pname, param);
}

void WebGL2RenderingContextImpl::tex_parameteri(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();
    glTexParameteri(target, pname, param);
}

void WebGL2RenderingContextImpl::uniform1f(GC::Root<WebGLUniformLocation> location, float x)
{
    m_context->make_current();
    glUniform1f(location ? location->handle() : 0, x);
}

void WebGL2RenderingContextImpl::uniform2f(GC::Root<WebGLUniformLocation> location, float x, float y)
{
    m_context->make_current();
    glUniform2f(location ? location->handle() : 0, x, y);
}

void WebGL2RenderingContextImpl::uniform3f(GC::Root<WebGLUniformLocation> location, float x, float y, float z)
{
    m_context->make_current();
    glUniform3f(location ? location->handle() : 0, x, y, z);
}

void WebGL2RenderingContextImpl::uniform4f(GC::Root<WebGLUniformLocation> location, float x, float y, float z, float w)
{
    m_context->make_current();
    glUniform4f(location ? location->handle() : 0, x, y, z, w);
}

void WebGL2RenderingContextImpl::uniform1i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x)
{
    m_context->make_current();
    glUniform1i(location ? location->handle() : 0, x);
}

void WebGL2RenderingContextImpl::uniform2i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y)
{
    m_context->make_current();
    glUniform2i(location ? location->handle() : 0, x, y);
}

void WebGL2RenderingContextImpl::uniform3i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z)
{
    m_context->make_current();
    glUniform3i(location ? location->handle() : 0, x, y, z);
}

void WebGL2RenderingContextImpl::uniform4i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w)
{
    m_context->make_current();
    glUniform4i(location ? location->handle() : 0, x, y, z, w);
}

void WebGL2RenderingContextImpl::use_program(GC::Root<WebGLProgram> program)
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

void WebGL2RenderingContextImpl::validate_program(GC::Root<WebGLProgram> program)
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

void WebGL2RenderingContextImpl::vertex_attrib1f(WebIDL::UnsignedLong index, float x)
{
    m_context->make_current();
    glVertexAttrib1f(index, x);
}

void WebGL2RenderingContextImpl::vertex_attrib2f(WebIDL::UnsignedLong index, float x, float y)
{
    m_context->make_current();
    glVertexAttrib2f(index, x, y);
}

void WebGL2RenderingContextImpl::vertex_attrib3f(WebIDL::UnsignedLong index, float x, float y, float z)
{
    m_context->make_current();
    glVertexAttrib3f(index, x, y, z);
}

void WebGL2RenderingContextImpl::vertex_attrib4f(WebIDL::UnsignedLong index, float x, float y, float z, float w)
{
    m_context->make_current();
    glVertexAttrib4f(index, x, y, z, w);
}

void WebGL2RenderingContextImpl::vertex_attrib1fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 1) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib1fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib2fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 2) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib2fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib3fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 3) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib3fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib4fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib4fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, bool normalized, WebIDL::Long stride, WebIDL::LongLong offset)
{
    m_context->make_current();

    glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<void*>(offset));
}

void WebGL2RenderingContextImpl::viewport(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glViewport(x, y, width, height);
}

void WebGL2RenderingContextImpl::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_realm);
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
}

}
