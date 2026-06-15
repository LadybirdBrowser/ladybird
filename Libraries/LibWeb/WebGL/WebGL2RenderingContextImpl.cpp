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
#include <LibWeb/WebGL/WebGL2RenderingContextImpl.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
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

WebGL2RenderingContextImpl::WebGL2RenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<WebGLContextProxy> context)
    : WebGLRenderingContextImpl(realm, move(context))
{
}

void WebGL2RenderingContextImpl::copy_buffer_sub_data(WebIDL::UnsignedLong read_target, WebIDL::UnsignedLong write_target, WebIDL::LongLong read_offset, WebIDL::LongLong write_offset, WebIDL::LongLong size)
{
    m_context->make_current();
    m_context->copy_buffer_sub_data(read_target, write_target, read_offset, write_offset, size);
}

// https://registry.khronos.org/webgl/specs/latest/2.0/#3.7.3
void WebGL2RenderingContextImpl::get_buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong src_byte_offset,
    WebIDL::ArrayBufferView dst_buffer, WebIDL::UnsignedLongLong dst_offset, WebIDL::UnsignedLong length)
{
    // If dstBuffer is a DataView, let elementSize be 1; otherwise, let elementSize be dstBuffer.BYTES_PER_ELEMENT.
    size_t element_size = dst_buffer.element_size();

    // If length is 0:
    size_t copy_length;
    if (length == 0) {
        // If dstBuffer is a DataView, let copyLength be dstBuffer.byteLength - dstOffset; the typed elements in the
        // text below are bytes. Otherwise, let copyLength be dstBuffer.length - dstOffset.
        copy_length = dst_buffer.byte_length() / element_size - dst_offset;
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
    if (dst_offset_in_bytes > dst_buffer.byte_length()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    // If dstOffset + copyLength is greater than dstBuffer.length (or dstBuffer.byteLength in the case of DataView),
    // generates an INVALID_VALUE error.
    size_t copy_bytes = copy_length * element_size;
    if (dst_offset_in_bytes + copy_bytes > dst_buffer.byte_length()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    // If copyLength is greater than zero, copy copyLength typed elements (each of size elementSize) from buf into
    // dstBuffer, reading buf starting at byte index srcByteOffset and writing into dstBuffer starting at element
    // index dstOffset. The destination span is bounds-checked above, so the readback can
    // land directly in the JS-owned buffer without staging.
    auto dst_span = dst_buffer.viewed_array_buffer()->span().slice(dst_buffer.byte_offset() + dst_offset_in_bytes, copy_bytes);
    m_context->read_buffer_sub_data(target, src_byte_offset, dst_span);
}

void WebGL2RenderingContextImpl::blit_framebuffer(WebIDL::Long src_x0, WebIDL::Long src_y0, WebIDL::Long src_x1, WebIDL::Long src_y1, WebIDL::Long dst_x0, WebIDL::Long dst_y0, WebIDL::Long dst_x1, WebIDL::Long dst_y1, WebIDL::UnsignedLong mask, WebIDL::UnsignedLong filter)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    did_update_canvas_content();
    m_context->blit_framebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

void WebGL2RenderingContextImpl::framebuffer_texture_layer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, GC::Ptr<WebGLTexture> texture, WebIDL::Long level, WebIDL::Long layer)
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

    m_context->framebuffer_texture_layer(target, attachment, texture_handle, level, layer);
}

void WebGL2RenderingContextImpl::invalidate_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    m_context->invalidate_framebuffer(target, attachments.size(), attachments.data());
    did_update_canvas_content();
}

void WebGL2RenderingContextImpl::invalidate_sub_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    m_context->invalidate_sub_framebuffer(target, attachments.size(), attachments.data(), x, y, width, height);
    did_update_canvas_content();
}

void WebGL2RenderingContextImpl::read_buffer(WebIDL::UnsignedLong src)
{
    m_context->make_current();
    m_context->read_buffer(src);
}

