/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Error.h>
#include <LibAccelGfx/Context.h>

#ifdef AK_OS_MACOS
#    define GL_SILENCE_DEPRECATION
#    include <OpenGL/CGLRenderers.h>
#    include <OpenGL/CGLTypes.h>
#    include <OpenGL/OpenGL.h>
#    include <OpenGL/gl3.h>
#endif

namespace AccelGfx {

#ifdef AK_OS_MACOS
class CGLContextWrapper : public Context {
public:
    CGLContextWrapper(CGLContextObj context)
        : m_context(context)
    {
    }

    virtual void activate() override
    {
        CGLSetCurrentContext(m_context);
    }

    ~CGLContextWrapper()
    {
        CGLReleaseContext(m_context);
    }

private:
    CGLContextObj m_context;
};
#else
class EGLContextWrapper : public Context {
public:
    EGLContextWrapper(EGLContext context)
        : m_context(context)
    {
    }

    ~EGLContextWrapper()
    {
        eglDestroyContext(eglGetCurrentDisplay(), m_context);
    }

    virtual void activate() override
    {
        eglMakeCurrent(eglGetCurrentDisplay(), EGL_NO_SURFACE, EGL_NO_SURFACE, m_context);
    }

private:
    EGLContext m_context;
};
#endif

#ifdef AK_OS_MACOS
static ErrorOr<NonnullOwnPtr<CGLContextWrapper>> make_context_cgl()
{
    CGLContextObj context = NULL;
    CGLPixelFormatAttribute attributes[4] = {
        kCGLPFAOpenGLProfile,
        (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
        kCGLPFAAccelerated,
        (CGLPixelFormatAttribute)0
    };

    CGLPixelFormatObj pixelFormat = NULL;
    GLint numPixelFormats = 0;
    CGLError error = CGLChoosePixelFormat(attributes, &pixelFormat, &numPixelFormats);
    if (error) {
        auto const* cgl_error_string = CGLErrorString(error);
        return Error::from_string_view(StringView { cgl_error_string, strlen(cgl_error_string) });
    }

    error = CGLCreateContext(pixelFormat, NULL, &context);
    if (error) {
        auto const* cgl_error_string = CGLErrorString(error);
        return Error::from_string_view(StringView { cgl_error_string, strlen(cgl_error_string) });
    }

    error = CGLSetCurrentContext(context);
    if (error) {
        auto const* cgl_error_string = CGLErrorString(error);
        return Error::from_string_view(StringView { cgl_error_string, strlen(cgl_error_string) });
    }

    VERIFY(glGetError() == GL_NO_ERROR);

    return make<CGLContextWrapper>(context);
}
#else

static StringView format_egl_error(EGLint error)
{
    switch (error) {
    case EGL_SUCCESS:
        return "EGL_SUCCESS"sv;
    case EGL_NOT_INITIALIZED:
        return "EGL_NOT_INITIALIZED"sv;
    case EGL_BAD_ACCESS:
        return "EGL_BAD_ACCESS"sv;
    case EGL_BAD_ALLOC:
        return "EGL_BAD_ALLOC"sv;
    case EGL_BAD_ATTRIBUTE:
        return "EGL_BAD_ATTRIBUTE"sv;
    case EGL_BAD_CONTEXT:
        return "EGL_BAD_CONTEXT"sv;
    case EGL_BAD_CONFIG:
        return "EGL_BAD_CONFIG"sv;
    case EGL_BAD_CURRENT_SURFACE:
        return "EGL_BAD_CURRENT_SURFACE"sv;
    case EGL_BAD_DISPLAY:
        return "EGL_BAD_DISPLAY"sv;
    case EGL_BAD_SURFACE:
        return "EGL_BAD_SURFACE"sv;
    case EGL_BAD_MATCH:
        return "EGL_BAD_MATCH"sv;
    case EGL_BAD_PARAMETER:
        return "EGL_BAD_PARAMETER"sv;
    case EGL_BAD_NATIVE_PIXMAP:
        return "EGL_BAD_NATIVE_PIXMAP"sv;
    case EGL_BAD_NATIVE_WINDOW:
        return "EGL_BAD_NATIVE_WINDOW"sv;
    case EGL_CONTEXT_LOST:
        return "EGL_CONTEXT_LOST"sv;
    default:
        return "Unknown error"sv;
    }
}

static ErrorOr<NonnullOwnPtr<EGLContextWrapper>> make_context_egl()
{
    EGLDisplay egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);

    EGLint major;
    EGLint minor;
    eglInitialize(egl_display, &major, &minor);

    EGLBoolean ok = eglBindAPI(EGL_OPENGL_API);
    if (ok == EGL_FALSE) {
        dbgln("eglBindAPI failed");
        VERIFY_NOT_REACHED();
    }

    static EGLint const config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_BLUE_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_RED_SIZE, 8,
        EGL_DEPTH_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };

    EGLConfig egl_config;
    EGLint num_configs;
    eglChooseConfig(egl_display, config_attributes, &egl_config, 1, &num_configs);

    static EGLint const context_attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_NONE
    };
    EGLContext egl_context = eglCreateContext(egl_display, egl_config, EGL_NO_CONTEXT, context_attributes);
    if (egl_context == EGL_FALSE) {
        return Error::from_string_view(format_egl_error(eglGetError()));
    }

    EGLBoolean result = eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context);
    if (result == EGL_FALSE) {
        return Error::from_string_view(format_egl_error(eglGetError()));
    }

    return make<EGLContextWrapper>(egl_context);
}
#endif

ErrorOr<NonnullOwnPtr<Context>> Context::create()
{
#ifdef AK_OS_MACOS
    return make_context_cgl();
#else
    return make_context_egl();
#endif
}

}
