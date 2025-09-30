/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_GLEXT_PROTOTYPES 1
#include <GLES3/gl3.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGL2RenderingContextImpl.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLSampler.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>
#include <LibWeb/WebGL/WebGLSync.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <core/SkColorSpace.h>
#include <core/SkColorType.h>
#include <core/SkImage.h>
#include <core/SkPixmap.h>

namespace Web::WebGL {

static Vector<GLchar> null_terminated_string(StringView string)
{
    Vector<GLchar> result;
    for (auto c : string.bytes())
        result.append(c);
    result.append('\0');
    return result;
}

static constexpr Optional<int> opengl_format_number_of_components(WebIDL::UnsignedLong format)
{
    switch (format) {
    case GL_RED:
    case GL_RED_INTEGER:
    case GL_LUMINANCE:
    case GL_ALPHA:
    case GL_DEPTH_COMPONENT:
        return 1;
    case GL_RG:
    case GL_RG_INTEGER:
    case GL_DEPTH_STENCIL:
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
    case GL_RGB_INTEGER:
        return 3;
    case GL_RGBA:
    case GL_RGBA_INTEGER:
        return 4;
    default:
        return OptionalNone {};
    }
}

static constexpr Optional<int> opengl_type_size_in_bytes(WebIDL::UnsignedLong type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
    case GL_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT:
    case GL_SHORT:
    case GL_HALF_FLOAT:
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
    case GL_UNSIGNED_INT:
    case GL_INT:
    case GL_UNSIGNED_INT_2_10_10_10_REV:
    case GL_UNSIGNED_INT_10F_11F_11F_REV:
    case GL_UNSIGNED_INT_5_9_9_9_REV:
    case GL_UNSIGNED_INT_24_8:
        return 4;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return 8;
    default:
        return OptionalNone {};
    }
}

static constexpr SkColorType opengl_format_and_type_to_skia_color_type(WebIDL::UnsignedLong format, WebIDL::UnsignedLong type)
{
    switch (format) {
    case GL_RGB:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kRGB_888x_SkColorType;
        case GL_UNSIGNED_SHORT_5_6_5:
            return SkColorType::kRGB_565_SkColorType;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kRGBA_8888_SkColorType;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            // FIXME: This is not exactly the same as RGBA.
            return SkColorType::kARGB_4444_SkColorType;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            dbgln("WebGL2 FIXME: Support conversion to RGBA5551.");
            break;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kAlpha_8_SkColorType;
        default:
            break;
        }
        break;
    case GL_LUMINANCE:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return SkColorType::kGray_8_SkColorType;
        default:
            break;
        }
        break;
    default:
        break;
    }

    dbgln("WebGL2: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
    return SkColorType::kUnknown_SkColorType;
}

struct ConvertedTexture {
    ByteBuffer buffer;
    int width { 0 };
    int height { 0 };
};

