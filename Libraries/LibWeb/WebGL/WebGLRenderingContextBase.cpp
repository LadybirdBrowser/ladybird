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

#include <LibGfx/BitmapExport.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaUtils.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
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
#include <LibWeb/WebGL/Extensions/WebGLDebugRendererInfo.h>
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

struct Extension {
    Vector<StringView> required_angle_extensions;
    JS::ThrowCompletionOr<GC::Ref<JS::Object>> (*factory)(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);
    Optional<OpenGLContext::WebGLVersion> only_for_webgl_version { OptionalNone {} };
};

static HashMap<String, Extension, AK::ASCIICaseInsensitiveStringTraits> s_available_webgl_extensions {
    // Khronos ratified WebGL Extensions
    { "ANGLE_instanced_arrays"_string, { { "GL_ANGLE_instanced_arrays"sv }, ANGLEInstancedArrays::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_blend_minmax"_string, { { "GL_EXT_blend_minmax"sv }, EXTBlendMinMax::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_frag_depth"_string, { { "GL_EXT_frag_depth"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_shader_texture_lod"_string, { { "GL_EXT_shader_texture_lod"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_texture_filter_anisotropic"_string, { { "GL_EXT_texture_filter_anisotropic"sv }, EXTTextureFilterAnisotropic::create } },
    { "OES_element_index_uint"_string, { { "GL_OES_element_index_uint"sv }, OESElementIndexUint::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_standard_derivatives"_string, { { "GL_OES_standard_derivatives"sv }, OESStandardDerivatives::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_texture_float"_string, { { "GL_OES_texture_float"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_texture_float_linear"_string, { { "GL_OES_texture_float_linear"sv }, nullptr } },
    { "OES_texture_half_float"_string, { { "GL_OES_texture_half_float"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_texture_half_float_linear"_string, { { "GL_OES_texture_half_float_linear"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_vertex_array_object"_string, { { "GL_OES_vertex_array_object"sv }, OESVertexArrayObject::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_compressed_texture_s3tc"_string, { { "GL_EXT_texture_compression_dxt1"sv, "GL_ANGLE_texture_compression_dxt3"sv, "GL_ANGLE_texture_compression_dxt5"sv }, WebGLCompressedTextureS3tc::create } },
    { "WEBGL_debug_renderer_info"_string, { {}, WebGLDebugRendererInfo::create } },
    { "WEBGL_debug_shaders"_string, { {}, nullptr } },
    { "WEBGL_depth_texture"_string, { { "GL_ANGLE_depth_texture"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_draw_buffers"_string, { { "GL_EXT_draw_buffers"sv }, WebGLDrawBuffers::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_lose_context"_string, { {}, nullptr } },

    // Community approved WebGL Extensions
    { "EXT_clip_control"_string, { { "GL_EXT_clip_control"sv }, nullptr } },
    { "EXT_color_buffer_float"_string, { { "GL_EXT_color_buffer_float"sv }, EXTColorBufferFloat::create, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_color_buffer_half_float"_string, { { "GL_EXT_color_buffer_half_float"sv }, nullptr } },
    { "EXT_conservative_depth"_string, { { "GL_EXT_conservative_depth"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_depth_clamp"_string, { { "GL_EXT_depth_clamp"sv }, nullptr } },
    { "EXT_disjoint_timer_query"_string, { { "GL_EXT_disjoint_timer_query"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_disjoint_timer_query_webgl2"_string, { { "GL_EXT_disjoint_timer_query"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_float_blend"_string, { { "GL_EXT_float_blend"sv }, nullptr } },
    { "EXT_polygon_offset_clamp"_string, { { "GL_EXT_polygon_offset_clamp"sv }, nullptr } },
    { "EXT_render_snorm"_string, { { "GL_EXT_render_snorm"sv }, EXTRenderSnorm::create, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_sRGB"_string, { { "GL_EXT_sRGB"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_texture_compression_bptc"_string, { { "GL_EXT_texture_compression_bptc"sv }, nullptr } },
    { "EXT_texture_compression_rgtc"_string, { { "GL_EXT_texture_compression_rgtc"sv }, nullptr } },
    { "EXT_texture_mirror_clamp_to_edge"_string, { { "GL_EXT_texture_mirror_clamp_to_edge"sv }, nullptr } },
    { "EXT_texture_norm16"_string, { { "GL_EXT_texture_norm16"sv }, EXTTextureNorm16::create, OpenGLContext::WebGLVersion::WebGL2 } },
    { "KHR_parallel_shader_compile"_string, { { "GL_KHR_parallel_shader_compile"sv }, nullptr } },
    { "NV_shader_noperspective_interpolation"_string, { { "GL_NV_shader_noperspective_interpolation"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "OES_draw_buffers_indexed"_string, { { "GL_OES_draw_buffers_indexed"sv }, nullptr } },
    { "OES_fbo_render_mipmap"_string, { { "GL_OES_fbo_render_mipmap"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_sample_variables"_string, { { "GL_OES_sample_variables"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "OES_shader_multisample_interpolation"_string, { { "GL_OES_shader_multisample_interpolation"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "OVR_multiview2"_string, { { "GL_OVR_multiview2"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_blend_func_extended"_string, { { "GL_EXT_blend_func_extended"sv }, nullptr } },
    { "WEBGL_clip_cull_distance"_string, { { "GL_EXT_clip_cull_distance"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_color_buffer_float"_string, { { "EXT_color_buffer_half_float"sv, "OES_texture_float"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_compressed_texture_astc"_string, { { "KHR_texture_compression_astc_hdr"sv, "KHR_texture_compression_astc_ldr"sv }, nullptr } },
    { "WEBGL_compressed_texture_etc"_string, { { "GL_ANGLE_compressed_texture_etc"sv }, nullptr } },
    { "WEBGL_compressed_texture_etc1"_string, { { "GL_OES_compressed_ETC1_RGB8_texture"sv }, nullptr } },
    { "WEBGL_compressed_texture_pvrtc"_string, { { "GL_IMG_texture_compression_pvrtc"sv }, nullptr } },
    { "WEBGL_compressed_texture_s3tc_srgb"_string, { { "GL_EXT_texture_compression_s3tc_srgb"sv }, WebGLCompressedTextureS3tcSrgb::create } },
    { "WEBGL_multi_draw"_string, { { "GL_ANGLE_multi_draw"sv }, nullptr } },
    { "WEBGL_polygon_mode"_string, { { "GL_ANGLE_polygon_mode"sv }, nullptr } },
    { "WEBGL_provoking_vertex"_string, { { "GL_ANGLE_provoking_vertex"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_render_shared_exponent"_string, { { "GL_QCOM_render_shared_exponent"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_stencil_texturing"_string, { { "GL_ANGLE_stencil_texturing"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
};

Optional<Vector<String>> WebGLRenderingContextBase::get_supported_extensions()
{
    auto opengl_extensions = context().get_supported_opengl_extensions();
    Vector<String> webgl_extensions;

    for (auto const& [available_extension_name, available_extension_info] : s_available_webgl_extensions) {
        bool supported = !available_extension_info.only_for_webgl_version.has_value()
            || context().webgl_version() == available_extension_info.only_for_webgl_version;

        if (!available_extension_info.factory && !HTML::UniversalGlobalScopeMixin::expose_experimental_interfaces()) {
            supported = false;
        }

        if (supported) {
            for (auto const& required_extension : available_extension_info.required_angle_extensions) {
                if (!opengl_extensions.contains_slow(required_extension)) {
                    supported = false;
                    break;
                }
            }
        }

        if (supported)
            webgl_extensions.append(available_extension_name);
    }

    return webgl_extensions;
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

    auto maybe_extension = m_enabled_extensions.get(name);
    if (maybe_extension.has_value())
        return maybe_extension.release_value();

    // If we pass the check above this will always return a value
    auto const& extension_info = s_available_webgl_extensions.get(name).release_value();

    if (!extension_info.factory)
        return nullptr;

    for (auto const& required_extension : extension_info.required_angle_extensions) {
        context().request_extension(null_terminated_string(required_extension).data());
    }

    auto extension = MUST(extension_info.factory(realm(), *this));
    m_enabled_extensions.set(name, extension);
    return extension;
}

void WebGLRenderingContextBase::enable_compressed_texture_format(WebIDL::UnsignedLong format)
{
    m_enabled_compressed_texture_formats.append(format);
}

void WebGLRenderingContextBase::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_enabled_extensions);
}

bool WebGLRenderingContextBase::extension_enabled(StringView extension) const
{
    return m_enabled_extensions.contains(MUST(String::from_utf8(extension)));
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
            return Gfx::ImmutableBitmap::create(surface->snapshot_bitmap());
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

    auto source_bitmap = bitmap->bitmap();
    if (!source_bitmap)
        return OptionalNone {};

    auto result = Gfx::export_bitmap_to_byte_buffer(
        *source_bitmap,
        bitmap->color_space(),
        export_format.value(),
        export_flags,
        destination_width,
        destination_height);
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

bool WebGLRenderingContextBase::is_context_lost() const
{
    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::is_context_lost()");
    return m_context_lost;
}

// https://immersive-web.github.io/webxr/#dom-webglrenderingcontextbase-makexrcompatible
GC::Ref<WebIDL::Promise> WebGLRenderingContextBase::make_xr_compatible()
{
    // 1. If the requesting document’s origin is not allowed to use the "xr-spatial-tracking" permissions policy,
    //    resolve promise and return it.
    // FIXME: Implement this.

    // 2. Let promise be a new Promise created in the Realm of this WebGLRenderingContextBase.
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);

    // 3. Let context be this.
    auto context = this;

    // 4. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, context, promise]() {
        // 1. Let device be the result of ensuring an immersive XR device is selected.
        // FIXME: Implement https://immersive-web.github.io/webxr/#ensure-an-immersive-xr-device-is-selected

        // 2. Set context’s XR compatible boolean as follows:

        // -> If context’s WebGL context lost flag is set:
        if (context->is_context_lost()) {
            // Queue a task to set context’s XR compatible boolean to false and reject promise with an InvalidStateError.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(false);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "The WebGL context has been lost."_utf16));
            }));
        }
        // -> If device is null:
        else if (false) {
            // Queue a task to set context’s XR compatible boolean to false and reject promise with an InvalidStateError.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(false);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Could not select an immersive XR device."_utf16));
            }));
        }
        // -> If context’s XR compatible boolean is true:
        else if (context->xr_compatible()) {
            // Queue a task to resolve promise.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise]() {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(realm, promise);
            }));
        }
        // -> If context was created on a compatible graphics adapter for device:
        // FIXME: For now we just pretend that this happened, so that we can resolve the promise and proceed running basic WPT tests for this.
        else if (true) {
            // Queue a task to set context’s XR compatible boolean to true and resolve promise.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(true);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(realm, promise);
            }));
        }
        // -> Otherwise:
        else {
            // Queue a task on the WebGL task source to perform the following steps:
            HTML::queue_a_task(HTML::Task::Source::WebGL, nullptr, nullptr, GC::create_function(realm.heap(), []() {
                // 1. Force context to be lost.

                // 2. Handle the context loss as described by the WebGL specification:
                // FIXME: Implement https://registry.khronos.org/webgl/specs/latest/1.0/#CONTEXT_LOST
            }));
        }
    }));

    // 5. Return promise.
    return promise;
}

}
