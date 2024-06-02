/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibWeb/WebGL/OpenGLContext.h>

#ifdef HAS_ACCELERATED_GRAPHICS
#    include <LibAccelGfx/Canvas.h>
#    include <LibAccelGfx/Context.h>
#endif

namespace Web::WebGL {

#ifdef HAS_ACCELERATED_GRAPHICS
class AccelGfxContext : public OpenGLContext {
public:
    void activate()
    {
        m_context->activate();
    }

    virtual void present(Gfx::Bitmap& bitmap) override
    {
        VERIFY(bitmap.format() == Gfx::BitmapFormat::BGRA8888);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, bitmap.width(), bitmap.height(), GL_BGRA, GL_UNSIGNED_BYTE, bitmap.scanline(0));
    }

    virtual GLenum gl_get_error() override
    {
        activate();
        return glGetError();
    }

    virtual void gl_get_doublev(GLenum pname, GLdouble* params) override
    {
        activate();
        glGetDoublev(pname, params);
    }

    virtual void gl_get_integerv(GLenum pname, GLint* params) override
    {
        activate();
        glGetIntegerv(pname, params);
    }

    virtual void gl_clear(GLbitfield mask) override
    {
        activate();
        glClear(mask);
    }

    virtual void gl_clear_color(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha) override
    {
        activate();
        glClearColor(red, green, blue, alpha);
    }

    virtual void gl_clear_depth(GLdouble depth) override
    {
        activate();
        glClearDepth(depth);
    }

    virtual void gl_clear_stencil(GLint s) override
    {
        activate();
        glClearStencil(s);
    }

    virtual void gl_active_texture(GLenum texture) override
    {
        activate();
        glActiveTexture(texture);
    }

    virtual void gl_viewport(GLint x, GLint y, GLsizei width, GLsizei height) override
    {
        activate();
        glViewport(x, y, width, height);
    }

    virtual void gl_line_width(GLfloat width) override
    {
        activate();
        glLineWidth(width);
    }

    virtual void gl_polygon_offset(GLfloat factor, GLfloat units) override
    {
        activate();
        glPolygonOffset(factor, units);
    }

    virtual void gl_scissor(GLint x, GLint y, GLsizei width, GLsizei height) override
    {
        activate();
        glScissor(x, y, width, height);
    }

    virtual void gl_depth_mask(GLboolean mask) override
    {
        activate();
        glDepthMask(mask);
    }

    virtual void gl_depth_func(GLenum func) override
    {
        activate();
        glDepthFunc(func);
    }

    virtual void gl_depth_range(GLdouble z_near, GLdouble z_far) override
    {
        activate();
        glDepthRange(z_near, z_far);
    }

    virtual void gl_cull_face(GLenum mode) override
    {
        activate();
        glCullFace(mode);
    }

    virtual void gl_color_mask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha) override
    {
        activate();
        glColorMask(red, green, blue, alpha);
    }

    virtual void gl_front_face(GLenum mode) override
    {
        activate();
        glFrontFace(mode);
    }

    virtual void gl_finish() override
    {
        activate();
        glFinish();
    }

    virtual void gl_flush() override
    {
        activate();
        glFlush();
    }

    virtual void gl_stencil_op_separate(GLenum, GLenum, GLenum, GLenum) override
    {
        TODO();
    }

    AccelGfxContext(NonnullOwnPtr<AccelGfx::Context> context, NonnullRefPtr<AccelGfx::Canvas> canvas)
        : m_context(move(context))
        , m_canvas(move(canvas))
    {
    }

    ~AccelGfxContext()
    {
        activate();
    }

private:
    NonnullOwnPtr<AccelGfx::Context> m_context;
    NonnullRefPtr<AccelGfx::Canvas> m_canvas;
};
#endif

#ifdef HAS_ACCELERATED_GRAPHICS
static OwnPtr<AccelGfxContext> make_accelgfx_context(Gfx::Bitmap& bitmap)
{
    auto context = AccelGfx::Context::create();
    if (context.is_error()) {
        dbgln("Failed to create AccelGfx context: {}", context.error().string_literal());
        return {};
    }
    auto canvas = AccelGfx::Canvas::create(bitmap.size());
    canvas->bind();
    return make<AccelGfxContext>(context.release_value(), move(canvas));
}
#endif

OwnPtr<OpenGLContext> OpenGLContext::create(Gfx::Bitmap& bitmap)
{
#ifdef HAS_ACCELERATED_GRAPHICS
    return make_accelgfx_context(bitmap);
#endif

    (void)bitmap;
    return {};
}

void OpenGLContext::clear_buffer_to_default_values()
{
#if defined(HAS_ACCELERATED_GRAPHICS)
    Array<GLdouble, 4> current_clear_color;
    gl_get_doublev(GL_COLOR_CLEAR_VALUE, current_clear_color.data());

    GLdouble current_clear_depth;
    gl_get_doublev(GL_DEPTH_CLEAR_VALUE, &current_clear_depth);

    GLint current_clear_stencil;
    gl_get_integerv(GL_STENCIL_CLEAR_VALUE, &current_clear_stencil);

    // The implicit clear value for the color buffer is (0, 0, 0, 0)
    gl_clear_color(0, 0, 0, 0);

    // The implicit clear value for the depth buffer is 1.0.
    gl_clear_depth(1.0);

    // The implicit clear value for the stencil buffer is 0.
    gl_clear_stencil(0);

    gl_clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // Restore the clear values.
    gl_clear_color(current_clear_color[0], current_clear_color[1], current_clear_color[2], current_clear_color[3]);
    gl_clear_depth(current_clear_depth);
    gl_clear_stencil(current_clear_stencil);
#endif
}

}