static Optional<ConvertedTexture> read_and_pixel_convert_texture_image_source(TexImageSource const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width = OptionalNone {}, Optional<int> destination_height = OptionalNone {})
{
    // FIXME: If this function is called with an ImageData whose data attribute has been neutered,
    //        an INVALID_VALUE error is generated.
    // FIXME: If this function is called with an ImageBitmap that has been neutered, an INVALID_VALUE
    //        error is generated.
    // FIXME: If this function is called with an HTMLImageElement or HTMLVideoElement whose origin
    //        differs from the origin of the containing Document, or with an HTMLCanvasElement,
    //        ImageBitmap or OffscreenCanvas whose bitmap's origin-clean flag is set to false,
    //        a SECURITY_ERR exception must be thrown. See Origin Restrictions.
    // FIXME: If source is null then an INVALID_VALUE error is generated.
    auto bitmap = source.visit(
        [](GC::Root<HTMLImageElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->immutable_bitmap();
        },
        [](GC::Root<HTMLCanvasElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            auto surface = source->surface();
            if (!surface)
                return {};
            auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, surface->size()));
            surface->read_into_bitmap(*bitmap);
            return Gfx::ImmutableBitmap::create(*bitmap);
        },
        [](GC::Root<OffscreenCanvas> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<ImageBitmap> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<ImageData> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(source->bitmap());
        });
    if (!bitmap)
        return OptionalNone {};

    int width = destination_width.value_or(bitmap->width());
    int height = destination_height.value_or(bitmap->height());

    Checked<size_t> buffer_pitch = width;

    auto number_of_components = opengl_format_number_of_components(format);
    if (!number_of_components.has_value())
        return OptionalNone {};

    buffer_pitch *= number_of_components.value();

    auto type_size = opengl_type_size_in_bytes(type);
    if (!type_size.has_value())
        return OptionalNone {};

    buffer_pitch *= type_size.value();

    if (buffer_pitch.has_overflow())
        return OptionalNone {};

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return OptionalNone {};

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    auto skia_format = opengl_format_and_type_to_skia_color_type(format, type);

    // FIXME: Respect UNPACK_PREMULTIPLY_ALPHA_WEBGL
    // FIXME: Respect unpackColorSpace
    auto color_space = SkColorSpace::MakeSRGB();
    auto image_info = SkImageInfo::Make(width, height, skia_format, SkAlphaType::kPremul_SkAlphaType, color_space);
    SkPixmap const pixmap(image_info, buffer.data(), buffer_pitch.value());
    bitmap->sk_image()->readPixels(pixmap, 0, 0);
    return ConvertedTexture {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
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

void WebGL2RenderingContextImpl::blit_framebuffer(WebIDL::Long src_x0, WebIDL::Long src_y0, WebIDL::Long src_x1, WebIDL::Long src_y1, WebIDL::Long dst_x0, WebIDL::Long dst_y0, WebIDL::Long dst_x1, WebIDL::Long dst_y1, WebIDL::UnsignedLong mask, WebIDL::UnsignedLong filter)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glBlitFramebuffer(src_x0, src_y0, src_x1, src_y1, dst_x0, dst_y0, dst_x1, dst_y1, mask, filter);
}

void WebGL2RenderingContextImpl::invalidate_framebuffer(WebIDL::UnsignedLong target, Vector<WebIDL::UnsignedLong> attachments)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    glInvalidateFramebuffer(target, attachments.size(), attachments.data());
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

    void const* src_data_ptr = nullptr;
    size_t buffer_size = 0;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        src_data_ptr = byte_buffer.data() + src_data->byte_offset();
        buffer_size = src_data->byte_length();
    }
    glTexImage3DRobustANGLE(target, level, internalformat, width, height, depth, border, format, type, buffer_size, src_data_ptr);
}

void WebGL2RenderingContextImpl::tex_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    void const* src_data_ptr = nullptr;
    size_t buffer_size = 0;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        src_data_ptr = byte_buffer.data() + src_offset;
        buffer_size = src_data->byte_length();
    }
    glTexImage3DRobustANGLE(target, level, internalformat, width, height, depth, border, format, type, buffer_size, src_data_ptr);
}

void WebGL2RenderingContextImpl::tex_sub_image3d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long zoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::Long depth, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();

    void const* pixels_ptr = nullptr;
    size_t buffer_size = 0;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + src_offset;
        buffer_size = src_data->byte_length();
    }
    glTexSubImage3DRobustANGLE(target, level, xoffset, yoffset, zoffset, width, height, depth, format, type, buffer_size, pixels_ptr);
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

void WebGL2RenderingContextImpl::draw_buffers(Vector<WebIDL::UnsignedLong> buffers)
{
    m_context->make_current();

    glDrawBuffers(buffers.size(), buffers.data());
}

void WebGL2RenderingContextImpl::clear_bufferfv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Float32List values, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    auto span = span_from_float32_list(values);

    switch (buffer) {
    case GL_COLOR:
        if (src_offset + 4 > span.size()) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (src_offset + 1 > span.size()) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    span = span.slice(src_offset);
    glClearBufferfv(buffer, drawbuffer, span.data());
    needs_to_present();
}

