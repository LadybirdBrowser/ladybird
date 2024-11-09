/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Bitmap.h>
#include <LibWeb/WebGL/Types.h>

namespace Web::WebGL {

class OpenGLContext {
public:
    static OwnPtr<OpenGLContext> create(Gfx::PaintingSurface&);

    virtual void present() = 0;
    void clear_buffer_to_default_values();

    virtual GLenum gl_get_error() = 0;
    virtual void gl_get_doublev(GLenum, GLdouble*) = 0;
    virtual void gl_get_integerv(GLenum, GLint*) = 0;
    virtual void gl_clear(GLbitfield) = 0;
    virtual void gl_clear_color(GLfloat, GLfloat, GLfloat, GLfloat) = 0;
    virtual void gl_clear_depth(GLdouble) = 0;
    virtual void gl_clear_stencil(GLint) = 0;
    virtual void gl_active_texture(GLenum) = 0;
    virtual void gl_viewport(GLint, GLint, GLsizei, GLsizei) = 0;
    virtual void gl_line_width(GLfloat) = 0;
    virtual void gl_polygon_offset(GLfloat, GLfloat) = 0;
    virtual void gl_scissor(GLint, GLint, GLsizei, GLsizei) = 0;
    virtual void gl_depth_mask(GLboolean) = 0;
    virtual void gl_depth_func(GLenum) = 0;
    virtual void gl_depth_range(GLdouble, GLdouble) = 0;
    virtual void gl_cull_face(GLenum) = 0;
    virtual void gl_color_mask(GLboolean, GLboolean, GLboolean, GLboolean) = 0;
    virtual void gl_front_face(GLenum) = 0;
    virtual void gl_finish() = 0;
    virtual void gl_flush() = 0;
    virtual void gl_stencil_op_separate(GLenum, GLenum, GLenum, GLenum) = 0;

    virtual ~OpenGLContext() { }
};

}
