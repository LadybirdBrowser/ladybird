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

#include <LibGfx/SkiaUtils.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkColorType.h>
#include <core/SkImage.h>
#include <core/SkPixmap.h>
#include <core/SkSurface.h>

namespace Web::WebGL {

static constexpr Optional<int> opengl_format_and_type_number_of_bytes(WebIDL::UnsignedLong format, WebIDL::UnsignedLong type)
{
    switch (format) {
    case GL_LUMINANCE:
    case GL_ALPHA:
        if (type != GL_UNSIGNED_BYTE)
            return OptionalNone {};

        return 1;
    case GL_LUMINANCE_ALPHA:
        if (type != GL_UNSIGNED_BYTE)
            return OptionalNone {};

        return 2;
    case GL_RGB:
        if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT_5_6_5)
            return OptionalNone {};

        return type == GL_UNSIGNED_BYTE ? 3 : 2;
    case GL_RGBA:
        if (type != GL_UNSIGNED_BYTE && type != GL_UNSIGNED_SHORT_4_4_4_4 && type != GL_UNSIGNED_SHORT_5_5_5_1)
            return OptionalNone {};

        return type == GL_UNSIGNED_BYTE ? 4 : 2;
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
                return Gfx::ImmutableBitmap::create(*source->get_bitmap_from_surface());
            return Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface);
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

    auto number_of_bytes = opengl_format_and_type_number_of_bytes(format, type);
    if (!number_of_bytes.has_value())
        return OptionalNone {};

    buffer_pitch *= number_of_bytes.value();

    if (buffer_pitch.has_overflow())
        return OptionalNone {};

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return OptionalNone {};

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    if (width > 0 && height > 0) {
        // FIXME: Respect unpackColorSpace
        auto skia_format = opengl_format_and_type_to_skia_color_type(format, type);
        auto color_space = SkColorSpace::MakeSRGB();
        auto image_info = SkImageInfo::Make(width, height, skia_format, m_unpack_premultiply_alpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType, color_space);
        auto surface = SkSurfaces::WrapPixels(image_info, buffer.data(), buffer_pitch.value());
        VERIFY(surface);
        auto surface_canvas = surface->getCanvas();
        auto dst_rect = Gfx::to_skia_rect(Gfx::Rect { 0, 0, width, height });

        // The first pixel transferred from the source to the WebGL implementation corresponds to the upper left corner of
        // the source. This behavior is modified by the UNPACK_FLIP_Y_WEBGL pixel storage parameter, except for ImageBitmap
        // arguments, as described in the abovementioned section.
        if (m_unpack_flip_y && !source.has<GC::Root<HTML::ImageBitmap>>()) {
            surface_canvas->translate(0, dst_rect.height());
            surface_canvas->scale(1, -1);
        }

        surface_canvas->drawImageRect(bitmap->sk_image(), dst_rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor));
    } else {
        VERIFY(buffer.is_empty());
    }

    return ConvertedTexture {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
}

// TODO: The glGetError spec allows for queueing errors which is something we should probably do, for now
//       this just keeps track of one error which is also fine by the spec
GLenum WebGLRenderingContextBase::get_error_value()
{
    if (m_error == GL_NO_ERROR)
        return glGetError();

    auto error = m_error;
    m_error = GL_NO_ERROR;
    return error;
}

void WebGLRenderingContextBase::set_error(GLenum error)
{
    if (m_error != GL_NO_ERROR)
        return;

    auto context_error = glGetError();
    if (context_error != GL_NO_ERROR)
        m_error = context_error;
    else
        m_error = error;
}

}