void WebGL2RenderingContextImpl::clear_bufferiv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Int32List values, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    auto span = span_from_int32_list(values);
    auto count = span.size();

    switch (buffer) {
    case GL_COLOR:
        if (src_offset + 4 > count) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (src_offset + 1 > count) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    span = span.slice(src_offset);
    glClearBufferiv(buffer, drawbuffer, span.data());
    needs_to_present();
}

void WebGL2RenderingContextImpl::clear_bufferuiv(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, Variant<GC::Root<WebIDL::BufferSource>, Vector<WebIDL::UnsignedLong>> values, WebIDL::UnsignedLongLong src_offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    u32 const* data = nullptr;
    size_t count = 0;
    if (values.has<Vector<u32>>()) {
        auto& vector = values.get<Vector<u32>>();
        data = vector.data();
        count = vector.size();
    } else if (values.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*values.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Uint32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    switch (buffer) {
    case GL_COLOR:
        if (src_offset + 4 > count) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    case GL_DEPTH:
    case GL_STENCIL:
        if (src_offset + 1 > count) {
            set_error(GL_INVALID_VALUE);
            return;
        }
        break;
    default:
        dbgln("Unknown WebGL buffer target for buffer clearing: 0x{:04x}", buffer);
        set_error(GL_INVALID_ENUM);
        return;
    }

    data += src_offset;
    glClearBufferuiv(buffer, drawbuffer, data);
    needs_to_present();
}

void WebGL2RenderingContextImpl::clear_bufferfi(WebIDL::UnsignedLong buffer, WebIDL::Long drawbuffer, float depth, WebIDL::Long stencil)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glClearBufferfi(buffer, drawbuffer, depth, stencil);
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
    glDeleteSync((GLsync)(sync ? sync->sync_handle() : nullptr));
}

WebIDL::UnsignedLong WebGL2RenderingContextImpl::client_wait_sync(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong flags, WebIDL::UnsignedLongLong timeout)
{
    m_context->make_current();
    return glClientWaitSync((GLsync)(sync ? sync->sync_handle() : nullptr), flags, timeout);
}

JS::Value WebGL2RenderingContextImpl::get_sync_parameter(GC::Root<WebGLSync> sync, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLint result = 0;
    glGetSynciv((GLsync)(sync ? sync->sync_handle() : nullptr), pname, 1, nullptr, &result);
    return JS::Value(result);
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

    void const* ptr = nullptr;
    size_t byte_size = 0;
    if (src_data->is_typed_array_base()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*src_data->raw_object());
        ptr = typed_array_base.viewed_array_buffer()->buffer().data() + typed_array_base.byte_offset();
        byte_size = src_data->byte_length();
    } else if (src_data->is_data_view()) {
        auto& data_view = static_cast<JS::DataView&>(*src_data->raw_object());
        ptr = data_view.viewed_array_buffer()->buffer().data();
        byte_size = data_view.viewed_array_buffer()->byte_length();
    } else if (src_data->is_array_buffer()) {
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(*src_data->raw_object());
        ptr = array_buffer.buffer().data();
        byte_size = array_buffer.byte_length();
    } else {
        VERIFY_NOT_REACHED();
    }
    glBufferData(target, byte_size, ptr, usage);
}

void WebGL2RenderingContextImpl::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, GC::Root<WebIDL::BufferSource> src_data)
{
    m_context->make_current();

    void const* ptr = nullptr;
    size_t byte_size = 0;
    if (src_data->is_typed_array_base()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*src_data->raw_object());
        ptr = typed_array_base.viewed_array_buffer()->buffer().data() + typed_array_base.byte_offset();
        byte_size = src_data->byte_length();
    } else if (src_data->is_data_view()) {
        auto& data_view = static_cast<JS::DataView&>(*src_data->raw_object());
        ptr = data_view.viewed_array_buffer()->buffer().data();
        byte_size = data_view.viewed_array_buffer()->byte_length();
    } else if (src_data->is_array_buffer()) {
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(*src_data->raw_object());
        ptr = array_buffer.buffer().data();
        byte_size = array_buffer.byte_length();
    } else {
        VERIFY_NOT_REACHED();
    }
    glBufferSubData(target, dst_byte_offset, byte_size, ptr);
}