JS::Value WebGL2RenderingContextImpl::get_internalformat_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    switch (pname) {
    case GL_SAMPLES: {
        GLint num_sample_counts { 0 };
        m_context->get_internalformativ_robust_angle(target, internalformat, GL_NUM_SAMPLE_COUNTS, 1, nullptr, &num_sample_counts);
        auto samples_buffer = MUST(ByteBuffer::create_zeroed(num_sample_counts * sizeof(GLint)));
        m_context->get_internalformativ_robust_angle(target, internalformat, GL_SAMPLES, num_sample_counts, nullptr, reinterpret_cast<GLint*>(samples_buffer.data()));
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
    m_context->renderbuffer_storage_multisample(target, samples, internalformat, width, height);
}

void WebGL2RenderingContextImpl::tex_storage2d(WebIDL::UnsignedLong target, WebIDL::Long levels, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();

    m_context->tex_storage2d(target, levels, internalformat, width, height);
}

void WebGL2RenderingContextImpl::tex_storage3d(WebIDL::UnsignedLong target, WebIDL::Long levels, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth)
{
    m_context->make_current();
    m_context->tex_storage3d(target, levels, internalformat, width, height, depth);
}

void WebGL2RenderingContextImpl::tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant src_data)
{
    m_context->make_current();

    ReadonlyBytes src_data_span;
    if (!src_data.has<Empty>()) {
        src_data_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data.downcast<WebIDL::ArrayBufferViewVariant>(), /* src_offset= */ 0), GL_INVALID_OPERATION);
    }

    m_context->tex_image3d_robust_angle(target, level, internalformat, width, height, depth, border, format, type, src_data_span.size(), src_data_span.data());
}

void WebGL2RenderingContextImpl::tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    auto src_data_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset), GL_INVALID_OPERATION);

    m_context->tex_image3d_robust_angle(target, level, internalformat, width, height, depth, border, format, type, src_data_span.size(), src_data_span.data());
}

void WebGL2RenderingContextImpl::tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, WebIDL::NullableArrayBufferViewVariant src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    ReadonlyBytes src_data_span;
    if (!src_data.has<Empty>()) {
        src_data_span = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data.downcast<WebIDL::ArrayBufferViewVariant>(), src_offset), GL_INVALID_OPERATION);
    }

    m_context->tex_sub_image3d_robust_angle(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, src_data_span.size(), src_data_span.data());
}

void WebGL2RenderingContextImpl::uniform1ui(GC::Ptr<WebGLUniformLocation> location, WebIDL::UnsignedLong v0)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    m_context->uniform1ui(location_handle, v0);
}

void WebGL2RenderingContextImpl::uniform2ui(GC::Ptr<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    m_context->uniform2ui(location_handle, v0, v1);
}

void WebGL2RenderingContextImpl::uniform3ui(GC::Ptr<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    m_context->uniform3ui(location_handle, v0, v1, v2);
}

void WebGL2RenderingContextImpl::uniform4ui(GC::Ptr<WebGLUniformLocation> location, WebIDL::UnsignedLong v0, WebIDL::UnsignedLong v1, WebIDL::UnsignedLong v2, WebIDL::UnsignedLong v3)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    m_context->uniform4ui(location_handle, v0, v1, v2, v3);
}

void WebGL2RenderingContextImpl::uniform1uiv(GC::Ptr<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = SET_ERROR_VALUE_IF_ERROR(span_from_uint32_list(values, src_offset, src_length), GL_INVALID_VALUE);
    m_context->uniform1uiv(location_handle, span.size(), span.data());
}

