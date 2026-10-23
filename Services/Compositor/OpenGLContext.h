/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <Compositor/PaintingSurface.h>
#include <LibCompositing/WebGL/GLFunctions.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

class SkImage;

#if defined(AK_OS_MACOS) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)) || defined(AK_OS_WINDOWS)
#    define ENABLE_WEBGL_CPU_PAINTING_SURFACE
#endif

namespace Compositor {

class OpenGLContext : public Compositing::WebGL::GLFunctions {
public:
    AK_ALLOC_WITH_KMALLOC;

    using WebGLVersion = Compositing::WebGL::WebGLVersion;

    struct DrawingBufferOptions {
        bool depth;
        bool stencil;
        bool antialias;
    };

    static OwnPtr<OpenGLContext> create(RefPtr<Gfx::SkiaBackendContext>, WebGLVersion, DrawingBufferOptions);

    void clear_buffer_to_default_values();
    void allocate_painting_surface_if_needed();

    struct Impl;
    OpenGLContext(RefPtr<Gfx::SkiaBackendContext>, Impl, WebGLVersion, DrawingBufferOptions);

    ~OpenGLContext();

    void make_current();

    // Makes everything drawn so far visible to readers of the drawing buffer's surface.
    void flush_drawing_buffer();
    // Hands the drawing buffer over for compositing and moves the page onto another one, so drawing the next frame
    // cannot change the frame being composited.
    void present_drawing_buffer(bool preserve_drawing_buffer);

    void set_size(Gfx::IntSize const&);

    RefPtr<Gfx::PaintingSurface> drawing_buffer_surface();
    RefPtr<Gfx::PaintingSurface> presented_surface();

    u32 default_framebuffer() const;
    u32 default_renderbuffer() const;

    Vector<String> get_supported_opengl_extensions();

    // Keeps the errors WebGL has yet to collect apart from those of the Compositor's own GL work.
    GLenum get_error();
    bool stash_errors();
    void discard_errors();

    WebGLVersion webgl_version() const { return m_webgl_version; }
    bool can_copy_images_into_textures();
    // The source size is the part of the image to copy, which can be smaller than the image.
    bool copy_image_into_bound_texture(SkImage const&, Gfx::IntSize source_size, Gfx::IntSize, GLenum target, GLint level, GLint xoffset, GLint yoffset, bool flip_y, bool premultiply_alpha);

private:
    struct ColorBuffer;

    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Gfx::IntSize m_size;
    bool m_drawing_buffer_needs_allocation { true };
    NonnullOwnPtr<Impl> m_impl;
    WebGLVersion m_webgl_version;
    [[maybe_unused]] DrawingBufferOptions m_drawing_buffer_options;
    Vector<GLenum> m_stashed_errors;

    void free_surface_resources();
    void free_color_buffer(ColorBuffer&);
    bool allocate_color_buffer(ColorBuffer&);
    bool allocate_shared_color_buffer(ColorBuffer&, Gfx::IntSize, Gfx::PaintingSurface::Origin);
    void attach_color_buffer_to_default_framebuffer(ColorBuffer const&);
    u32 color_buffer_texture_target() const;
    bool supports_copy_texture();
#if defined(AK_OS_MACOS)
    void allocate_iosurface_color_buffer(ColorBuffer&, Gfx::IntSize, Gfx::PaintingSurface::Origin);
#endif
#if defined(USE_VULKAN_DMABUF_IMAGES)
    bool allocate_vkimage_color_buffer(ColorBuffer&, Gfx::IntSize, Gfx::PaintingSurface::Origin);
#endif
#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
    void allocate_cpu_painting_surface(ColorBuffer&);
    void copy_default_framebuffer_to_cpu_painting_surface();
#endif
};

}
