/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibWeb/Export.h>

namespace Web::WebGL {

class WEB_API OpenGLContext {
public:
    enum class WebGLVersion {
        WebGL1,
        WebGL2,
    };

    static OwnPtr<OpenGLContext> create(NonnullRefPtr<Gfx::SkiaBackendContext> const&, WebGLVersion);

    void notify_content_will_change();
    void clear_buffer_to_default_values();
    void allocate_painting_surface_if_needed();

    struct Impl;
    OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext>, Impl, WebGLVersion);

    ~OpenGLContext();

    void make_current();

    void present(bool preserve_drawing_buffer);

    void set_size(Gfx::IntSize const&);

    RefPtr<Gfx::PaintingSurface> surface();

    u32 default_framebuffer() const;
    u32 default_renderbuffer() const;

    Vector<String> get_supported_extensions();
    void request_extension(char const* extension_name);

private:
    NonnullRefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Gfx::IntSize m_size;
    RefPtr<Gfx::PaintingSurface> m_painting_surface;
    NonnullOwnPtr<Impl> m_impl;
    Optional<Vector<String>> m_requestable_extensions;
    WebGLVersion m_webgl_version;

    void free_surface_resources();
#if defined(AK_OS_MACOS)
    void allocate_iosurface_painting_surface();
#elif defined(USE_VULKAN_IMAGES)
    void allocate_vkimage_painting_surface();
#endif
};

}
