/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/WebGL/OpenGLContext.h>

#ifdef AK_OS_MACOS
#    include <EGL/egl.h>
#    include <EGL/eglext.h>
#    include <EGL/eglext_angle.h>
#    include <GLES2/gl2.h>
#    include <GLES2/gl2ext.h>
#    include <GLES2/gl2ext_angle.h>
#endif

namespace Web::WebGL {

struct OpenGLContext::Impl {
#ifdef AK_OS_MACOS
    EGLDisplay display { nullptr };
    EGLConfig config { nullptr };
    EGLContext context { nullptr };
    EGLSurface surface { nullptr };

    GLuint framebuffer { 0 };
    GLuint depth_buffer { 0 };
#endif
};

OpenGLContext::OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(impl))
{
}

OpenGLContext::~OpenGLContext()
{
#ifdef AK_OS_MACOS
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    glDeleteFramebuffers(1, &m_impl->framebuffer);
    glDeleteRenderbuffers(1, &m_impl->depth_buffer);
    eglDestroyContext(m_impl->display, m_impl->context);
    eglDestroySurface(m_impl->display, m_impl->surface);
#endif
}

#ifdef AK_OS_MACOS
static EGLConfig get_egl_config(EGLDisplay display)
{
    EGLint const config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint number_of_configs;
    eglChooseConfig(display, config_attribs, NULL, 0, &number_of_configs);

    Vector<EGLConfig> configs;
    configs.resize(number_of_configs);
    eglChooseConfig(display, config_attribs, configs.data(), number_of_configs, &number_of_configs);
    return configs[0];
}
#endif

OwnPtr<OpenGLContext> OpenGLContext::create(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context)
{
#ifdef AK_OS_MACOS
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (display == EGL_NO_DISPLAY) {
        dbgln("Failed to get EGL display");
        return {};
    }

    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        dbgln("Failed to initialize EGL");
        return {};
    }

    auto* config = get_egl_config(display);

    EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        dbgln("Failed to create EGL context");
        return {};
    }

    return make<OpenGLContext>(skia_backend_context, Impl { .display = display, .config = config, .context = context });
#else
    (void)skia_backend_context;
    return nullptr;
#endif
}

void OpenGLContext::notify_content_will_change()
{
    m_painting_surface->notify_content_will_change();
}

void OpenGLContext::clear_buffer_to_default_values()
{
}

void OpenGLContext::allocate_painting_surface_if_needed()
{
#ifdef AK_OS_MACOS
    if (m_painting_surface)
        return;

    VERIFY(!m_size.is_empty());

    auto iosurface = Core::IOSurfaceHandle::create(m_size.width(), m_size.height());
    m_painting_surface = Gfx::PaintingSurface::wrap_iosurface(iosurface, m_skia_backend_context);
    m_painting_surface->set_flip_vertically();

    auto width = m_size.width();
    auto height = m_size.height();

    auto* display = m_impl->display;
    auto* config = m_impl->config;

    EGLint target = 0;
    eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &target);

    EGLint const surface_attributes[] = {
        EGL_WIDTH,
        width,
        EGL_HEIGHT,
        height,
        EGL_IOSURFACE_PLANE_ANGLE,
        0,
        EGL_TEXTURE_TARGET,
        target,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,
        GL_BGRA_EXT,
        EGL_TEXTURE_FORMAT,
        EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE,
        GL_UNSIGNED_BYTE,
        EGL_NONE,
        EGL_NONE,
    };
    m_impl->surface = eglCreatePbufferFromClientBuffer(display, EGL_IOSURFACE_ANGLE, iosurface.core_foundation_pointer(), config, surface_attributes);

    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);

    EGLint texture_target_angle = 0;
    eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &texture_target_angle);
    VERIFY(texture_target_angle == EGL_TEXTURE_RECTANGLE_ANGLE);

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_RECTANGLE_ANGLE, texture);
    auto result = eglBindTexImage(display, m_impl->surface, EGL_BACK_BUFFER);
    VERIFY(result == EGL_TRUE);

    glGenFramebuffers(1, &m_impl->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE_ANGLE, texture, 0);

    // NOTE: ANGLE doesn't allocate depth buffer for us, so we need to do it manually
    // FIXME: Depth buffer only needs to be allocated if it's configured in WebGL context attributes
    glGenRenderbuffers(1, &m_impl->depth_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
#endif
}

void OpenGLContext::set_size(Gfx::IntSize const& size)
{
    if (m_size != size) {
        m_painting_surface = nullptr;
    }
    m_size = size;
}

void OpenGLContext::make_current()
{
#ifdef AK_OS_MACOS
    allocate_painting_surface_if_needed();
    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);
#endif
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::surface()
{
    return m_painting_surface;
}

}
