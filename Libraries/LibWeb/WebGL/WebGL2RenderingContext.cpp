/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SkiaBackendContext.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGL2RenderingContext.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGL2RenderingContext.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGL2RenderingContext);

JS::ThrowCompletionOr<GC::Ptr<WebGL2RenderingContext>> WebGL2RenderingContext::create(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, JS::Value options)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element.
    auto context_attributes = TRY(convert_value_to_context_attributes_dictionary(canvas_element.vm(), options));

    auto skia_backend_context = Gfx::SkiaBackendContext::the();
    if (!skia_backend_context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGL2RenderingContext> { nullptr };
    }
    OpenGLContext::DrawingBufferOptions context_options {
        .depth = context_attributes.depth,
        .stencil = context_attributes.stencil,
        .antialias = context_attributes.antialias,
    };
    auto context = OpenGLContext::create(*skia_backend_context, OpenGLContext::WebGLVersion::WebGL2, context_options);
    if (!context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGL2RenderingContext> { nullptr };
    }

    context->set_size(canvas_element.bitmap_size_for_canvas(1, 1));

    return realm.create<WebGL2RenderingContext>(realm, canvas_element, context.release_nonnull(), context_attributes, context_attributes);
}

WebGL2RenderingContext::WebGL2RenderingContext(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters)
    : WebGL2RenderingContextOverloads(realm, move(context))
    , m_canvas_element(canvas_element)
    , m_context_creation_parameters(context_creation_parameters)
    , m_actual_context_parameters(actual_context_parameters)
{
}

WebGL2RenderingContext::~WebGL2RenderingContext() = default;

void WebGL2RenderingContext::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGL2RenderingContext);
    Base::initialize(realm);
}

void WebGL2RenderingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    WebGL2RenderingContextImpl::visit_edges(visitor);
    visitor.visit(m_canvas_element);
}

void WebGL2RenderingContext::present()
{
    context().present(m_context_creation_parameters.preserve_drawing_buffer);
}

GC::Ref<HTML::HTMLCanvasElement> WebGL2RenderingContext::canvas_for_binding() const
{
    return *m_canvas_element;
}

void WebGL2RenderingContext::needs_to_present()
{
    m_canvas_element->set_canvas_content_dirty();

    m_canvas_element->set_needs_repaint();
}

Optional<WebGLContextAttributes> WebGL2RenderingContext::get_context_attributes()
{
    if (is_context_lost())
        return {};
    return m_actual_context_parameters;
}

void WebGL2RenderingContext::set_size(Gfx::IntSize const& size)
{
    Gfx::IntSize final_size;
    final_size.set_width(max(size.width(), 1));
    final_size.set_height(max(size.height(), 1));
    context().set_size(final_size);
}

void WebGL2RenderingContext::reset_to_default_state()
{
}

RefPtr<Gfx::PaintingSurface> WebGL2RenderingContext::surface()
{
    return context().surface();
}

void WebGL2RenderingContext::allocate_painting_surface_if_needed()
{
    context().allocate_painting_surface_if_needed();
}

WebIDL::Long WebGL2RenderingContext::drawing_buffer_width() const
{
    auto size = canvas_for_binding()->bitmap_size_for_canvas();
    return size.width();
}

WebIDL::Long WebGL2RenderingContext::drawing_buffer_height() const
{
    auto size = canvas_for_binding()->bitmap_size_for_canvas();
    return size.height();
}

}
