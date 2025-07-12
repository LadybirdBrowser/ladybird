/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
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
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>
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
    case GL_LUMINANCE:
    case GL_ALPHA:
        return 1;
    case GL_LUMINANCE_ALPHA:
        return 2;
    case GL_RGB:
        return 3;
    case GL_RGBA:
        return 4;
    default:
        return OptionalNone {};
    }
}

static constexpr Optional<int> opengl_type_size_in_bytes(WebIDL::UnsignedLong type)
{
    switch (type) {
    case GL_UNSIGNED_BYTE:
        return 1;
    case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_4_4_4_4:
    case GL_UNSIGNED_SHORT_5_5_5_1:
        return 2;
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
            dbgln("WebGL FIXME: Support conversion to RGBA5551.");
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

    dbgln("WebGL: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
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

WebGLRenderingContextImpl::WebGLRenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : m_realm(realm)
    , m_context(move(context))
{
}

void WebGLRenderingContextImpl::buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    glBufferData(target, size, 0, usage);
}

void WebGLRenderingContextImpl::buffer_data(WebIDL::UnsignedLong target, GC::Root<WebIDL::BufferSource> data, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    void const* ptr = nullptr;
    size_t byte_size = 0;
    if (data->is_typed_array_base()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*data->raw_object());
        ptr = typed_array_base.viewed_array_buffer()->buffer().data() + typed_array_base.byte_offset();
        byte_size = data->byte_length();
    } else if (data->is_data_view()) {
        auto& data_view = static_cast<JS::DataView&>(*data->raw_object());
        ptr = data_view.viewed_array_buffer()->buffer().data();
        byte_size = data_view.viewed_array_buffer()->byte_length();
    } else if (data->is_array_buffer()) {
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(*data->raw_object());
        ptr = array_buffer.buffer().data();
        byte_size = array_buffer.byte_length();
    } else {
        VERIFY_NOT_REACHED();
    }
    glBufferData(target, byte_size, ptr, usage);
}

void WebGLRenderingContextImpl::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong offset, GC::Root<WebIDL::BufferSource> data)
{
    m_context->make_current();

    void const* ptr = nullptr;
    size_t byte_size = 0;
    if (data->is_typed_array_base()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*data->raw_object());
        ptr = typed_array_base.viewed_array_buffer()->buffer().data() + typed_array_base.byte_offset();
        byte_size = data->byte_length();
    } else if (data->is_data_view()) {
        auto& data_view = static_cast<JS::DataView&>(*data->raw_object());
        ptr = data_view.viewed_array_buffer()->buffer().data();
        byte_size = data_view.viewed_array_buffer()->byte_length();
    } else if (data->is_array_buffer()) {
        auto& array_buffer = static_cast<JS::ArrayBuffer&>(*data->raw_object());
        ptr = array_buffer.buffer().data();
        byte_size = array_buffer.byte_length();
    } else {
        VERIFY_NOT_REACHED();
    }
    glBufferSubData(target, offset, byte_size, ptr);
}

void WebGLRenderingContextImpl::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> data)
{
    m_context->make_current();

    void const* ptr = data->viewed_array_buffer()->buffer().data() + data->byte_offset();
    size_t byte_size = data->byte_length();
    glCompressedTexImage2DRobustANGLE(target, level, internalformat, width, height, border, byte_size, byte_size, ptr);
}

void WebGLRenderingContextImpl::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> data)
{
    m_context->make_current();

    void const* ptr = data->viewed_array_buffer()->buffer().data() + data->byte_offset();
    size_t byte_size = data->byte_length();
    glCompressedTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, byte_size, byte_size, ptr);
}

void WebGLRenderingContextImpl::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (!pixels) {
        return;
    }

    void* ptr = pixels->viewed_array_buffer()->buffer().data() + pixels->byte_offset();
    glReadPixelsRobustANGLE(x, y, width, height, format, type, pixels->byte_length(), nullptr, nullptr, nullptr, ptr);
}

void WebGLRenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
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

void WebGLRenderingContextImpl::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>, GC::Root<OffscreenCanvas>> source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, 0, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGLRenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
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

