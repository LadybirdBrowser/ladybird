/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16FlyString.h>
#include <AK/Vector.h>
#include <LibCompositing/WebGL/TextureUpload.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/WebGLContextEvent.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLContextAttributes.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::WebGL {

namespace {

// `ContextAttributes` of Bindings/Rust/src/web_gl/exports.rs.
struct RustContextAttributes {
    bool alpha { true };
    bool depth { true };
    bool stencil { false };
    bool antialias { true };
    bool premultiplied_alpha { true };
    bool preserve_drawing_buffer { false };
    // 0 is "default", 1 is "low-power" and 2 is "high-performance".
    u8 power_preference { 0 };
    bool fail_if_major_performance_caveat { false };
    bool desynchronized { false };
    bool xr_compatible { false };
};

// `ContextCreationArguments` of Bindings/Rust/src/web_gl/exports.rs.
struct ContextCreationArguments {
    void* transport { nullptr };
    bool is_webgl2 { false };
    void* realm { nullptr };
    void* canvas { nullptr };
    void* canvas_host { nullptr };
    bool canvas_is_offscreen { false };
    RustContextAttributes context_attributes;
    ReadonlySpan<ReadonlyBytes> supported_extensions;
};

// `TexImageSourceKind` and `TexImageSourceResult` of Bindings/Rust/src/web_gl/canvas.rs. The kinds are the sources
// that are implemented in C++.
enum class TexImageSourceKind : u8 {
    HTMLImageElement,
    HTMLCanvasElement,
    HTMLVideoElement,
    OffscreenCanvas,
};

enum class TexImageSourceResult : u8 {
    Frame,
    NoFrame,
    InvalidValue,
};

// NB: `PixelUnpackState` of Bindings/Rust/src/web_gl/canvas.rs is four `usize`s in this order, with these defaults.
static_assert(sizeof(PixelUnpackState) == 4 * sizeof(size_t));
static_assert(offsetof(PixelUnpackState, alignment) == 0);
static_assert(offsetof(PixelUnpackState, row_length) == sizeof(size_t));
static_assert(offsetof(PixelUnpackState, skip_pixels) == 2 * sizeof(size_t));
static_assert(offsetof(PixelUnpackState, skip_rows) == 3 * sizeof(size_t));
static_assert(PixelUnpackState {}.alignment == 4);
static_assert(PixelUnpackState {}.row_length == 0);
static_assert(PixelUnpackState {}.skip_pixels == 0);
static_assert(PixelUnpackState {}.skip_rows == 0);

}

extern "C" {

// Defined in Bindings/Rust/src/web_gl/exports.rs. A context is the cell of a context of either kind.
void* web_webgl_context_create(JSVM*, ContextCreationArguments const*);
void* web_webgl_context_canvas_host(void* context);
void* web_webgl_context_realm(void* context);
bool web_webgl_context_is_webgl2(void* context);
bool web_webgl_context_is_lost(void* context);
bool web_webgl_context_canvas_id(void* context, u64* out_canvas_id);
void web_webgl_context_set_size(void* context, i32 width, i32 height);
void web_webgl_context_present_for_compositing(void* context);
void web_webgl_context_clear_drawing_buffer(void* context);
void* web_webgl_context_read_back_drawing_buffer(void* context, i32 x, i32 y, i32 width, i32 height);
bool web_webgl_context_mark_lost(void* context);
void web_webgl_context_set_restore_requested(void* context, bool requested);
bool web_webgl_context_wants_restoring(void* context);
void web_webgl_context_actual_context_parameters(void* context, RustContextAttributes* out_context_attributes);
void web_webgl_context_restore(void* context, void* transport, ReadonlySpan<ReadonlyBytes> supported_extensions);
bool web_webgl_context_xr_compatible(void* context);
void web_webgl_context_set_xr_compatible(void* context, bool xr_compatible);

// What the Rust implementation asks of LibWeb, declared in Bindings/Rust/src/web_gl/canvas.rs.
void web_webgl_canvas_did_change_content(void* canvas_host);
void web_webgl_canvas_bitmap_size(void* canvas_host, i32* out_width, i32* out_height);
void* web_webgl_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm);
void* web_webgl_canvas_relevant_realm(void* canvas_host);
bool web_webgl_expose_experimental_interfaces();
bool web_webgl_is_valid_2d_pixel_unpack_state(i32 width, PixelUnpackState const* unpack_state);
bool web_webgl_required_2d_texture_data_size(i32 width, i32 height, u32 format, u32 type, PixelUnpackState const* unpack_state, size_t* out_size);
TexImageSourceResult web_webgl_read_tex_image_source(void* source_wrapper, TexImageSourceKind, u32 format, u32 type, void** out_frame);
TexImageSourceResult web_webgl_read_tex_image_frame(Gfx::DecodedImageFrame const* image_frame, u32 format, u32 type, void** out_frame);
TexImageSourceResult web_webgl_read_tex_image_pixels(u8 const* pixels, u32 width, u32 height, u32 format, u32 type, void** out_frame);
void* web_webgl_make_xr_compatible(void* context);
}

