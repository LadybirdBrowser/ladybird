/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLRenderingContextPrototype.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/TraversableNavigable.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/WebGL/EventNames.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLContextEvent.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLRenderingContext.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebIDL/Buffers.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLRenderingContext);

#define RETURN_WITH_WEBGL_ERROR_IF(condition, error)                         \
    if (condition) {                                                         \
        dbgln_if(WEBGL_CONTEXT_DEBUG, "{}(): error {:#x}", __func__, error); \
        set_error(error);                                                    \
        return;                                                              \
    }

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-event
static void fire_webgl_context_event(HTML::HTMLCanvasElement& canvas_element, FlyString const& type)
{
    // To fire a WebGL context event named e means that an event using the WebGLContextEvent interface, with its type attribute [DOM4] initialized to e, its cancelable attribute initialized to true, and its isTrusted attribute [DOM4] initialized to true, is to be dispatched at the given object.
    // FIXME: Consider setting a status message.
    auto event = WebGLContextEvent::create(canvas_element.realm(), type, WebGLContextEventInit {});
    event->set_is_trusted(true);
    event->set_cancelable(true);
    canvas_element.dispatch_event(*event);
}

// https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-creation-error
static void fire_webgl_context_creation_error(HTML::HTMLCanvasElement& canvas_element)
{
    // 1. Fire a WebGL context event named "webglcontextcreationerror" at canvas, optionally with its statusMessage attribute set to a platform dependent string about the nature of the failure.
    fire_webgl_context_event(canvas_element, EventNames::webglcontextcreationerror);
}

JS::ThrowCompletionOr<GC::Ptr<WebGLRenderingContext>> WebGLRenderingContext::create(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, JS::Value options)
{
    // We should be coming here from getContext being called on a wrapped <canvas> element.
    auto context_attributes = TRY(convert_value_to_context_attributes_dictionary(canvas_element.vm(), options));

    auto context = OpenGLContext::create();

    if (!context) {
        fire_webgl_context_creation_error(canvas_element);
        return GC::Ptr<WebGLRenderingContext> { nullptr };
    }

    return realm.create<WebGLRenderingContext>(realm, canvas_element, context.release_nonnull(), context_attributes, context_attributes);
}

WebGLRenderingContext::WebGLRenderingContext(JS::Realm& realm, HTML::HTMLCanvasElement& canvas_element, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters)
    : PlatformObject(realm)
    , m_context(move(context))
    , m_canvas_element(canvas_element)
    , m_context_creation_parameters(context_creation_parameters)
    , m_actual_context_parameters(actual_context_parameters)
{
}

WebGLRenderingContext::~WebGLRenderingContext() = default;

void WebGLRenderingContext::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLRenderingContext);
}

void WebGLRenderingContext::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_canvas_element);
}

void WebGLRenderingContext::present()
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
        m_context->clear_buffer_to_default_values();
    }
}

GC::Ref<HTML::HTMLCanvasElement> WebGLRenderingContext::canvas_for_binding() const
{
    return *m_canvas_element;
}

void WebGLRenderingContext::needs_to_present()
{
    m_should_present = true;

    if (!m_canvas_element->paintable())
        return;
    m_canvas_element->paintable()->set_needs_display();
}

void WebGLRenderingContext::set_error(GLenum error)
{
    auto context_error = glGetError();
    if (context_error != GL_NO_ERROR)
        m_error = context_error;
    else
        m_error = error;
}

bool WebGLRenderingContext::is_context_lost() const
{
    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::is_context_lost()");
    return m_context_lost;
}

void WebGLRenderingContext::set_size(Gfx::IntSize const&)
{
    TODO();
}

void WebGLRenderingContext::reset_to_default_state()
{
}

RefPtr<Gfx::PaintingSurface> WebGLRenderingContext::surface()
{
    TODO();
}

Optional<Vector<String>> WebGLRenderingContext::get_supported_extensions() const
{
    if (m_context_lost)
        return Optional<Vector<String>> {};

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::get_supported_extensions()");

    // FIXME: We don't currently support any extensions.
    return Vector<String> {};
}

JS::Object* WebGLRenderingContext::get_extension(String const& name) const
{
    if (m_context_lost)
        return nullptr;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::get_extension(name='{}')", name);

    // FIXME: We don't currently support any extensions.
    return nullptr;
}