void WebGL2RenderingContextImpl::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLong usage, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    VERIFY(src_data);
    auto const& viewed_array_buffer = src_data->viewed_array_buffer();
    auto const& byte_buffer = viewed_array_buffer->buffer();
    auto src_data_length = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();
    u8 const* buffer_ptr = byte_buffer.data();

    u64 copy_length = length == 0 ? src_data_length - src_offset : length;
    copy_length *= src_data_element_size;

    if (src_offset > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    if (src_offset + copy_length > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    buffer_ptr += src_offset * src_data_element_size;
    glBufferData(target, copy_length, buffer_ptr, usage);
}

void WebGL2RenderingContextImpl::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong dst_byte_offset, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong length)
{
    m_context->make_current();

    VERIFY(src_data);
    auto const& viewed_array_buffer = src_data->viewed_array_buffer();
    auto const& byte_buffer = viewed_array_buffer->buffer();
    auto src_data_length = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();
    u8 const* buffer_ptr = byte_buffer.data();

    u64 copy_length = length == 0 ? src_data_length - src_offset : length;
    copy_length *= src_data_element_size;

    if (src_offset > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    if (src_offset + copy_length > src_data_length) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    buffer_ptr += src_offset * src_data_element_size;
    glBufferSubData(target, dst_byte_offset, copy_length, buffer_ptr);
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    void const* pixels_ptr = nullptr;
    size_t buffer_size = 0;
    if (pixels) {
        auto const& viewed_array_buffer = pixels->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + pixels->byte_offset();
        buffer_size = pixels->byte_length();
    }
    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, buffer_size, pixels_ptr);
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>, GC::Root<OffscreenCanvas>> source)
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

    void const* pixels_ptr = nullptr;
    size_t buffer_size = 0;
    if (pixels) {
        auto const& viewed_array_buffer = pixels->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + pixels->byte_offset();
        buffer_size = pixels->byte_length();
    }
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, buffer_size, pixels_ptr);
}

void WebGL2RenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>, GC::Root<OffscreenCanvas>> source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGL2RenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>, GC::Root<OffscreenCanvas>> source)
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

    void const* pixels_ptr = nullptr;
    size_t buffer_size = 0;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + src_offset;
        buffer_size = src_data->byte_length();
    }
    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, buffer_size, pixels_ptr);
}

void WebGL2RenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>, GC::Root<OffscreenCanvas>> source)
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

    void const* pixels_ptr = nullptr;
    size_t buffer_size = 0;
    if (src_data) {
        auto const& viewed_array_buffer = src_data->viewed_array_buffer();
        auto const& byte_buffer = viewed_array_buffer->buffer();
        pixels_ptr = byte_buffer.data() + src_data->byte_offset() + src_offset;
        buffer_size = src_data->byte_length();
    }
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, buffer_size, pixels_ptr);
}

void WebGL2RenderingContextImpl::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    u8 const* pixels_ptr = src_data->viewed_array_buffer()->buffer().data();
    size_t count = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();

    if ((src_offset * src_data_element_size) + src_length_override > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    pixels_ptr += src_data->byte_offset();
    pixels_ptr += src_offset * src_data_element_size;
    if (src_length_override == 0) {
        count -= src_offset;
    } else {
        count = src_length_override;
    }

    glCompressedTexImage2DRobustANGLE(target, level, internalformat, width, height, border, count, src_data->byte_length(), pixels_ptr);
}

void WebGL2RenderingContextImpl::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> src_data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length_override)
{
    m_context->make_current();

    u8 const* pixels_ptr = src_data->viewed_array_buffer()->buffer().data();
    size_t count = src_data->byte_length();
    auto src_data_element_size = src_data->element_size();

    if ((src_offset * src_data_element_size) + src_length_override > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    pixels_ptr += src_data->byte_offset();
    pixels_ptr += src_offset * src_data_element_size;
    if (src_length_override == 0) {
        count -= src_offset;
    } else {
        count = src_length_override;
    }

    glCompressedTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, count, src_data->byte_length(), pixels_ptr);
}

