/*
 * Copyright (c) 2024, Olekoop <mlglol360xd@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibCore/EGLInterface.h>

namespace Core {

static bool s_has_been_created = false;

ErrorOr<void> create_egl_interface()
{
    if (s_has_been_created)
        return {};

    EGLDisplay display;
    EGLint num_configs;
    EGLConfig config;

    EGLint const config_attributes[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_NONE
    };

    EGLint const pbuffer_attributes[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_TEXTURE_TARGET, EGL_NO_TEXTURE,
        EGL_TEXTURE_FORMAT, EGL_NO_TEXTURE,
        EGL_NONE
    };

    EGLint const context_atributes[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };

    if ((display = eglGetDisplay(EGL_DEFAULT_DISPLAY)) == EGL_NO_DISPLAY)
        return Error::from_string_literal("eglGetDisplay() failed");

    if (!eglInitialize(display, 0, 0))
        return Error::from_string_literal("eglInitialize() failed");

    if (!eglChooseConfig(display, config_attributes, &config, 1, &num_configs))
        return Error::from_string_literal("eglChooseConfig() failed");

    EGLContext context = eglCreateContext(display, config, nullptr, context_atributes);
    if (context == EGL_NO_CONTEXT)
        return Error::from_string_literal("eglCreateContext() failed");

    EGLSurface surface = eglCreatePbufferSurface(display, config, pbuffer_attributes);
    if (surface == EGL_NO_SURFACE)
        return Error::from_errno(eglGetError());

    if (!eglMakeCurrent(display, surface, surface, context))
        return Error::from_string_literal("eglMakeCurrent() failed");

    s_has_been_created = true;

    return {};
}

}