void WebGLRenderingContext::active_texture(GLenum texture)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::active_texture(texture={:#08x})", texture);
    m_context->gl_active_texture(texture);
}

void WebGLRenderingContext::clear(GLbitfield mask)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::clear(mask={:#08x})", mask);
    m_context->gl_clear(mask);

    // FIXME: This should only be done if this is targeting the front buffer.
    needs_to_present();
}

void WebGLRenderingContext::clear_color(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::clear_color(red={}, green={}, blue={}, alpha={})", red, green, blue, alpha);
    m_context->gl_clear_color(red, green, blue, alpha);
}

void WebGLRenderingContext::clear_depth(GLclampf depth)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::clear_depth(depth={})", depth);
    m_context->gl_clear_depth(depth);
}

void WebGLRenderingContext::clear_stencil(GLint s)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::clear_stencil(s={:#08x})", s);
    m_context->gl_clear_stencil(s);
}

void WebGLRenderingContext::color_mask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::color_mask(red={}, green={}, blue={}, alpha={})", red, green, blue, alpha);
    m_context->gl_color_mask(red, green, blue, alpha);
}

void WebGLRenderingContext::cull_face(GLenum mode)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::cull_face(mode={:#08x})", mode);
    m_context->gl_cull_face(mode);
}

void WebGLRenderingContext::depth_func(GLenum func)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::depth_func(func={:#08x})", func);
    m_context->gl_depth_func(func);
}

void WebGLRenderingContext::depth_mask(GLboolean mask)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::depth_mask(mask={})", mask);
    m_context->gl_depth_mask(mask);
}

void WebGLRenderingContext::depth_range(GLclampf z_near, GLclampf z_far)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::depth_range(z_near={}, z_far={})", z_near, z_far);

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#VIEWPORT_DEPTH_RANGE
    // "The WebGL API does not support depth ranges with where the near plane is mapped to a value greater than that of the far plane. A call to depthRange will generate an INVALID_OPERATION error if zNear is greater than zFar."
    RETURN_WITH_WEBGL_ERROR_IF(z_near > z_far, GL_INVALID_OPERATION);
    m_context->gl_depth_range(z_near, z_far);
}

void WebGLRenderingContext::finish()
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::finish()");
    m_context->gl_finish();
}

void WebGLRenderingContext::flush()
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::flush()");
    m_context->gl_flush();
}

void WebGLRenderingContext::front_face(GLenum mode)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::front_face(mode={:#08x})", mode);
    m_context->gl_front_face(mode);
}

GLenum WebGLRenderingContext::get_error()
{
    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::get_error()");

    // "If the context's webgl context lost flag is set, returns CONTEXT_LOST_WEBGL the first time this method is called. Afterward, returns NO_ERROR until the context has been restored."
    // FIXME: The plan here is to make the context lost handler unconditionally set m_error to CONTEXT_LOST_WEBGL, which we currently do not have.
    //        The idea for the unconditional set is that any potentially error generating functions will not execute when the context is lost.
    if (m_error != GL_NO_ERROR || m_context_lost) {
        auto last_error = m_error;
        m_error = GL_NO_ERROR;
        return last_error;
    }

    return m_context->gl_get_error();
}

void WebGLRenderingContext::line_width(GLfloat width)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::line_width(width={})", width);

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#NAN_LINE_WIDTH
    // "In the WebGL API, if the width parameter passed to lineWidth is set to NaN, an INVALID_VALUE error is generated and the line width is not changed."
    RETURN_WITH_WEBGL_ERROR_IF(isnan(width), GL_INVALID_VALUE);
    m_context->gl_line_width(width);
}

void WebGLRenderingContext::polygon_offset(GLfloat factor, GLfloat units)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::polygon_offset(factor={}, units={})", factor, units);
    m_context->gl_polygon_offset(factor, units);
}

void WebGLRenderingContext::scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::scissor(x={}, y={}, width={}, height={})", x, y, width, height);
    m_context->gl_scissor(x, y, width, height);
}

void WebGLRenderingContext::stencil_op(GLenum fail, GLenum zfail, GLenum zpass)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::stencil_op(fail={:#08x}, zfail={:#08x}, zpass={:#08x})", fail, zfail, zpass);
    m_context->gl_stencil_op_separate(GL_FRONT_AND_BACK, fail, zfail, zpass);
}

void WebGLRenderingContext::stencil_op_separate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::stencil_op_separate(face={:#08x}, fail={:#08x}, zfail={:#08x}, zpass={:#08x})", face, fail, zfail, zpass);
    m_context->gl_stencil_op_separate(face, fail, zfail, zpass);
}

void WebGLRenderingContext::viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    if (m_context_lost)
        return;

    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::viewport(x={}, y={}, width={}, height={})", x, y, width, height);
    m_context->gl_viewport(x, y, width, height);
}

}