void WebGL2RenderingContextImpl::uniform1fv(GC::Root<WebGLUniformLocation> location, Float32List values, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_float32_list(values);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform1fv(location->handle(), count, span.data());
}

void WebGL2RenderingContextImpl::uniform2fv(GC::Root<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_float32_list(v);
    auto count = span.size();
    if (src_offset + src_length > span.size()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform2fv(location->handle(), count / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3fv(GC::Root<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_float32_list(v);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform3fv(location->handle(), count / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4fv(GC::Root<WebGLUniformLocation> location, Float32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_float32_list(v);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform4fv(location->handle(), count / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform1iv(GC::Root<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_int32_list(v);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform1iv(location->handle(), count / 1, span.data());
}

void WebGL2RenderingContextImpl::uniform2iv(GC::Root<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_int32_list(v);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform2iv(location->handle(), count / 2, span.data());
}

void WebGL2RenderingContextImpl::uniform3iv(GC::Root<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_int32_list(v);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform3iv(location->handle(), count / 3, span.data());
}

void WebGL2RenderingContextImpl::uniform4iv(GC::Root<WebGLUniformLocation> location, Int32List v, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto span = span_from_int32_list(v);
    auto count = span.size();
    if (src_offset + src_length > count) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniform4iv(location->handle(), count / 4, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto matrix_size = 2 * 2;
    auto span = span_from_float32_list(data);
    auto count = span.size() / matrix_size;

    if (src_offset + src_length > (count * matrix_size)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniformMatrix2fv(location->handle(), count, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto matrix_size = 3 * 3;
    auto span = span_from_float32_list(data);
    auto count = span.size() / matrix_size;

    if (src_offset + src_length > (count * matrix_size)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniformMatrix3fv(location->handle(), count, transpose, span.data());
}

void WebGL2RenderingContextImpl::uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List data, WebIDL::UnsignedLongLong src_offset, WebIDL::UnsignedLong src_length)
{
    m_context->make_current();

    if (!location)
        return;

    auto matrix_size = 4 * 4;
    auto span = span_from_float32_list(data);
    auto count = span.size() / matrix_size;

    if (src_offset + src_length > (count * matrix_size)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    span = span.slice(src_offset);
    if (src_length == 0) {
        count -= src_offset;
    } else {
        count = src_length;
    }

    glUniformMatrix4fv(location->handle(), count, transpose, span.data());
}

void WebGL2RenderingContextImpl::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (!pixels) {
        return;
    }

    void* ptr = pixels->viewed_array_buffer()->buffer().data() + pixels->byte_offset();
    glReadPixelsRobustANGLE(x, y, width, height, format, type, pixels->byte_length(), nullptr, nullptr, nullptr, ptr);
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
    case GL_ELEMENT_ARRAY_BUFFER:
        m_element_array_buffer_binding = buffer;
        break;
    case GL_ARRAY_BUFFER:
        m_array_buffer_binding = buffer;
        break;

    case GL_UNIFORM_BUFFER:
        m_uniform_buffer_binding = buffer;
        break;
    case GL_COPY_READ_BUFFER:
        m_copy_read_buffer_binding = buffer;
        break;
    case GL_COPY_WRITE_BUFFER:
        m_copy_write_buffer_binding = buffer;
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
    return WebGLUniformLocation::create(m_realm, glGetUniformLocation(program_handle, name_null_terminated.data()));
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

    auto span = span_from_float32_list(values);
    if (span.size() < 1) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib1fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib2fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = span_from_float32_list(values);
    if (span.size() < 2) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib2fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib3fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = span_from_float32_list(values);
    if (span.size() < 3) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib3fv(index, span.data());
}

void WebGL2RenderingContextImpl::vertex_attrib4fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = span_from_float32_list(values);
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
    visitor.visit(m_texture_binding_2d_array);
    visitor.visit(m_texture_binding_3d);
}

}
