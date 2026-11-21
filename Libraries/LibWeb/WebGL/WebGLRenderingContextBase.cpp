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
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace Web::WebGL {

namespace {

// `ContextCreationArguments` of Bindings/Rust/src/web_gl/exports.rs.
struct ContextCreationArguments {
    bool is_webgl2 { false };
    void* realm { nullptr };
    void* canvas { nullptr };
    void* canvas_host { nullptr };
    bool canvas_is_offscreen { false };
    ContextAttributes context_attributes;
};

// `TexImageSourceKind` of Bindings/Rust/src/web_gl/canvas.rs: the sources whose frame LibWeb reads.
enum class TexImageSourceKind : u8 {
    HTMLImageElement,
    HTMLCanvasElement,
    OffscreenCanvas,
};

}

extern "C" {

// Defined in Bindings/Rust/src/web_gl/exports.rs. A context is the cell of a context of either kind.
void* web_webgl_context_create(JSVM*, ContextCreationArguments const*);
void* web_webgl_context_canvas_host(void* context);
bool web_webgl_context_is_lost(void* context);
bool web_webgl_context_canvas_id(void* context, u64* out_canvas_id);
void web_webgl_context_set_size(void* context, i32 width, i32 height);
void web_webgl_context_present_for_compositing(void* context);
void web_webgl_context_clear_drawing_buffer(void* context);
void* web_webgl_context_read_back_drawing_buffer(void* context, i32 x, i32 y, i32 width, i32 height);
bool web_webgl_context_mark_lost(void* context);
void web_webgl_context_set_restore_requested(void* context, bool requested);
bool web_webgl_context_wants_restoring(void* context);
bool web_webgl_context_restore(void* context);
void* web_webgl_context_event_create(JSVM*, void* realm, FlatPtr type);
bool web_webgl_context_attributes_from_value(JSVM*, JSValue, ContextAttributes* out_context_attributes, JSValue* out_exception);

// What the Rust implementation asks of LibWeb, declared in Bindings/Rust/src/web_gl/canvas.rs.
void web_webgl_canvas_did_change_content(void* canvas_host);
void web_webgl_canvas_bitmap_size(void* canvas_host, i32* out_width, i32* out_height);
void* web_webgl_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm);
void* web_webgl_canvas_relevant_realm(void* canvas_host);
bool web_webgl_expose_experimental_interfaces();
Gfx::DecodedImageFrame* web_webgl_texture_image_frame(void* source_wrapper, TexImageSourceKind);
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

WebGLRenderingContextBase* WebGLRenderingContextBase::create(JS::Realm& realm, CanvasOwner canvas, ContextAttributes const& context_attributes, WebGLVersion webgl_version)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element or OffscreenCanvas.
    auto& canvas_host = canvas_host_for(canvas);
    ContextCreationArguments arguments {
        .is_webgl2 = webgl_version == WebGLVersion::WebGL2,
        .realm = &realm,
        .canvas = canvas.visit([](auto const& canvas) -> void* { return static_cast<GC::Cell*>(canvas.ptr()); }),
        .canvas_host = &canvas_host,
        .canvas_is_offscreen = canvas.has<GC::Ref<HTML::OffscreenCanvas>>(),
        .context_attributes = context_attributes,
    };
    auto* context = static_cast<WebGLRenderingContextBase*>(web_webgl_context_create(JS::HostABI::vm_to_abi(realm.vm()), &arguments));
    if (!context) {
        fire_webgl_context_creation_error(canvas_host);
        return nullptr;
    }

    // NB: The display list must be re-recorded so its DrawCanvas command refers to the new remote context's
    //     canvas id. Content updates alone don't invalidate the display list, so do it here.
    canvas_host.did_create_canvas_backing_storage();
    return context;
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
    if (!web_webgl_context_restore(this))
        return;
    canvas_host().did_create_canvas_backing_storage();

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

// The frame that a texture upload from an image or a canvas takes, allocated with new, or null.
Gfx::DecodedImageFrame* web_webgl_texture_image_frame(void* source_wrapper, TexImageSourceKind source_kind)
{
    // FIXME: If source is null then an INVALID_VALUE error is generated.
    auto& wrapper = *static_cast<JS::Object*>(source_wrapper);
    Optional<Gfx::DecodedImageFrame> frame;
    switch (source_kind) {
    case TexImageSourceKind::HTMLImageElement:
        // NB: The frame of an animated image that the Compositor moves through has another id than this one.
        frame = Bindings::impl_from<HTML::HTMLImageElement>(wrapper).default_image_frame();
        break;
    case TexImageSourceKind::HTMLCanvasElement:
        frame = Bindings::impl_from<HTML::HTMLCanvasElement>(wrapper).get_srgb_frame_from_surface();
        break;
    case TexImageSourceKind::OffscreenCanvas:
        frame = Bindings::impl_from<HTML::OffscreenCanvas>(wrapper).get_srgb_frame_from_surface();
        break;
    }
    // FIXME: Respect unpackColorSpace
    if (!frame.has_value())
        return nullptr;
    return new Gfx::DecodedImageFrame { frame.release_value() };
}

}
