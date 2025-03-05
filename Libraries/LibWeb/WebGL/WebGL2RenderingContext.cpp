/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGL2RenderingContextPrototype.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Infra/Strings.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/Extensions/EXTColorBufferFloat.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
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

    auto skia_backend_context = canvas_element.navigable()->traversable_navigable()->skia_backend_context();
    if (!skia_backend_context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGL2RenderingContext> { nullptr };
    }
    auto context = OpenGLContext::create(*skia_backend_context);
    if (!context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGL2RenderingContext> { nullptr };
    }

    context->set_size(canvas_element.bitmap_size_for_canvas(1, 1));

    return realm.create<WebGL2RenderingContext>(realm, canvas_element, context.release_nonnull(), context_attributes, context_attributes);
}

WebGL2RenderingContext::WebGL2RenderingContext(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters)
    : PlatformObject(realm)
    , WebGL2RenderingContextImpl(realm, move(context))
    , m_canvas_element(canvas_element)
    , m_context_creation_parameters(context_creation_parameters)
    , m_actual_context_parameters(actual_context_parameters)
{
}

WebGL2RenderingContext::~WebGL2RenderingContext() = default;

void WebGL2RenderingContext::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGL2RenderingContext);
}

void WebGL2RenderingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    WebGL2RenderingContextImpl::visit_edges(visitor);
    visitor.visit(m_canvas_element);
    visitor.visit(m_ext_color_buffer_float_extension);
    visitor.visit(m_webgl_compressed_texture_s3tc_extension);
}

void WebGL2RenderingContext::present()
{
    if (!m_should_present)
        return;

    m_should_present = false;

    // "Before the drawing buffer is presented for compositing the implementation shall ensure that all rendering operations have been flushed to the drawing buffer."
    glFlush();

    // "By default, after compositing the contents of the drawing buffer shall be cleared to their default values, as shown in the table above.
    // This default behavior can be changed by setting the preserveDrawingBuffer attribute of the WebGLContextAttributes object.
    // If this flag is true, the contents of the drawing buffer shall be preserved until the author either clears or overwrites them."
    if (!m_context_creation_parameters.preserve_drawing_buffer) {
        context().clear_buffer_to_default_values();
    }
}

GC::Ref<HTML::HTMLCanvasElement> WebGL2RenderingContext::canvas_for_binding() const
{
    return *m_canvas_element;
}

void WebGL2RenderingContext::needs_to_present()
{
    m_should_present = true;

    if (!m_canvas_element->paintable())
        return;
    m_canvas_element->paintable()->set_needs_display();
}

void WebGL2RenderingContext::set_error(GLenum error)
{
    auto context_error = glGetError();
    if (context_error != GL_NO_ERROR)
        m_error = context_error;
    else
        m_error = error;
}

bool WebGL2RenderingContext::is_context_lost() const
{
    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::is_context_lost()");
    return m_context_lost;
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

Optional<Vector<String>> WebGL2RenderingContext::get_supported_extensions()
{
    return context().get_supported_extensions();
}

JS::Object* WebGL2RenderingContext::get_extension(String const& name)
{
    // Returns an object if, and only if, name is an ASCII case-insensitive match [HTML] for one of the names returned
    // from getSupportedExtensions; otherwise, returns null. The object returned from getExtension contains any constants
    // or functions provided by the extension. A returned object may have no constants or functions if the extension does
    // not define any, but a unique object must still be returned. That object is used to indicate that the extension has
    // been enabled.
    auto supported_extensions = get_supported_extensions();
    auto supported_extension_iterator = supported_extensions->find_if([&name](String const& supported_extension) {
        return Infra::is_ascii_case_insensitive_match(supported_extension, name);
    });
    if (supported_extension_iterator == supported_extensions->end())
        return nullptr;

    if (Infra::is_ascii_case_insensitive_match(name, "WEBGL_compressed_texture_s3tc"sv)) {
        if (!m_webgl_compressed_texture_s3tc_extension) {
            m_webgl_compressed_texture_s3tc_extension = MUST(Extensions::WebGLCompressedTextureS3tc::create(realm(), this));
        }

        VERIFY(m_webgl_compressed_texture_s3tc_extension);
        return m_webgl_compressed_texture_s3tc_extension;
    }

    if (Infra::is_ascii_case_insensitive_match(name, "EXT_color_buffer_float"sv)) {
        if (!m_ext_color_buffer_float_extension) {
            m_ext_color_buffer_float_extension = MUST(Extensions::EXTColorBufferFloat::create(realm(), *this));
        }

        VERIFY(m_ext_color_buffer_float_extension);
        return m_ext_color_buffer_float_extension;
    }

    return nullptr;
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