void WebGLRenderingContextImpl::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Variant<GC::Root<ImageBitmap>, GC::Root<ImageData>, GC::Root<HTMLImageElement>, GC::Root<HTMLCanvasElement>, GC::Root<HTMLVideoElement>, GC::Root<OffscreenCanvas>> source)
{
    m_context->make_current();

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
}

void WebGLRenderingContextImpl::uniform1fv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> v)
{
    m_context->make_current();

    if (!location)
        return;

    float const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<float>>()) {
        auto& vector = v.get<Vector<float>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Float32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform1fv(location->handle(), count / 1, data);
}

void WebGLRenderingContextImpl::uniform2fv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> v)
{
    m_context->make_current();

    if (!location)
        return;

    float const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<float>>()) {
        auto& vector = v.get<Vector<float>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Float32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform2fv(location->handle(), count / 2, data);
}

void WebGLRenderingContextImpl::uniform3fv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> v)
{
    m_context->make_current();

    if (!location)
        return;

    float const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<float>>()) {
        auto& vector = v.get<Vector<float>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Float32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform3fv(location->handle(), count / 3, data);
}

void WebGLRenderingContextImpl::uniform4fv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> v)
{
    m_context->make_current();

    if (!location)
        return;

    float const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<float>>()) {
        auto& vector = v.get<Vector<float>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Float32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform4fv(location->handle(), count / 4, data);
}

void WebGLRenderingContextImpl::uniform1iv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<WebIDL::Long>> v)
{
    m_context->make_current();

    if (!location)
        return;

    int const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<int>>()) {
        auto& vector = v.get<Vector<int>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Int32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform1iv(location->handle(), count / 1, data);
}

void WebGLRenderingContextImpl::uniform2iv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<WebIDL::Long>> v)
{
    m_context->make_current();

    if (!location)
        return;

    int const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<int>>()) {
        auto& vector = v.get<Vector<int>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Int32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform2iv(location->handle(), count / 2, data);
}

void WebGLRenderingContextImpl::uniform3iv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<WebIDL::Long>> v)
{
    m_context->make_current();

    if (!location)
        return;

    int const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<int>>()) {
        auto& vector = v.get<Vector<int>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Int32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform3iv(location->handle(), count / 3, data);
}

void WebGLRenderingContextImpl::uniform4iv(GC::Root<WebGLUniformLocation> location, Variant<GC::Root<WebIDL::BufferSource>, Vector<WebIDL::Long>> v)
{
    m_context->make_current();

    if (!location)
        return;

    int const* data = nullptr;
    size_t count = 0;
    if (v.has<Vector<int>>()) {
        auto& vector = v.get<Vector<int>>();
        data = vector.data();
        count = vector.size();
    } else if (v.has<GC::Root<WebIDL::BufferSource>>()) {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*v.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& typed_array = as<JS::Int32Array>(typed_array_base);
        data = typed_array.data().data();
        count = typed_array.array_length().length();
    } else {
        VERIFY_NOT_REACHED();
    }

    glUniform4iv(location->handle(), count / 4, data);
}

void WebGLRenderingContextImpl::uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> value)
{
    m_context->make_current();

    if (!location)
        return;

    auto matrix_size = 2 * 2;
    float const* raw_data = nullptr;
    u64 count = 0;
    if (value.has<Vector<float>>()) {
        auto& vector_data = value.get<Vector<float>>();
        raw_data = vector_data.data();
        count = vector_data.size() / matrix_size;
    } else {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*value.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& float32_array = as<JS::Float32Array>(typed_array_base);
        raw_data = float32_array.data().data();
        count = float32_array.array_length().length() / matrix_size;
    }

    glUniformMatrix2fv(location->handle(), count, transpose, raw_data);
}

