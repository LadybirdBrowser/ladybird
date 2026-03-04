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
#include <LibWeb/WebGL/Extensions/ANGLEInstancedArrays.h>
#include <LibWeb/WebGL/Extensions/EXTBlendMinMax.h>
#include <LibWeb/WebGL/Extensions/EXTColorBufferFloat.h>
#include <LibWeb/WebGL/Extensions/EXTRenderSnorm.h>
#include <LibWeb/WebGL/Extensions/EXTTextureFilterAnisotropic.h>
#include <LibWeb/WebGL/Extensions/EXTTextureNorm16.h>
#include <LibWeb/WebGL/Extensions/OESElementIndexUint.h>
#include <LibWeb/WebGL/Extensions/OESStandardDerivatives.h>
#include <LibWeb/WebGL/Extensions/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tcSrgb.h>
#include <LibWeb/WebGL/Extensions/WebGLDrawBuffers.h>
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

WebGLRenderingContextBase::WebGLRenderingContextBase(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

Optional<Vector<String>> WebGLRenderingContextBase::get_supported_extensions()
{
    return context().get_supported_extensions();
}

JS::Object* WebGLRenderingContextBase::get_extension(String const& name)
{
    // Returns an object if, and only if, name is an ASCII case-insensitive match [HTML] for one of the names returned
    // from getSupportedExtensions; otherwise, returns null. The object returned from getExtension contains any constants
    // or functions provided by the extension. A returned object may have no constants or functions if the extension does
    // not define any, but a unique object must still be returned. That object is used to indicate that the extension has
    // been enabled.
    auto supported_extensions = get_supported_extensions();
    auto supported_extension_iterator = supported_extensions->find_if([&name](String const& supported_extension) {
        return supported_extension.equals_ignoring_ascii_case(name);
    });
    if (supported_extension_iterator == supported_extensions->end())
        return nullptr;

    if (name.equals_ignoring_ascii_case("ANGLE_instanced_arrays"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL1) {
        if (!m_angle_instanced_arrays_extension) {
            m_angle_instanced_arrays_extension = MUST(Extensions::ANGLEInstancedArrays::create(realm(), *this));
        }

        VERIFY(m_angle_instanced_arrays_extension);
        return m_angle_instanced_arrays_extension;
    }

    if (name.equals_ignoring_ascii_case("EXT_blend_minmax"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL1) {
        if (!m_ext_blend_min_max_extension) {
            m_ext_blend_min_max_extension = MUST(Extensions::EXTBlendMinMax::create(realm(), *this));
        }

        VERIFY(m_ext_blend_min_max_extension);
        return m_ext_blend_min_max_extension;
    }

    if (name.equals_ignoring_ascii_case("EXT_color_buffer_float"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        if (!m_ext_color_buffer_float_extension) {
            m_ext_color_buffer_float_extension = MUST(Extensions::EXTColorBufferFloat::create(realm(), *this));
        }

        VERIFY(m_ext_color_buffer_float_extension);
        return m_ext_color_buffer_float_extension;
    }

    if (name.equals_ignoring_ascii_case("EXT_render_snorm"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        if (!m_ext_render_snorm) {
            m_ext_render_snorm = MUST(Extensions::EXTRenderSnorm::create(realm(), *this));
        }

        VERIFY(m_ext_render_snorm);
        return m_ext_render_snorm;
    }

    if (name.equals_ignoring_ascii_case("EXT_texture_filter_anisotropic"sv)) {
        if (!m_ext_texture_filter_anisotropic) {
            m_ext_texture_filter_anisotropic = MUST(Extensions::EXTTextureFilterAnisotropic::create(realm(), *this));
        }

        VERIFY(m_ext_texture_filter_anisotropic);
        return m_ext_texture_filter_anisotropic;
    }

    if (name.equals_ignoring_ascii_case("EXT_texture_norm16"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        if (!m_ext_texture_norm16) {
            m_ext_texture_norm16 = MUST(Extensions::EXTTextureNorm16::create(realm(), *this));
        }

        VERIFY(m_ext_texture_norm16);
        return m_ext_texture_norm16;
    }

    if (name.equals_ignoring_ascii_case("OES_element_index_uint"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL1) {
        if (!m_oes_element_index_uint_object_extension) {
            m_oes_element_index_uint_object_extension = MUST(Extensions::OESElementIndexUint::create(realm(), *this));
        }

        VERIFY(m_oes_element_index_uint_object_extension);
        return m_oes_element_index_uint_object_extension;
    }

    if (name.equals_ignoring_ascii_case("OES_standard_derivatives"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL1) {
        if (!m_oes_standard_derivatives_object_extension) {
            m_oes_standard_derivatives_object_extension = MUST(Extensions::OESStandardDerivatives::create(realm(), *this));
        }

        VERIFY(m_oes_standard_derivatives_object_extension);
        return m_oes_standard_derivatives_object_extension;
    }

    if (name.equals_ignoring_ascii_case("OES_vertex_array_object"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL1) {
        if (!m_oes_vertex_array_object_extension) {
            m_oes_vertex_array_object_extension = MUST(Extensions::OESVertexArrayObject::create(realm(), *this));
        }

        VERIFY(m_oes_vertex_array_object_extension);
        return m_oes_vertex_array_object_extension;
    }

    if (name.equals_ignoring_ascii_case("WEBGL_compressed_texture_s3tc"sv)) {
        if (!m_webgl_compressed_texture_s3tc_extension) {
            m_webgl_compressed_texture_s3tc_extension = MUST(Extensions::WebGLCompressedTextureS3tc::create(realm(), *this));

            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGB_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGBA_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGBA_S3TC_DXT3_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_RGBA_S3TC_DXT5_EXT);
        }

        VERIFY(m_webgl_compressed_texture_s3tc_extension);
        return m_webgl_compressed_texture_s3tc_extension;
    }

    if (name.equals_ignoring_ascii_case("WEBGL_compressed_texture_s3tc_srgb"sv)) {
        if (!m_webgl_compressed_texture_s3tc_srgb_extension) {
            m_webgl_compressed_texture_s3tc_srgb_extension = MUST(Extensions::WebGLCompressedTextureS3tcSrgb::create(realm(), *this));

            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT);
            m_enabled_compressed_texture_formats.append(GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT);
        }

        VERIFY(m_webgl_compressed_texture_s3tc_srgb_extension);
        return m_webgl_compressed_texture_s3tc_srgb_extension;
    }

    if (name.equals_ignoring_ascii_case("WEBGL_draw_buffers"sv) && context().webgl_version() == OpenGLContext::WebGLVersion::WebGL1) {
        if (!m_webgl_draw_buffers_extension) {
            m_webgl_draw_buffers_extension = MUST(Extensions::WebGLDrawBuffers::create(realm(), *this));
        }

        VERIFY(m_webgl_draw_buffers_extension);
        return m_webgl_draw_buffers_extension;
    }

    return nullptr;
}

void WebGLRenderingContextBase::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_angle_instanced_arrays_extension);
    visitor.visit(m_ext_blend_min_max_extension);
    visitor.visit(m_ext_color_buffer_float_extension);
    visitor.visit(m_ext_render_snorm);
    visitor.visit(m_ext_texture_filter_anisotropic);
    visitor.visit(m_ext_texture_norm16);
    visitor.visit(m_oes_element_index_uint_object_extension);
    visitor.visit(m_oes_standard_derivatives_object_extension);
    visitor.visit(m_oes_vertex_array_object_extension);
    visitor.visit(m_webgl_compressed_texture_s3tc_extension);
    visitor.visit(m_webgl_compressed_texture_s3tc_srgb_extension);
    visitor.visit(m_webgl_draw_buffers_extension);
}

bool WebGLRenderingContextBase::ext_texture_filter_anisotropic_extension_enabled() const
{
    return !!m_ext_texture_filter_anisotropic;
}

bool WebGLRenderingContextBase::angle_instanced_arrays_extension_enabled() const
{
    return !!m_angle_instanced_arrays_extension;
}

bool WebGLRenderingContextBase::oes_standard_derivatives_extension_enabled() const
{
    return !!m_oes_standard_derivatives_object_extension;
}

bool WebGLRenderingContextBase::webgl_draw_buffers_extension_enabled() const
{
    return !!m_webgl_draw_buffers_extension;
}

ReadonlySpan<WebIDL::UnsignedLong> WebGLRenderingContextBase::enabled_compressed_texture_formats() const
{
    return m_enabled_compressed_texture_formats;
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
            return source->bitmap();
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
