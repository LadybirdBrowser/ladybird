/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
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

#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#include <core/SkColorSpace.h>
#include <core/SkColorType.h>
#include <core/SkImage.h>
#include <core/SkPixmap.h>

namespace Web::WebGL {

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

Optional<WebGLRenderingContextBase::ConvertedTexture> WebGLRenderingContextBase::read_and_pixel_convert_texture_image_source(TexImageSource const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width, Optional<int> destination_height)
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
        [](GC::Root<HTML::HTMLImageElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->immutable_bitmap();
        },
        [](GC::Root<HTML::HTMLCanvasElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            auto surface = source->surface();
            if (!surface)
                return {};
            auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, surface->size()));
            surface->read_into_bitmap(*bitmap);
            return Gfx::ImmutableBitmap::create(*bitmap);
        },
        [](GC::Root<HTML::OffscreenCanvas> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<HTML::HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<HTML::ImageBitmap> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return Gfx::ImmutableBitmap::create(*source->bitmap());
        },
        [](GC::Root<HTML::ImageData> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
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

}