void WebGLRenderingContextImpl::uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> value)
{
    m_context->make_current();

    if (!location)
        return;

    auto matrix_size = 3 * 3;
    float const* raw_data = nullptr;
    u64 count = 0;
    if (value.has<Vector<float>>()) {
        auto& vector_data = value.get<Vector<float>>();
        raw_data = vector_data.data();
        count = vector_data.size() / matrix_size;
    } else {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*value.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& float32_array = as<JS::Float32Array>(typed_array_base);
        raw_data = float32_array.data().data();
        count = float32_array.array_length().length() / matrix_size;
    }

    glUniformMatrix3fv(location->handle(), count, transpose, raw_data);
}

void WebGLRenderingContextImpl::uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> value)
{
    m_context->make_current();

    if (!location)
        return;

    auto matrix_size = 4 * 4;
    float const* raw_data = nullptr;
    u64 count = 0;
    if (value.has<Vector<float>>()) {
        auto& vector_data = value.get<Vector<float>>();
        raw_data = vector_data.data();
        count = vector_data.size() / matrix_size;
    } else {
        auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*value.get<GC::Root<WebIDL::BufferSource>>()->raw_object());
        auto& float32_array = as<JS::Float32Array>(typed_array_base);
        raw_data = float32_array.data().data();
        count = float32_array.array_length().length() / matrix_size;
    }

    glUniformMatrix4fv(location->handle(), count, transpose, raw_data);
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
    }

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
    return WebGLBuffer::create(m_realm, *this, handle);
}

GC::Root<WebGLFramebuffer> WebGLRenderingContextImpl::create_framebuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenFramebuffers(1, &handle);
    return WebGLFramebuffer::create(m_realm, *this, handle);
}

GC::Root<WebGLProgram> WebGLRenderingContextImpl::create_program()
{
    m_context->make_current();
    return WebGLProgram::create(m_realm, *this, glCreateProgram());
}

GC::Root<WebGLRenderbuffer> WebGLRenderingContextImpl::create_renderbuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenRenderbuffers(1, &handle);
    return WebGLRenderbuffer::create(m_realm, *this, handle);
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
    return WebGLShader::create(m_realm, *this, handle, type);
}

GC::Root<WebGLTexture> WebGLRenderingContextImpl::create_texture()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenTextures(1, &handle);
    return WebGLTexture::create(m_realm, *this, handle);
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
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
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
    return WebGLActiveInfo::create(m_realm, String::from_utf8_without_validation(readonly_bytes), type, size);
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

JS::Value WebGLRenderingContextImpl::get_parameter(WebIDL::UnsignedLong pname)
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
    default:
        dbgln("Unknown WebGL parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

WebIDL::UnsignedLong WebGLRenderingContextImpl::get_error()
{
    m_context->make_current();
    return glGetError();
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
    return WebGLShaderPrecisionFormat::create(m_realm, range[0], range[1], precision);
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
    return WebGLUniformLocation::create(m_realm, glGetUniformLocation(program_handle, name_null_terminated.data()));
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

#define GL_DEPTH_STENCIL 0x84F9
#define GL_DEPTH24_STENCIL8 0x88F0

    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;

#undef GL_DEPTH_STENCIL
#undef GL_DEPTH24_STENCIL8

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
    glUniform1f(location ? location->handle() : 0, x);
}

void WebGLRenderingContextImpl::uniform2f(GC::Root<WebGLUniformLocation> location, float x, float y)
{
    m_context->make_current();
    glUniform2f(location ? location->handle() : 0, x, y);
}

void WebGLRenderingContextImpl::uniform3f(GC::Root<WebGLUniformLocation> location, float x, float y, float z)
{
    m_context->make_current();
    glUniform3f(location ? location->handle() : 0, x, y, z);
}

void WebGLRenderingContextImpl::uniform4f(GC::Root<WebGLUniformLocation> location, float x, float y, float z, float w)
{
    m_context->make_current();
    glUniform4f(location ? location->handle() : 0, x, y, z, w);
}

void WebGLRenderingContextImpl::uniform1i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x)
{
    m_context->make_current();
    glUniform1i(location ? location->handle() : 0, x);
}

void WebGLRenderingContextImpl::uniform2i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y)
{
    m_context->make_current();
    glUniform2i(location ? location->handle() : 0, x, y);
}

