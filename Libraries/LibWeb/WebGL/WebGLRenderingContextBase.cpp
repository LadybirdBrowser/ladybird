/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/HTML/Canvas/CanvasHost.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/WindowOrWorkerGlobalScope.h>
#include <LibWeb/Loader/ResourceLoader.h>
#include <LibWeb/Page/Page.h>
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
bool web_webgl_context_is_lost(void* context);
bool web_webgl_context_canvas_id(void* context, u64* out_canvas_id);
void web_webgl_context_set_size(void* context, i32 width, i32 height);
void web_webgl_context_present_for_compositing(void* context);
void web_webgl_context_clear_drawing_buffer(void* context);
void* web_webgl_context_read_back_drawing_buffer(void* context, i32 x, i32 y, i32 width, i32 height);
bool web_webgl_context_attributes_from_value(JSVM*, JSValue, ContextAttributes* out_context_attributes, JSValue* out_exception);

// What the Rust implementation asks of LibWeb, declared in Bindings/Rust/src/web_gl/canvas.rs.
void web_webgl_canvas_did_change_content(void* canvas_host);
void web_webgl_canvas_did_create_backing_storage(void* canvas_host);
DOM::EventTarget* web_webgl_canvas_event_target(void* canvas_host);
void web_webgl_canvas_bitmap_size(void* canvas_host, i32* out_width, i32* out_height);
void* web_webgl_canvas_wrapper(void* canvas, bool canvas_is_offscreen, void* realm);
void* web_webgl_canvas_relevant_realm(void* canvas_host);
bool web_webgl_expose_experimental_interfaces();
Gfx::DecodedImageFrame* web_webgl_texture_image_frame(void* source_wrapper, TexImageSourceKind);
u8 web_navigator_compatibility_mode();
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

void web_webgl_canvas_did_change_content(void* canvas_host)
{
    static_cast<HTML::CanvasHost*>(canvas_host)->did_change_canvas_content();
}

void web_webgl_canvas_did_create_backing_storage(void* canvas_host)
{
    static_cast<HTML::CanvasHost*>(canvas_host)->did_create_canvas_backing_storage();
}

DOM::EventTarget* web_webgl_canvas_event_target(void* canvas_host)
{
    return &static_cast<HTML::CanvasHost*>(canvas_host)->canvas_event_target();
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

// The navigator compatibility mode, numbered as Web::NavigatorCompatibilityMode is: Chrome, Gecko, WebKit.
u8 web_navigator_compatibility_mode()
{
    return to_underlying(ResourceLoader::navigator_compatibility_mode());
}

}