static void* cell_of(WebGLRenderingContextBase const& context)
{
    return const_cast<WebGLRenderingContextBase*>(&context);
}

static RustContextAttributes to_rust_context_attributes(WebGLContextAttributes const& context_attributes)
{
    u8 power_preference = 0;
    switch (context_attributes.power_preference) {
    case Bindings::WebGLPowerPreference::Default:
        power_preference = 0;
        break;
    case Bindings::WebGLPowerPreference::LowPower:
        power_preference = 1;
        break;
    case Bindings::WebGLPowerPreference::HighPerformance:
        power_preference = 2;
        break;
    }
    return {
        .alpha = context_attributes.alpha,
        .depth = context_attributes.depth,
        .stencil = context_attributes.stencil,
        .antialias = context_attributes.antialias,
        .premultiplied_alpha = context_attributes.premultiplied_alpha,
        .preserve_drawing_buffer = context_attributes.preserve_drawing_buffer,
        .power_preference = power_preference,
        .fail_if_major_performance_caveat = context_attributes.fail_if_major_performance_caveat,
        .desynchronized = context_attributes.desynchronized,
        .xr_compatible = context_attributes.xr_compatible,
    };
}

HTML::CanvasHost& canvas_host_for(CanvasOwner const& canvas)
{
    return canvas.visit([](auto const& canvas) -> HTML::CanvasHost& { return *canvas; });
}

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-event
// Returns false if the event was canceled (the page called preventDefault), which is how
// webglcontextlost signals that the page wants the context restored.
static bool fire_webgl_context_event(HTML::CanvasHost& canvas, Utf16FlyString const& type)
{
    // To fire a WebGL context event named e means that an event using the WebGLContextEvent interface, with its type attribute [DOM4] initialized to e, its cancelable attribute initialized to true, and its isTrusted attribute [DOM4] initialized to true, is to be dispatched at the given object.
    // FIXME: Consider setting a status message.
    auto event = WebGLContextEvent::create(type, Bindings::WebGLContextEventInit {}, HighResolutionTime::current_high_resolution_time(canvas.canvas_relevant_global_object()));
    event->set_is_trusted(true);
    event->set_cancelable(true);
    return canvas.canvas_event_target().dispatch_event(*event);
}

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-creation-error
static void fire_webgl_context_creation_error(HTML::CanvasHost& canvas)
{
    // 1. Fire a WebGL context event named "webglcontextcreationerror" at canvas, optionally with its statusMessage attribute set to a platform dependent string about the nature of the failure.
    fire_webgl_context_event(canvas, EventNames::webglcontextcreationerror);
}

// The drawing buffer's creation-time size, clamped like set_size() clamps resizes;
// later resizes travel as SetDrawingBufferSize commands.
static Gfx::IntSize initial_drawing_buffer_size(HTML::CanvasHost& canvas)
{
    auto size = canvas.bitmap_size_for_canvas();
    return {
        clamp(size.width(), 1, max_webgl_drawing_buffer_dimension),
        clamp(size.height(), 1, max_webgl_drawing_buffer_dimension),
    };
}

namespace {

struct RemoteWebGLContext {
    NonnullRefPtr<RemoteWebGLTransport> transport;
    Vector<String> supported_extensions;

    Vector<ReadonlyBytes> supported_extension_spans() const
    {
        Vector<ReadonlyBytes> spans;
        spans.ensure_capacity(supported_extensions.size());
        for (auto const& extension : supported_extensions)
            spans.unchecked_append(extension.bytes());
        return spans;
    }
};

Optional<RemoteWebGLContext> create_remote_webgl_context(HTML::CanvasHost& canvas, WebGLVersion webgl_version, bool depth, bool stencil, bool antialias)
{
    auto& page = canvas.canvas_page();
    if (!page.has_compositor_host())
        return {};
    auto transport = page.compositor_host().create_webgl_transport();
    if (!transport)
        return {};

    auto result = transport->create_context(webgl_version, initial_drawing_buffer_size(canvas), depth, stencil, antialias);
    if (!result.success)
        return {};

    // NB: The display list must be re-recorded so its DrawCanvas command refers to the new remote context's
    //     canvas id. Content updates alone don't invalidate the display list, so do it here.
    canvas.did_create_canvas_backing_storage();

    return RemoteWebGLContext { transport.release_nonnull(), move(result.supported_extensions) };
}

}

