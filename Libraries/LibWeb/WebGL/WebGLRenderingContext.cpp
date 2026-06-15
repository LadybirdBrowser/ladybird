/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLContextEvent.h>
#include <LibWeb/Bindings/WebGLRenderingContext.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/RemoteWebGLTransport.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>
#include <LibWeb/WebGL/WebGLContextProxy.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLRenderingContext);

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-event
// Returns false if the event was canceled (the page called preventDefault), which is how
// webglcontextlost signals that the page wants the context restored.
bool fire_webgl_context_event(HTML::HTMLCanvasElement& canvas_element, FlyString const& type)
{
    // To fire a WebGL context event named e means that an event using the WebGLContextEvent interface, with its type attribute [DOM4] initialized to e, its cancelable attribute initialized to true, and its isTrusted attribute [DOM4] initialized to true, is to be dispatched at the given object.
    // FIXME: Consider setting a status message.
    auto event = WebGLContextEvent::create(canvas_element.realm(), type, Bindings::WebGLContextEventInit {});
    event->set_is_trusted(true);
    event->set_cancelable(true);
    return canvas_element.dispatch_event(*event);
}

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-creation-error
void fire_webgl_context_creation_error(HTML::HTMLCanvasElement& canvas_element)
{
    // 1. Fire a WebGL context event named "webglcontextcreationerror" at canvas, optionally with its statusMessage attribute set to a platform dependent string about the nature of the failure.
    fire_webgl_context_event(canvas_element, EventNames::webglcontextcreationerror);
}

// The drawing buffer's creation-time size, clamped like set_size() clamps resizes;
// later resizes travel as SetDrawingBufferSize commands.
static Gfx::IntSize initial_drawing_buffer_size(HTML::HTMLCanvasElement& canvas_element)
{
    auto size = canvas_element.bitmap_size_for_canvas(1, 1);
    return {
        clamp(size.width(), 1, max_webgl_drawing_buffer_dimension),
        clamp(size.height(), 1, max_webgl_drawing_buffer_dimension),
    };
}

namespace {

struct RemoteWebGLContext {
    NonnullRefPtr<RemoteWebGLTransport> transport;
    RemoteWebGLTransport::CreateResult result;
};

Optional<RemoteWebGLContext> create_remote_webgl_context(HTML::HTMLCanvasElement& canvas_element, WebGLVersion webgl_version, WebGLContextAttributes const& context_attributes)
{
    auto& page = canvas_element.document().page();
    if (!page.has_compositor_host())
        return {};
    auto transport = page.compositor_host().create_webgl_transport();
    if (!transport)
        return {};

    auto result = transport->create_context(
        webgl_version,
        initial_drawing_buffer_size(canvas_element),
        context_attributes.depth,
        context_attributes.stencil,
        context_attributes.antialias);
    if (!result.success)
        return {};

    return RemoteWebGLContext { transport.release_nonnull(), move(result) };
}

}

OwnPtr<WebGLContextProxy> create_webgl_context_proxy(HTML::HTMLCanvasElement& canvas_element, WebGLVersion webgl_version, WebGLContextAttributes const& context_attributes)
{
    auto remote = create_remote_webgl_context(canvas_element, webgl_version, context_attributes);
    if (!remote.has_value())
        return {};

    return make<WebGLContextProxy>(move(remote->transport), webgl_version, move(remote->result.supported_extensions));
}

bool restore_webgl_context_proxy(WebGLContextProxy& context, HTML::HTMLCanvasElement& canvas_element, WebGLVersion webgl_version, WebGLContextAttributes const& context_attributes)
{
    auto remote = create_remote_webgl_context(canvas_element, webgl_version, context_attributes);
    if (!remote.has_value())
        return false;

    context.restore(move(remote->transport), move(remote->result.supported_extensions));
    return true;
}

JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> WebGLRenderingContext::create(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, JS::Value options)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element.
    auto context_attributes = TRY(convert_value_to_context_attributes_dictionary(canvas_element.vm(), options));

    auto context = create_webgl_context_proxy(canvas_element, WebGLVersion::WebGL1, context_attributes);
    if (!context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGLRenderingContext> { nullptr };
    }

    return realm.create<WebGLRenderingContext>(realm, canvas_element, context.release_nonnull(), context_attributes, context_attributes);
}

WebGLRenderingContext::WebGLRenderingContext(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, NonnullOwnPtr<WebGLContextProxy> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters)
    : WebGLRenderingContextOverloads(realm, move(context))
    , m_canvas_element(canvas_element)
    , m_context_creation_parameters(context_creation_parameters)
    , m_actual_context_parameters(actual_context_parameters)
{
}

WebGLRenderingContext::~WebGLRenderingContext() = default;

void WebGLRenderingContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLRenderingContext);
    Base::initialize(realm);
}

void WebGLRenderingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    WebGLRenderingContextImpl::visit_edges(visitor);
    visitor.visit(m_canvas_element);
}

void WebGLRenderingContext::prepare_for_compositing()
{
    context().present_canvas_for_compositing(m_context_creation_parameters.preserve_drawing_buffer);
}

bool WebGLRenderingContext::reestablish_remote_context()
{
    return restore_webgl_context_proxy(context(), *m_canvas_element, WebGLVersion::WebGL1, m_actual_context_parameters);
}

GC::Ref<HTML::HTMLCanvasElement> WebGLRenderingContext::canvas_for_binding() const
{
    return *m_canvas_element;
}

void WebGLRenderingContext::did_update_canvas_content()
{
    m_canvas_element->set_canvas_content_dirty();

    m_canvas_element->set_needs_repaint();
}

Optional<WebGLContextAttributes> WebGLRenderingContext::get_context_attributes()
{
    if (is_context_lost())
        return {};
    return m_actual_context_parameters;
}

void WebGLRenderingContext::set_size(Gfx::IntSize const& size)
{
    Gfx::IntSize final_size;
    final_size.set_width(clamp(size.width(), 1, max_webgl_drawing_buffer_dimension));
    final_size.set_height(clamp(size.height(), 1, max_webgl_drawing_buffer_dimension));
    context().set_size(final_size);
}

void WebGLRenderingContext::reset_to_default_state()
{
}

WebIDL::Long WebGLRenderingContext::drawing_buffer_width() const
{
    auto size = canvas_for_binding()->bitmap_size_for_canvas();
    return min(size.width(), max_webgl_drawing_buffer_dimension);
}

WebIDL::Long WebGLRenderingContext::drawing_buffer_height() const
{
    auto size = canvas_for_binding()->bitmap_size_for_canvas();
    return min(size.height(), max_webgl_drawing_buffer_dimension);
}

}