void WebGL2RenderingContextImpl::uniform2uiv(GC::Ptr<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform2uiv(location_handle, span.size() / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3uiv(GC::Ptr<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform3uiv(location_handle, span.size() / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4uiv(GC::Ptr<WebGLUniformLocation> location, Uint32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform4uiv(location_handle, span.size() / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3x2fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix3x2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4x2fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix4x2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2x3fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix2x3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4x3fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix4x3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2x4fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix2x4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3x4fv(GC::Ptr<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
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
    m_context->uniform_matrix3x4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_i4i(WebIDL::UnsignedLong index, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w)
{
    m_context->make_current();
    m_context->vertex_attrib_i4i(index, x, y, z, w);
}

void WebGL2RenderingContextImpl::vertex_attrib_i4iv(WebIDL::UnsignedLong index, Int32List values)
{
    m_context->make_current();
    auto span = MUST(span_from_int32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    m_context->vertex_attrib_i4iv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_i4ui(WebIDL::UnsignedLong index, WebIDL::UnsignedLong x, WebIDL::UnsignedLong y, WebIDL::UnsignedLong z, WebIDL::UnsignedLong w)
{
    m_context->make_current();
    m_context->vertex_attrib_i4ui(index, x, y, z, w);
}

void WebGL2RenderingContextImpl::vertex_attrib_i4uiv(WebIDL::UnsignedLong index, Uint32List values)
{
    m_context->make_current();
    auto span = MUST(span_from_uint32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    m_context->vertex_attrib_i4uiv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib_i_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, WebIDL::Long stride, WebIDL::LongLong offset)
{
    m_context->make_current();

    m_context->vertex_attrib_i_pointer(index, size, type, stride, reinterpret_cast<void*>(offset));
}

void WebGL2RenderingContextImpl::vertex_attrib_divisor(WebIDL::UnsignedLong index, WebIDL::UnsignedLong divisor)
{
    m_context->make_current();
    m_context->vertex_attrib_divisor(index, divisor);
}

void WebGL2RenderingContextImpl::draw_arrays_instanced(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count, WebIDL::Long instance_count)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    did_update_canvas_content();
    m_context->draw_arrays_instanced(mode, first, count, instance_count);
}

void WebGL2RenderingContextImpl::draw_elements_instanced(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset, WebIDL::Long instance_count)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    m_context->draw_elements_instanced(mode, count, type, reinterpret_cast<void*>(offset), instance_count);
    did_update_canvas_content();
}

void WebGL2RenderingContextImpl::draw_range_elements(WebIDL::UnsignedLong mode, WebIDL::UnsignedLong start, WebIDL::UnsignedLong end, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    did_update_canvas_content();
    m_context->draw_range_elements(mode, start, end, count, type, reinterpret_cast<void*>(offset));
}

void WebGL2RenderingContextImpl::draw_buffers(Vector<WebIDL::UnsignedLong> buffers)
{
    m_context->make_current();

    m_context->draw_buffers(buffers.size(), buffers.data());
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

    m_context->clear_bufferfv(buffer, drawbuffer, span.data());
    did_update_canvas_content();
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

    m_context->clear_bufferiv(buffer, drawbuffer, span.data());
    did_update_canvas_content();
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

    m_context->clear_bufferuiv(buffer, drawbuffer, span.data());
    did_update_canvas_content();
}

void WebGL2RenderingContextImpl::clear_bufferfi(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, float depth, WebIDL::Long stencil)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    did_update_canvas_content();
    m_context->clear_bufferfi(buffer, drawbuffer, depth, stencil);
}

GC::Ref<WebGLQuery> WebGL2RenderingContextImpl::create_query()
{
    m_context->make_current();

    GLuint handle = 0;
    m_context->gen_queries(1, &handle);
    return WebGLQuery::create(realm(), *this, handle);
}

void WebGL2RenderingContextImpl::delete_query(GC::Ptr<WebGLQuery> query)
{
    m_context->make_current();

    if (!query)
        return;

    auto handle_or_error = query->handle_for_deletion(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto query_handle = handle_or_error.release_value();
    if (!query_handle.has_value())
        return;

    auto handle = query_handle.value();
    m_context->delete_queries(1, &handle);
}

void WebGL2RenderingContextImpl::begin_query(WebIDL::UnsignedLong target, GC::Ref<WebGLQuery> query)
{
    m_context->make_current();

    auto handle_or_error = query->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto query_handle = handle_or_error.release_value();

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

    m_context->begin_query(target, query_handle);
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

    m_context->end_query(target);
}

GC::Ptr<WebGLQuery> WebGL2RenderingContextImpl::get_query(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
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

JS::Value WebGL2RenderingContextImpl::get_query_parameter(GC::Ref<WebGLQuery> query, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    auto handle_or_error = query->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return JS::js_null();
    }
    auto query_handle = handle_or_error.release_value();

    GLuint result { 0 };
    m_context->get_query_objectuiv_robust_angle(query_handle, pname, 1, nullptr, &result);

    switch (pname) {
    case GL_QUERY_RESULT:
        return JS::Value(result);
    case GL_QUERY_RESULT_AVAILABLE:
        return JS::Value(result == GL_TRUE);
    default:
        return JS::js_null();
    }
}

GC::Ref<WebGLSampler> WebGL2RenderingContextImpl::create_sampler()
{
    m_context->make_current();

    GLuint handle = 0;
    m_context->gen_samplers(1, &handle);
    return WebGLSampler::create(realm(), *this, handle);
}

void WebGL2RenderingContextImpl::delete_sampler(GC::Ptr<WebGLSampler> sampler)
{
    m_context->make_current();

    if (!sampler)
        return;

    auto handle_or_error = sampler->handle_for_deletion(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto sampler_handle = handle_or_error.release_value();
    if (!sampler_handle.has_value())
        return;

    auto handle = sampler_handle.value();
    m_context->delete_samplers(1, &handle);
}

void WebGL2RenderingContextImpl::bind_sampler(WebIDL::UnsignedLong unit, GC::Ptr<WebGLSampler> sampler)
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
    m_context->bind_sampler(unit, sampler_handle);
}

void WebGL2RenderingContextImpl::sampler_parameteri(GC::Ref<WebGLSampler> sampler, WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();

    auto handle_or_error = sampler->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto sampler_handle = handle_or_error.release_value();

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
        if (extension_enabled("EXT_texture_filter_anisotropic"sv))
            break;

        set_error(GL_INVALID_ENUM);
        return;
    }
    default:
        dbgln("Unknown WebGL sampler parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return;
    }
    m_context->sampler_parameteri(sampler_handle, pname, param);
}

void WebGL2RenderingContextImpl::sampler_parameterf(GC::Ref<WebGLSampler> sampler, WebIDL::UnsignedLong pname, float param)
{
    m_context->make_current();

    auto handle_or_error = sampler->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto sampler_handle = handle_or_error.release_value();

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
        if (extension_enabled("EXT_texture_filter_anisotropic"sv))
            break;

        set_error(GL_INVALID_ENUM);
        return;
    }
    default:
        dbgln("Unknown WebGL sampler parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return;
    }
    m_context->sampler_parameterf(sampler_handle, pname, param);
}

GC::Ptr<WebGLSync> WebGL2RenderingContextImpl::fence_sync(WebIDL::UnsignedLong condition, WebIDL::UnsignedLong flags)
{
    m_context->make_current();

    GLsync handle = m_context->fence_sync(condition, flags);
    return WebGLSync::create(realm(), *this, handle);
}

void WebGL2RenderingContextImpl::delete_sync(GC::Ptr<WebGLSync> sync)
{
    m_context->make_current();

    if (!sync)
        return;

    auto handle_or_error = sync->sync_handle_for_deletion(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto sync_handle = handle_or_error.release_value();
    if (!sync_handle.has_value())
        return;

    m_context->delete_sync(static_cast<GLsync>(sync_handle.value()));
}

WebIDL::UnsignedLong WebGL2RenderingContextImpl::client_wait_sync(GC::Ref<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout)
{
    m_context->make_current();

    auto handle_or_error = sync->sync_handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return GL_WAIT_FAILED;
    }
    auto sync_handle = static_cast<GLsync>(handle_or_error.release_value());

    return m_context->client_wait_sync(sync_handle, flags, timeout);
}

void WebGL2RenderingContextImpl::wait_sync(GC::Ref<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout)
{
    m_context->make_current();

    auto handle_or_error = sync->sync_handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto sync_handle = static_cast<GLsync>(handle_or_error.release_value());

    m_context->wait_sync(sync_handle, flags, timeout);
}

JS::Value WebGL2RenderingContextImpl::get_sync_parameter(GC::Ref<WebGLSync> sync, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    auto handle_or_error = sync->sync_handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return JS::js_null();
    }
    auto sync_handle = static_cast<GLsync>(handle_or_error.release_value());

    GLint result = 0;
    m_context->get_synciv(sync_handle, pname, 1, nullptr, &result);
    return JS::Value(result);
}

GC::Ref<WebGLTransformFeedback> WebGL2RenderingContextImpl::create_transform_feedback()
{
    m_context->make_current();

    GLuint handle = 0;
    m_context->gen_transform_feedbacks(1, &handle);
    return WebGLTransformFeedback::create(realm(), *this, handle);
}

void WebGL2RenderingContextImpl::delete_transform_feedback(GC::Ptr<WebGLTransformFeedback> transform_feedback)
{
    m_context->make_current();

    if (!transform_feedback)
        return;

    auto handle_or_error = transform_feedback->handle_for_deletion(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto transform_feedback_handle = handle_or_error.release_value();
    if (!transform_feedback_handle.has_value())
        return;

    auto handle = transform_feedback_handle.value();
    m_context->delete_transform_feedbacks(1, &handle);
}

void WebGL2RenderingContextImpl::bind_transform_feedback(WebIDL::UnsignedLong target, GC::Ptr<WebGLTransformFeedback> transform_feedback)
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

    m_context->bind_transform_feedback(target, transform_feedback_handle);
}

void WebGL2RenderingContextImpl::begin_transform_feedback(WebIDL::UnsignedLong primitive_mode)
{
    m_context->make_current();
    m_context->begin_transform_feedback(primitive_mode);
}

void WebGL2RenderingContextImpl::end_transform_feedback()
{
    m_context->make_current();
    m_context->end_transform_feedback();
}

void WebGL2RenderingContextImpl::transform_feedback_varyings(GC::Ref<WebGLProgram> program, Vector<String> const& varyings, WebIDL::UnsignedLong buffer_mode)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto program_handle = handle_or_error.release_value();

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

    m_context->transform_feedback_varyings(program_handle, varying_strings_characters.size(), varying_strings_characters.data(), buffer_mode);
}

void WebGL2RenderingContextImpl::pause_transform_feedback()
{
    m_context->make_current();
    m_context->pause_transform_feedback();
}

void WebGL2RenderingContextImpl::resume_transform_feedback()
{
    m_context->make_current();
    m_context->resume_transform_feedback();
}

void WebGL2RenderingContextImpl::bind_buffer_base(WebIDL::UnsignedLong target, WebIDL::UnsignedLong index, GC::Ptr<WebGLBuffer> buffer)
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
    m_context->bind_buffer_base(target, index, buffer_handle);
}

void WebGL2RenderingContextImpl::bind_buffer_range(WebIDL::UnsignedLong target, WebIDL::UnsignedLong index, GC::Ptr<WebGLBuffer> buffer, WebIDL::LongLong offset, WebIDL::LongLong size)
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
    m_context->bind_buffer_range(target, index, buffer_handle, offset, size);
}

Optional<Vector<WebIDL::UnsignedLong>> WebGL2RenderingContextImpl::get_uniform_indices(GC::Ref<WebGLProgram> program, Vector<String> const& uniform_names)
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
    m_context->get_uniform_indices(program_handle, uniform_names_characters.size(), uniform_names_characters.data(), result_span.data());
    return Vector<WebIDL::UnsignedLong> { result_span };
}

JS::Value WebGL2RenderingContextImpl::get_active_uniforms(GC::Ref<WebGLProgram> program, Vector<WebIDL::UnsignedLong> uniform_indices, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return {};
    }
    auto program_handle = handle_or_error.release_value();

    auto params = MUST(ByteBuffer::create_zeroed(uniform_indices.size() * sizeof(GLint)));
    Span<GLint> params_span(reinterpret_cast<GLint*>(params.data()), uniform_indices.size());
    m_context->get_active_uniformsiv(program_handle, uniform_indices.size(), uniform_indices.data(), pname, params_span.data());

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

WebIDL::UnsignedLong WebGL2RenderingContextImpl::get_uniform_block_index(GC::Ref<WebGLProgram> program, String uniform_block_name)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return -1;
    }
    auto program_handle = handle_or_error.release_value();

    auto uniform_block_name_null_terminated = null_terminated_string(uniform_block_name);
    return m_context->get_uniform_block_index(program_handle, uniform_block_name_null_terminated.data());
}