JS::ThrowCompletionOr<WebGLRenderingContextBase*> WebGLRenderingContextBase::create(JS::Realm& realm, CanvasOwner canvas, JS::Value options, WebGLVersion webgl_version)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element or OffscreenCanvas.
    auto context_attributes = TRY(convert_value_to_context_attributes_dictionary(realm.vm(), options));

    auto& canvas_host = canvas_host_for(canvas);
    auto remote = create_remote_webgl_context(canvas_host, webgl_version, context_attributes.depth, context_attributes.stencil, context_attributes.antialias);
    if (!remote.has_value()) {
        fire_webgl_context_creation_error(canvas_host);
        return nullptr;
    }

    auto supported_extensions = remote->supported_extension_spans();
    ContextCreationArguments arguments {
        .transport = &remote->transport.leak_ref(),
        .is_webgl2 = webgl_version == WebGLVersion::WebGL2,
        .realm = &realm,
        .canvas = canvas.visit([](auto const& canvas) -> void* { return static_cast<GC::Cell*>(canvas.ptr()); }),
        .canvas_host = &canvas_host,
        .canvas_is_offscreen = canvas.has<GC::Ref<HTML::OffscreenCanvas>>(),
        .context_attributes = to_rust_context_attributes(context_attributes),
        .supported_extensions = supported_extensions.span(),
    };
    return static_cast<WebGLRenderingContextBase*>(web_webgl_context_create(JS::HostABI::vm_to_abi(realm.vm()), &arguments));
}

JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> WebGLRenderingContext::create(JS::Realm& realm, CanvasOwner canvas, JS::Value options)
{
    return static_cast<WebGLRenderingContext*>(TRY(WebGLRenderingContextBase::create(realm, canvas, options, WebGLVersion::WebGL1)));
}

JS::ThrowCompletionOr<GC::Ptr<WebGL2RenderingContext>> WebGL2RenderingContext::create(JS::Realm& realm, CanvasOwner canvas, JS::Value options)
{
    return static_cast<WebGL2RenderingContext*>(TRY(WebGLRenderingContextBase::create(realm, canvas, options, WebGLVersion::WebGL2)));
}

HTML::CanvasHost& WebGLRenderingContextBase::canvas_host() const
{
    return *static_cast<HTML::CanvasHost*>(web_webgl_context_canvas_host(cell_of(*this)));
}

bool WebGLRenderingContextBase::is_context_lost() const
{
    return web_webgl_context_is_lost(cell_of(*this));
}

Optional<Compositing::CanvasId> WebGLRenderingContextBase::canvas_id() const
{
    u64 canvas_id = 0;
    if (!web_webgl_context_canvas_id(cell_of(*this), &canvas_id))
        return {};
    return Compositing::CanvasId { canvas_id };
}

void WebGLRenderingContextBase::set_size(Gfx::IntSize const& size)
{
    web_webgl_context_set_size(this, size.width(), size.height());
}

void WebGLRenderingContextBase::prepare_for_compositing()
{
    web_webgl_context_present_for_compositing(this);
}

void WebGLRenderingContextBase::clear_drawing_buffer()
{
    web_webgl_context_clear_drawing_buffer(this);
}

RefPtr<Gfx::Bitmap> WebGLRenderingContextBase::read_back_drawing_buffer(Gfx::IntRect const& rect)
{
    auto* bitmap = static_cast<Gfx::Bitmap*>(web_webgl_context_read_back_drawing_buffer(this, rect.x(), rect.y(), rect.width(), rect.height()));
    if (!bitmap)
        return nullptr;
    return adopt_ref(*bitmap);
}

void WebGLRenderingContextBase::lose_context_from_compositor_loss()
{
    if (!web_webgl_context_mark_lost(this))
        return;

    HTML::queue_a_task(HTML::Task::Source::WebGL, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this] {
        // webglcontextlost is cancelable; preventDefault() means the page wants the context
        // restored once a compositor is available again.
        web_webgl_context_set_restore_requested(this, !fire_webgl_context_event(canvas_host(), EventNames::webglcontextlost));
    }));
}