void WebGLRenderingContextImpl::uniform3i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z)
{
    m_context->make_current();
    glUniform3i(location ? location->handle() : 0, x, y, z);
}

void WebGLRenderingContextImpl::uniform4i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w)
{
    m_context->make_current();
    glUniform4i(location ? location->handle() : 0, x, y, z, w);
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

void WebGLRenderingContextImpl::vertex_attrib1fv(WebIDL::UnsignedLong index, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> values)
{
    m_context->make_current();

    if (values.has<Vector<float>>()) {
        auto& data = values.get<Vector<float>>();
        if (data.size() < 1) {
            set_error(GL_INVALID_VALUE);
            return;
        }

        glVertexAttrib1fv(index, data.data());
        return;
    }

    auto& buffer_source = values.get<GC::Root<WebIDL::BufferSource>>();
    if (buffer_source->byte_length() < 1 * sizeof(float)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*buffer_source->raw_object());
    auto& float32_array = as<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    glVertexAttrib1fv(index, data);
}

void WebGLRenderingContextImpl::vertex_attrib2fv(WebIDL::UnsignedLong index, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> values)
{
    m_context->make_current();

    if (values.has<Vector<float>>()) {
        auto& data = values.get<Vector<float>>();
        if (data.size() < 2) {
            set_error(GL_INVALID_VALUE);
            return;
        }

        glVertexAttrib2fv(index, data.data());
        return;
    }

    auto& buffer_source = values.get<GC::Root<WebIDL::BufferSource>>();
    if (buffer_source->byte_length() < 2 * sizeof(float)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*buffer_source->raw_object());
    auto& float32_array = as<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    glVertexAttrib2fv(index, data);
}

void WebGLRenderingContextImpl::vertex_attrib3fv(WebIDL::UnsignedLong index, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> values)
{
    m_context->make_current();

    if (values.has<Vector<float>>()) {
        auto& data = values.get<Vector<float>>();
        if (data.size() < 3) {
            set_error(GL_INVALID_VALUE);
            return;
        }

        glVertexAttrib3fv(index, data.data());
        return;
    }

    auto& buffer_source = values.get<GC::Root<WebIDL::BufferSource>>();
    if (buffer_source->byte_length() < 3 * sizeof(float)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*buffer_source->raw_object());
    auto& float32_array = as<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    glVertexAttrib3fv(index, data);
}

void WebGLRenderingContextImpl::vertex_attrib4fv(WebIDL::UnsignedLong index, Variant<GC::Root<WebIDL::BufferSource>, Vector<float>> values)
{
    m_context->make_current();

    if (values.has<Vector<float>>()) {
        auto& data = values.get<Vector<float>>();
        if (data.size() < 4) {
            set_error(GL_INVALID_VALUE);
            return;
        }

        glVertexAttrib4fv(index, data.data());
        return;
    }

    auto& buffer_source = values.get<GC::Root<WebIDL::BufferSource>>();
    if (buffer_source->byte_length() < 4 * sizeof(float)) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto& typed_array_base = static_cast<JS::TypedArrayBase&>(*buffer_source->raw_object());
    auto& float32_array = as<JS::Float32Array>(typed_array_base);
    float const* data = float32_array.data().data();
    glVertexAttrib4fv(index, data);
}

void WebGLRenderingContextImpl::vertex_attrib_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, bool normalized, WebIDL::Long stride, WebIDL::LongLong offset)
{
    m_context->make_current();

    glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<void*>(offset));
}

void WebGLRenderingContextImpl::viewport(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glViewport(x, y, width, height);
}

void WebGLRenderingContextImpl::visit_edges(JS::Cell::Visitor& visitor)
{
    visitor.visit(m_realm);
    visitor.visit(m_array_buffer_binding);
    visitor.visit(m_element_array_buffer_binding);
    visitor.visit(m_current_program);
    visitor.visit(m_framebuffer_binding);
    visitor.visit(m_renderbuffer_binding);
    visitor.visit(m_texture_binding_2d);
    visitor.visit(m_texture_binding_cube_map);
}

}
