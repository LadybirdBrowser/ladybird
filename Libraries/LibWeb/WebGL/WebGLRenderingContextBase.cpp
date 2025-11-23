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

#include <LibGfx/ImmutableBitmap.h>
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

static constexpr Optional<Gfx::ExportFormat> determine_export_format(WebIDL::UnsignedLong format, WebIDL::UnsignedLong type)
{
    switch (format) {
    case GL_RGB:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::RGB888;
        case GL_UNSIGNED_SHORT_5_6_5:
            return Gfx::ExportFormat::RGB565;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::RGBA8888;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            // FIXME: This is not exactly the same as RGBA.
            return Gfx::ExportFormat::RGBA4444;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return Gfx::ExportFormat::RGBA5551;
            break;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::Alpha8;
        default:
            break;
        }
        break;
    case GL_LUMINANCE:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::Gray8;
        default:
            break;
        }
        break;
    default:
        break;
    }

    dbgln("WebGL: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
    return {};
}

Optional<Gfx::BitmapExportResult> WebGLRenderingContextBase::read_and_pixel_convert_texture_image_source(TexImageSource const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width, Optional<int> destination_height)
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

    auto export_format = determine_export_format(format, type);
    if (!export_format.has_value())
        return OptionalNone {};

    // FIXME: Respect unpackColorSpace
    auto export_flags = 0;
    if (m_unpack_flip_y && !source.has<GC::Root<HTML::ImageBitmap>>())
        // The first pixel transferred from the source to the WebGL implementation corresponds to the upper left corner of
        // the source. This behavior is modified by the UNPACK_FLIP_Y_WEBGL pixel storage parameter, except for ImageBitmap
        // arguments, as described in the abovementioned section.
        export_flags |= Gfx::ExportFlags::FlipY;
    if (m_unpack_premultiply_alpha)
        export_flags |= Gfx::ExportFlags::PremultiplyAlpha;

    auto result = bitmap->export_to_byte_buffer(export_format.value(), export_flags, destination_width, destination_height);
    if (result.is_error()) {
        dbgln("Could not export bitmap: {}", result.release_error());
        return OptionalNone {};
    }

    return result.release_value();
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