void WebGLRenderingContextBase::restore_context_after_compositor_reconnect()
{
    if (!web_webgl_context_wants_restoring(this))
        return;

    // A fresh host context starts with no GL objects; per the spec the page re-creates them
    // in its webglcontextrestored handler.
    RustContextAttributes actual_context_parameters;
    web_webgl_context_actual_context_parameters(this, &actual_context_parameters);
    auto webgl_version = web_webgl_context_is_webgl2(this) ? WebGLVersion::WebGL2 : WebGLVersion::WebGL1;
    auto remote = create_remote_webgl_context(canvas_host(), webgl_version, actual_context_parameters.depth, actual_context_parameters.stencil, actual_context_parameters.antialias);
    if (!remote.has_value())
        return;

    auto supported_extensions = remote->supported_extension_spans();
    web_webgl_context_restore(this, &remote->transport.leak_ref(), supported_extensions.span());

    HTML::queue_a_task(HTML::Task::Source::WebGL, nullptr, nullptr, GC::create_function(GC::Heap::the(), [this] {
        fire_webgl_context_event(canvas_host(), EventNames::webglcontextrestored);
    }));
}

bool WebGLRenderingContextBase::xr_compatible() const
{
    return web_webgl_context_xr_compatible(cell_of(*this));
}

void WebGLRenderingContextBase::set_xr_compatible(bool xr_compatible)
{
    web_webgl_context_set_xr_compatible(this, xr_compatible);
}

// https://immersive-web.github.io/webxr/#dom-webglrenderingcontextbase-makexrcompatible
GC::Ref<WebIDL::Promise> WebGLRenderingContextBase::make_xr_compatible()
{
    // 1. If the requesting document’s origin is not allowed to use the "xr-spatial-tracking" permissions policy,
    //    resolve promise and return it.
    // FIXME: Implement this.

    // 2. Let promise be a new Promise created in the Realm of this WebGLRenderingContextBase.
    auto& realm = *static_cast<JS::Realm*>(web_webgl_context_realm(this));
    auto promise = WebIDL::create_promise_for(canvas_host().canvas_relevant_global_object());

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
                WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("The WebGL context has been lost."_utf16));
            }));
        }
        // -> If device is null:
        else if (false) {
            // Queue a task to set context’s XR compatible boolean to false and reject promise with an InvalidStateError.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(false);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Could not select an immersive XR device."_utf16));
            }));
        }
        // -> If context’s XR compatible boolean is true:
        else if (context->xr_compatible()) {
            // Queue a task to resolve promise.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise]() {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(promise);
            }));
        }
        // -> If context was created on a compatible graphics adapter for device:
        // FIXME: For now we just pretend that this happened, so that we can resolve the promise and proceed running basic WPT tests for this.
        else if (true) {
            // Queue a task to set context’s XR compatible boolean to true and resolve promise.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(true);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(promise);
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

void web_webgl_canvas_did_change_content(void* canvas_host)
{
    static_cast<HTML::CanvasHost*>(canvas_host)->did_change_canvas_content();
}

void web_webgl_canvas_bitmap_size(void* canvas_host, i32* out_width, i32* out_height)
{
    auto size = static_cast<HTML::CanvasHost*>(canvas_host)->bitmap_size_for_canvas();
    *out_width = size.width();
    *out_height = size.height();
}

void* web_webgl_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm_pointer)
{
    auto& realm = *static_cast<JS::Realm*>(realm_pointer);
    auto& wrapper_world = Bindings::host_defined_wrapper_world(realm);
    auto* cell = static_cast<GC::Cell*>(canvas);
    if (canvas_is_offscreen)
        return Bindings::wrap(wrapper_world, realm, GC::Ref { *static_cast<HTML::OffscreenCanvas*>(cell) }).ptr();
    return Bindings::wrap(wrapper_world, realm, GC::Ref { *static_cast<HTML::HTMLCanvasElement*>(cell) }).ptr();
}

void* web_webgl_canvas_relevant_realm(void* canvas_host)
{
    return &static_cast<HTML::CanvasHost*>(canvas_host)->canvas_relevant_global_object().shape().realm();
}

bool web_webgl_expose_experimental_interfaces()
{
    return HTML::WindowOrWorkerGlobalScopeMixin::expose_experimental_interfaces();
}

