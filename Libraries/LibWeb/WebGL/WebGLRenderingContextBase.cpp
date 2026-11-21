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
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

namespace {

// `ContextCreationArguments` of Bindings/Rust/src/web_gl/exports.rs.
struct ContextCreationArguments {
    void* transport { nullptr };
    bool is_webgl2 { false };
    void* realm { nullptr };
    void* canvas { nullptr };
    void* canvas_host { nullptr };
    bool canvas_is_offscreen { false };
    ContextAttributes context_attributes;
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

}

extern "C" {

// Defined in Bindings/Rust/src/web_gl/exports.rs. A context is the cell of a context of either kind.
void* web_webgl_context_create(JSVM*, ContextCreationArguments const*);
void* web_webgl_context_canvas_host(void* context);
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
void web_webgl_context_actual_context_parameters(void* context, ContextAttributes* out_context_attributes);
void web_webgl_context_restore(void* context, void* transport, ReadonlySpan<ReadonlyBytes> supported_extensions);
void* web_webgl_context_event_create(JSVM*, void* realm, FlatPtr type);
bool web_webgl_context_attributes_from_value(JSVM*, JSValue, ContextAttributes* out_context_attributes, JSValue* out_exception);

// What the Rust implementation asks of LibWeb, declared in Bindings/Rust/src/web_gl/canvas.rs.
void web_webgl_canvas_did_change_content(void* canvas_host);
void web_webgl_canvas_bitmap_size(void* canvas_host, i32* out_width, i32* out_height);
void* web_webgl_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm);
void* web_webgl_canvas_relevant_realm(void* canvas_host);
bool web_webgl_expose_experimental_interfaces();
TexImageSourceResult web_webgl_read_tex_image_source(void* source_wrapper, TexImageSourceKind, u32 format, u32 type, void** out_frame);
TexImageSourceResult web_webgl_read_tex_image_frame(Gfx::DecodedImageFrame const* image_frame, u32 format, u32 type, void** out_frame);
TexImageSourceResult web_webgl_read_tex_image_pixels(u8 const* pixels, u32 width, u32 height, u32 format, u32 type, void** out_frame);
}

static void* cell_of(WebGLRenderingContextBase const& context)
{
    return const_cast<WebGLRenderingContextBase*>(&context);
}

JS::ThrowCompletionOr<ContextAttributes> convert_to_context_attributes(JS::VM& vm, JS::Value value)
{
    ContextAttributes context_attributes;
    JSValue exception {};
    if (web_webgl_context_attributes_from_value(JS::HostABI::vm_to_abi(vm), JS::HostABI::value_to_abi(value), &context_attributes, &exception))
        return JS::throw_completion(JS::HostABI::value_from_abi(exception));
    return context_attributes;
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
    auto& event = *static_cast<DOM::Event*>(web_webgl_context_event_create(JS::HostABI::vm_to_abi(JS::VM::the()), web_webgl_canvas_relevant_realm(&canvas), type.to_utf16_string().into_raw()));
    return canvas.canvas_event_target().dispatch_event(event);
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

WebGLRenderingContextBase* WebGLRenderingContextBase::create(JS::Realm& realm, CanvasOwner canvas, ContextAttributes const& context_attributes, WebGLVersion webgl_version)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element or OffscreenCanvas.
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
        .context_attributes = context_attributes,
        .supported_extensions = supported_extensions.span(),
    };
    return static_cast<WebGLRenderingContextBase*>(web_webgl_context_create(JS::HostABI::vm_to_abi(realm.vm()), &arguments));
}

GC::Ptr<WebGLRenderingContext> WebGLRenderingContext::create(JS::Realm& realm, CanvasOwner canvas, ContextAttributes const& context_attributes)
{
    return static_cast<WebGLRenderingContext*>(WebGLRenderingContextBase::create(realm, canvas, context_attributes, WebGLVersion::WebGL1));
}

