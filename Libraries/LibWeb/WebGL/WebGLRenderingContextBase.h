/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLContextAttributes.h>

namespace Web::WebGL {

#define GL_NO_ERROR 0

class WebGLRenderingContextBase : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(WebGLRenderingContextBase, Bindings::PlatformObject);

public:
    virtual ~WebGLRenderingContextBase();

    void present();

    GC::Ref<HTML::HTMLCanvasElement> canvas_for_binding() const;

    bool is_context_lost() const;

    Optional<Vector<String>> get_supported_extensions() const;
    JS::Object* get_extension(String const& name) const;

    void active_texture(GLenum texture);

    void clear(GLbitfield mask);
    void clear_color(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha);
    void clear_depth(GLclampf depth);
    void clear_stencil(GLint s);
    void color_mask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);

    void cull_face(GLenum mode);

    void depth_func(GLenum func);
    void depth_mask(GLboolean mask);
    void depth_range(GLclampf z_near, GLclampf z_far);

    void finish();
    void flush();

    void front_face(GLenum mode);

    GLenum get_error();

    void line_width(GLfloat width);
    void polygon_offset(GLfloat factor, GLfloat units);

    void scissor(GLint x, GLint y, GLsizei width, GLsizei height);

    void stencil_op(GLenum fail, GLenum zfail, GLenum zpass);
    void stencil_op_separate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass);

    void viewport(GLint x, GLint y, GLsizei width, GLsizei height);

protected:
    WebGLRenderingContextBase(JS::Realm&, HTML::HTMLCanvasElement& canvas_element, NonnullOwnPtr<OpenGLContext> context, WebGLContextAttributes context_creation_parameters, WebGLContextAttributes actual_context_parameters);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::HTMLCanvasElement> m_canvas_element;

    NonnullOwnPtr<OpenGLContext> m_context;

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#context-creation-parameters
    // Each WebGLRenderingContext has context creation parameters, set upon creation, in a WebGLContextAttributes object.
    WebGLContextAttributes m_context_creation_parameters {};

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#actual-context-parameters
    // Each WebGLRenderingContext has actual context parameters, set each time the drawing buffer is created, in a WebGLContextAttributes object.
    WebGLContextAttributes m_actual_context_parameters {};

    // https://www.khronos.org/registry/webgl/specs/latest/1.0/#webgl-context-lost-flag
    // Each WebGLRenderingContext has a webgl context lost flag, which is initially unset.
    bool m_context_lost { false };

    // WebGL presents its drawing buffer to the HTML page compositor immediately before a compositing operation, but only if at least one of the following has occurred since the previous compositing operation:
    // - Context creation
    // - Canvas resize
    // - clear, drawArrays, or drawElements has been called while the drawing buffer is the currently bound framebuffer
    bool m_should_present { true };

    GLenum m_error { GL_NO_ERROR };

    HTML::HTMLCanvasElement& canvas_element();
    HTML::HTMLCanvasElement const& canvas_element() const;

    void needs_to_present();
    void set_error(GLenum error);
};

}
