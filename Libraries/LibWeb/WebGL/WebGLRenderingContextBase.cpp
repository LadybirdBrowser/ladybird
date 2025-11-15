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
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
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