GC::Ptr<WebGL2RenderingContext> WebGL2RenderingContext::create(JS::Realm& realm, CanvasOwner canvas, ContextAttributes const& context_attributes)
{
    return static_cast<WebGL2RenderingContext*>(WebGLRenderingContextBase::create(realm, canvas, context_attributes, WebGLVersion::WebGL2));
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

Optional<Gfx::DecodedImageFrame> WebGLRenderingContextBase::read_back_drawing_buffer(Gfx::IntRect const& rect)
{
    auto frame = adopt_own_if_nonnull(static_cast<Gfx::DecodedImageFrame*>(web_webgl_context_read_back_drawing_buffer(this, rect.x(), rect.y(), rect.width(), rect.height())));
    if (!frame)
        return {};
    return move(*frame);
}

void WebGLRenderingContextBase::lose_context_from_compositor_loss()
{
    if (!web_webgl_context_mark_lost(this))
        return;

    HTML::queue_a_task(HTML::Task::Source::WebGL, nullptr, nullptr, GC::create_function(GC::Heap::the(), GC::captures(*this), [](WebGLRenderingContextBase& self) {
        // webglcontextlost is cancelable; preventDefault() means the page wants the context
        // restored once a compositor is available again.
        web_webgl_context_set_restore_requested(&self, !fire_webgl_context_event(self.canvas_host(), EventNames::webglcontextlost));
    }));
}

void WebGLRenderingContextBase::restore_context_after_compositor_reconnect()
{
    if (!web_webgl_context_wants_restoring(this))
        return;

    // A fresh host context starts with no GL objects; per the spec the page re-creates them
    // in its webglcontextrestored handler.
    ContextAttributes actual_context_parameters;
    web_webgl_context_actual_context_parameters(this, &actual_context_parameters);
    auto webgl_version = web_webgl_context_is_webgl2(this) ? WebGLVersion::WebGL2 : WebGLVersion::WebGL1;
    auto remote = create_remote_webgl_context(canvas_host(), webgl_version, actual_context_parameters.depth, actual_context_parameters.stencil, actual_context_parameters.antialias);
    if (!remote.has_value())
        return;

    auto supported_extensions = remote->supported_extension_spans();
    web_webgl_context_restore(this, &remote->transport.leak_ref(), supported_extensions.span());

    HTML::queue_a_task(HTML::Task::Source::WebGL, nullptr, nullptr, GC::create_function(GC::Heap::the(), GC::captures(*this), [](WebGLRenderingContextBase& self) {
        fire_webgl_context_event(self.canvas_host(), EventNames::webglcontextrestored);
    }));
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
        if (auto canvas_frame = Bindings::impl_from<HTML::HTMLCanvasElement>(wrapper).get_srgb_frame_from_surface(); canvas_frame.has_value())
            frame = canvas_frame.release_value();
        break;
    case TexImageSourceKind::OffscreenCanvas:
        if (auto canvas_frame = Bindings::impl_from<HTML::OffscreenCanvas>(wrapper).get_srgb_frame_from_surface(); canvas_frame.has_value())
            frame = canvas_frame.release_value();
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

    // NB: The bitmap is shareable, so that the frame shares its memory instead of copying the pixels again.
    auto bitmap = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Unpremultiplied, { static_cast<int>(width), static_cast<int>(height) });
    if (bitmap.is_error())
        return TexImageSourceResult::InvalidValue;
    auto row_size = static_cast<size_t>(width) * sizeof(u32);
    for (u32 y = 0; y < height; ++y)
        memcpy(bitmap.value()->scanline_u8(static_cast<int>(y)), pixels + y * row_size, row_size);
    auto pixels_frame = Gfx::DecodedImageFrame::create(*bitmap.value());
    if (pixels_frame.is_error())
        return TexImageSourceResult::InvalidValue;
    return tex_image_source_frame(Compositing::WebGL::ImageSourceFrame { pixels_frame.release_value() }, format, type, out_frame);
}

}
