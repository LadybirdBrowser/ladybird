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

namespace Web::WebGL {

WebGL2RenderingContextImpl::WebGL2RenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : WebGLRenderingContextImpl(realm, move(context))
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
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(samples_buffer));
        return JS::Int32Array::create(realm(), num_sample_counts, array_buffer);
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

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform1ui(location_handle, v0);
}

void WebGL2RenderingContextImpl::uniform2ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform2ui(location_handle, v0, v1);
}

void WebGL2RenderingContextImpl::uniform3ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform3ui(location_handle, v0, v1, v2);
}

void WebGL2RenderingContextImpl::uniform4ui(GC::Root<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2, WebIDL::UnsignedLong v3)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform4ui(location_handle, v0, v1, v2, v3);
}

void WebGL2RenderingContextImpl::uniform1uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    glUniform1uiv(location_handle, span.size(), span.data());
}

void WebGL2RenderingContextImpl::uniform2uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2uiv(location_handle, span.size() / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3uiv(location_handle, span.size() / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4uiv(GC::Root<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4uiv(location_handle, span.size() / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3x2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 3 * 2;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3x2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4x2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 4 * 2;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4x2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2x3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 2 * 3;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2x3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4x3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 4 * 3;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4x3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2x4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 2 * 4;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2x4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3x4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 3 * 4;
    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_float32_list(data, src_offset, src_length), GL_INVALID_VALUE);
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3x4fv(location_handle, span.size() / matrix_size, transpose, span.data());
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
    return WebGLQuery::create(realm(), *this, handle);
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

    switch (target) {
    case GL_ANY_SAMPLES_PASSED:
        m_any_samples_passed = query;
        break;
    case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
        m_any_samples_passed_conservative = query;
        break;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
        m_transform_feedback_primitives_written = query;
        break;
    }

    glBeginQuery(target, query_handle);
}

void WebGL2RenderingContextImpl::end_query(WebIDL::UnsignedLong target)
{
    m_context->make_current();

    switch (target) {
    case GL_ANY_SAMPLES_PASSED:
        m_any_samples_passed = nullptr;
        break;
    case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
        m_any_samples_passed_conservative = nullptr;
        break;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
        m_transform_feedback_primitives_written = nullptr;
        break;
    }

    glEndQuery(target);
}

GC::Root<WebGLQuery> WebGL2RenderingContextImpl::get_query(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    if (pname != GL_CURRENT_QUERY) {
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }

    switch (target) {
    case GL_ANY_SAMPLES_PASSED:
        return m_any_samples_passed;
    case GL_ANY_SAMPLES_PASSED_CONSERVATIVE:
        return m_any_samples_passed_conservative;
    case GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN:
        return m_transform_feedback_primitives_written;
    }

    set_error(GL_INVALID_ENUM);
    return nullptr;
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
    return WebGLSampler::create(realm(), *this, handle);
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
    return WebGLSync::create(realm(), *this, handle);
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
    return WebGLTransformFeedback::create(realm(), *this, handle);
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

        if (!buffer->is_compatible_with(target)) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
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

        if (!buffer->is_compatible_with(target)) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
    }
    glBindBufferRange(target, index, buffer_handle, offset, size);
}

Optional<Vector<WebIDL::UnsignedLong>> WebGL2RenderingContextImpl::get_uniform_indices(GC::Root<WebGLProgram> program, Vector<String> const& uniform_names)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return OptionalNone {};
    }

    auto program_handle = handle_or_error.release_value();

    Vector<Vector<GLchar>> uniform_names_strings;
    uniform_names_strings.ensure_capacity(uniform_names.size());
    for (auto const& uniform_name : uniform_names) {
        uniform_names_strings.unchecked_append(null_terminated_string(uniform_name));
    }

    Vector<GLchar const*> uniform_names_characters;
    uniform_names_characters.ensure_capacity(uniform_names_strings.size());
    for (auto const& uniform_name_string : uniform_names_strings) {
        uniform_names_characters.unchecked_append(uniform_name_string.data());
    }

    auto result_buffer = MUST(ByteBuffer::create_zeroed(uniform_names_characters.size() * sizeof(WebIDL::UnsignedLong)));
    auto result_span = result_buffer.bytes().reinterpret<WebIDL::UnsignedLong>();
    glGetUniformIndices(program_handle, uniform_names_characters.size(), uniform_names_characters.data(), result_span.data());
    return Vector<WebIDL::UnsignedLong> { result_span };
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

    return JS::Array::create_from(realm(), params_as_values);
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
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(active_uniform_indices_buffer));
        return JS::Uint32Array::create(realm(), num_active_uniforms, array_buffer);
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
    return WebGLVertexArrayObject::create(realm(), *this, handle);
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
    if (m_current_vertex_array == vertex_array)
        m_current_vertex_array = nullptr;
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
    m_current_vertex_array = array;
}

void WebGL2RenderingContextImpl::compressed_tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(internalformat)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexImage3DRobustANGLE(target, level, internalformat, width, height, depth, border, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextImpl::compressed_tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(*src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    glCompressedTexSubImage3DRobustANGLE(target, level, xoffset, yoffset, zoffset, width, height, depth, format, pixels.size(), pixels.size(), pixels.data());
}

}