JS::Value WebGL2RenderingContextImpl::get_active_uniform_block_parameter(GC::Ref<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return JS::js_null();
    }
    auto program_handle = handle_or_error.release_value();

    switch (pname) {
    case GL_UNIFORM_BLOCK_BINDING:
    case GL_UNIFORM_BLOCK_DATA_SIZE:
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS: {
        GLint result = 0;
        m_context->get_active_uniform_blockiv_robust_angle(program_handle, uniform_block_index, pname, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: {
        GLint num_active_uniforms = 0;
        m_context->get_active_uniform_blockiv_robust_angle(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS, 1, nullptr, &num_active_uniforms);
        size_t buffer_size = num_active_uniforms * sizeof(GLint);
        auto active_uniform_indices_buffer = MUST(ByteBuffer::create_zeroed(buffer_size));
        m_context->get_active_uniform_blockiv_robust_angle(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES, num_active_uniforms, nullptr, reinterpret_cast<GLint*>(active_uniform_indices_buffer.data()));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(active_uniform_indices_buffer));
        return JS::Uint32Array::create(realm(), num_active_uniforms, array_buffer);
    }
    case GL_UNIFORM_BLOCK_REFERENCED_BY_VERTEX_SHADER:
    case GL_UNIFORM_BLOCK_REFERENCED_BY_FRAGMENT_SHADER: {
        GLint result = 0;
        m_context->get_active_uniform_blockiv_robust_angle(program_handle, uniform_block_index, pname, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    default:
        dbgln("Unknown WebGL active uniform block parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

Optional<String> WebGL2RenderingContextImpl::get_active_uniform_block_name(GC::Ref<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return OptionalNone {};
    }
    auto program_handle = handle_or_error.release_value();

    GLint uniform_block_name_length = 0;
    m_context->get_active_uniform_blockiv_robust_angle(program_handle, uniform_block_index, GL_UNIFORM_BLOCK_NAME_LENGTH, 1, nullptr, &uniform_block_name_length);
    Vector<GLchar> uniform_block_name;
    uniform_block_name.resize(uniform_block_name_length);
    if (!uniform_block_name_length)
        return String {};
    m_context->get_active_uniform_block_name(program_handle, uniform_block_index, uniform_block_name_length, nullptr, uniform_block_name.data());
    return String::from_utf8_without_validation(ReadonlyBytes { uniform_block_name.data(), static_cast<size_t>(uniform_block_name_length - 1) });
}

void WebGL2RenderingContextImpl::uniform_block_binding(GC::Ref<WebGLProgram> program, WebIDL::UnsignedLong uniform_block_index, WebIDL::UnsignedLong uniform_block_binding)
{
    m_context->make_current();

    auto handle_or_error = program->handle(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto program_handle = handle_or_error.release_value();
    m_context->uniform_block_binding(program_handle, uniform_block_index, uniform_block_binding);
}

GC::Ref<WebGLVertexArrayObject> WebGL2RenderingContextImpl::create_vertex_array()
{
    m_context->make_current();

    GLuint handle = 0;
    m_context->gen_vertex_arrays(1, &handle);
    return WebGLVertexArrayObject::create(realm(), *this, handle);
}

void WebGL2RenderingContextImpl::delete_vertex_array(GC::Ptr<WebGLVertexArrayObject> vertex_array)
{
    m_context->make_current();

    if (!vertex_array)
        return;

    auto handle_or_error = vertex_array->handle_for_deletion(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }
    auto vertex_array_handle = handle_or_error.release_value();
    if (!vertex_array_handle.has_value())
        return;

    auto handle = vertex_array_handle.value();
    m_context->delete_vertex_arrays(1, &handle);
    if (m_current_vertex_array == vertex_array)
        m_current_vertex_array = nullptr;
}

bool WebGL2RenderingContextImpl::is_vertex_array(GC::Ptr<WebGLVertexArrayObject> vertex_array)
{
    m_context->make_current();

    if (!vertex_array)
        return false;

    auto handle_or_error = vertex_array->handle_for_query(this);
    if (handle_or_error.is_error()) {
        set_error(GL_INVALID_OPERATION);
        return false;
    }
    auto vertex_array_handle = handle_or_error.release_value();
    if (!vertex_array_handle.has_value())
        return false;
    return m_context->is_vertex_array(vertex_array_handle.value());
}

void WebGL2RenderingContextImpl::bind_vertex_array(GC::Ptr<WebGLVertexArrayObject> array)
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

    m_context->bind_vertex_array(array_handle);
    m_current_vertex_array = array;
}

void WebGL2RenderingContextImpl::compressed_tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(internalformat)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    m_context->compressed_tex_image3d_robust_angle(target, level, internalformat, width, height, depth, border, pixels.size(), pixels.size(), pixels.data());
}

void WebGL2RenderingContextImpl::compressed_tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, WebIDL::ArrayBufferView src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto pixels = SET_ERROR_VALUE_IF_ERROR(get_offset_span<u8 const>(src_data, src_offset, src_length_override), GL_INVALID_VALUE);
    m_context->compressed_tex_sub_image3d_robust_angle(target, level, xoffset, yoffset, zoffset, width, height, depth, format, pixels.size(), pixels.size(), pixels.data());
}

}