bool web_webgl_is_valid_2d_pixel_unpack_state(i32 width, PixelUnpackState const* unpack_state)
{
    return is_valid_2d_pixel_unpack_state(width, *unpack_state);
}

bool web_webgl_required_2d_texture_data_size(i32 width, i32 height, u32 format, u32 type, PixelUnpackState const* unpack_state, size_t* out_size)
{
    auto size = required_2d_texture_data_size(width, height, format, type, *unpack_state);
    if (!size.has_value())
        return false;
    *out_size = *size;
    return true;
}

// The frame that a texture image source uploads, which the recorder takes over, if this format and type upload one.
static TexImageSourceResult tex_image_source_frame(Optional<Compositing::WebGL::ImageSourceFrame> frame, u32 format, u32 type, void** out_frame)
{
    if (!frame.has_value())
        return TexImageSourceResult::NoFrame;

    // Validate the combination before recording; the pixels travel to the host as shared
    // memory and the host performs the conversion next to GL.
    if (!texture_export_format(format, type).has_value())
        return TexImageSourceResult::NoFrame;

    // FIXME: Respect unpackColorSpace
    *out_frame = new ImageSourceFrameOwnedByRecorder { frame.release_value() };
    return TexImageSourceResult::Frame;
}

TexImageSourceResult web_webgl_read_tex_image_source(void* source_wrapper, TexImageSourceKind source_kind, u32 format, u32 type, void** out_frame)
{
    // FIXME: If source is null then an INVALID_VALUE error is generated.
    auto& wrapper = *static_cast<JS::Object*>(source_wrapper);
    Optional<Compositing::WebGL::ImageSourceFrame> frame;
    switch (source_kind) {
    case TexImageSourceKind::HTMLImageElement:
        if (auto image_frame = Bindings::impl_from<HTML::HTMLImageElement>(wrapper).current_image_frame(); image_frame.has_value())
            frame = image_frame.release_value();
        break;
    case TexImageSourceKind::HTMLCanvasElement:
        if (auto bitmap = Bindings::impl_from<HTML::HTMLCanvasElement>(wrapper).get_bitmap_from_surface())
            frame = Gfx::DecodedImageFrame { *bitmap };
        break;
    case TexImageSourceKind::OffscreenCanvas:
        if (auto bitmap = Bindings::impl_from<HTML::OffscreenCanvas>(wrapper).get_bitmap_from_surface())
            frame = Gfx::DecodedImageFrame { *bitmap };
        break;
    case TexImageSourceKind::HTMLVideoElement:
        if (auto captured_frame = Bindings::impl_from<HTML::HTMLVideoElement>(wrapper).capture_current_video_frame(); captured_frame.has_value())
            frame = captured_frame.release_value();
        break;
    }
    return tex_image_source_frame(move(frame), format, type, out_frame);
}

// The bitmap data of an ImageBitmap.
TexImageSourceResult web_webgl_read_tex_image_frame(Gfx::DecodedImageFrame const* image_frame, u32 format, u32 type, void** out_frame)
{
    return tex_image_source_frame(Compositing::WebGL::ImageSourceFrame { *image_frame }, format, type, out_frame);
}

// The pixels of an ImageData, which are copied: script can change them, and detach or free their buffer, before the
// recorder sends the frame.
TexImageSourceResult web_webgl_read_tex_image_pixels(u8 const* pixels, u32 width, u32 height, u32 format, u32 type, void** out_frame)
{
    // NB: Check the format first, so that pixels that are not uploaded are not copied.
    if (!texture_export_format(format, type).has_value())
        return TexImageSourceResult::NoFrame;

    // NB: The bitmap is shareable, so that the recorder sends it to the host without copying the pixels again.
    auto bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, { static_cast<int>(width), static_cast<int>(height) });
    if (bitmap.is_error())
        return TexImageSourceResult::InvalidValue;
    auto row_size = static_cast<size_t>(width) * sizeof(u32);
    for (u32 y = 0; y < height; ++y)
        memcpy(bitmap.value()->scanline_u8(static_cast<int>(y)), pixels + y * row_size, row_size);
    NonnullRefPtr<Gfx::Bitmap const> pixels_bitmap = bitmap.release_value();
    return tex_image_source_frame(Compositing::WebGL::ImageSourceFrame { Gfx::DecodedImageFrame { move(pixels_bitmap) } }, format, type, out_frame);
}

void* web_webgl_make_xr_compatible(void* context)
{
    return static_cast<WebGLRenderingContextBase*>(context)->make_xr_compatible().ptr();
}

}
