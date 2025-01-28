/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/WebGL/OpenGLContext.h>

#ifdef AK_OS_MACOS
#    include <EGL/egl.h>
#    include <EGL/eglext.h>
#    include <EGL/eglext_angle.h>
#    define GL_GLEXT_PROTOTYPES 1
#    include <GLES2/gl2.h>
#    include <GLES2/gl2ext.h>
extern "C" {
#    include <GLES2/gl2ext_angle.h>
}
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
        EGL_CONTEXT_CLIENT_VERSION,
        2,
        EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE,
        EGL_TRUE,
        EGL_NONE,
        EGL_NONE,
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
#ifdef AK_OS_MACOS
    Array<GLfloat, 4> current_clear_color;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, current_clear_color.data());

    GLfloat current_clear_depth;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &current_clear_depth);

    GLint current_clear_stencil;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &current_clear_stencil);

    // The implicit clear value for the color buffer is (0, 0, 0, 0)
    glClearColor(0, 0, 0, 0);

    // The implicit clear value for the depth buffer is 1.0.
    glClearDepthf(1.0f);

    // The implicit clear value for the stencil buffer is 0.
    glClearStencil(0);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // Restore the clear values.
    glClearColor(current_clear_color[0], current_clear_color[1], current_clear_color[2], current_clear_color[3]);
    glClearDepthf(current_clear_depth);
    glClearStencil(current_clear_stencil);
#endif
}

void OpenGLContext::allocate_painting_surface_if_needed()
{
#ifdef AK_OS_MACOS
    if (m_painting_surface)
        return;

    VERIFY(!m_size.is_empty());

    auto iosurface = Core::IOSurfaceHandle::create(m_size.width(), m_size.height());
    m_painting_surface = Gfx::PaintingSurface::wrap_iosurface(iosurface, m_skia_backend_context, Gfx::PaintingSurface::Origin::BottomLeft);

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

    // This extension is not enabled by default in WebGL compatibility mode, so we need to request it.
    glRequestExtensionANGLE("GL_ANGLE_texture_rectangle");

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

u32 OpenGLContext::default_renderbuffer() const
{
#ifdef AK_OS_MACOS
    return m_impl->depth_buffer;
#else
    return 0;
#endif
}

u32 OpenGLContext::default_framebuffer() const
{
#ifdef AK_OS_MACOS
    return m_impl->framebuffer;
#else
    return 0;
#endif
}

struct Extension {
    String webgl_extension_name;
    Vector<StringView> required_angle_extensions;
    Optional<u32> only_for_webgl_version { OptionalNone {} };
};

Vector<Extension> s_available_webgl_extensions {
    // Khronos ratified WebGL Extensions
    { "ANGLE_instanced_arrays"_string, { "GL_ANGLE_instanced_arrays"sv }, 1 },
    { "EXT_blend_minmax"_string, { "GL_EXT_blend_minmax"sv }, 1 },
    { "EXT_frag_depth"_string, { "GL_EXT_frag_depth"sv }, 1 },
    { "EXT_shader_texture_lod"_string, { "GL_EXT_shader_texture_lod"sv }, 1 },
    { "EXT_texture_filter_anisotropic"_string, { "GL_EXT_texture_filter_anisotropic"sv } },
    { "OES_element_index_uint"_string, { "GL_OES_element_index_uint"sv }, 1 },
    { "OES_standard_derivatives"_string, { "GL_OES_standard_derivatives"sv }, 1 },
    { "OES_texture_float"_string, { "GL_OES_texture_float"sv }, 1 },
    { "OES_texture_float_linear"_string, { "GL_OES_texture_float_linear"sv } },
    { "OES_texture_half_float"_string, { "GL_OES_texture_half_float"sv }, 1 },
    { "OES_texture_half_float_linear"_string, { "GL_OES_texture_half_float_linear"sv }, 1 },
    { "OES_vertex_array_object"_string, { "GL_OES_vertex_array_object"sv }, 1 },
    { "WEBGL_compressed_texture_s3tc"_string, { "GL_EXT_texture_compression_dxt1"sv, "GL_ANGLE_texture_compression_dxt3"sv, "GL_ANGLE_texture_compression_dxt5"sv } },
    { "WEBGL_debug_renderer_info"_string, {} },
    { "WEBGL_debug_shaders"_string, {} },
    { "WEBGL_depth_texture"_string, { "GL_ANGLE_depth_texture"sv }, 1 },
    { "WEBGL_draw_buffers"_string, { "GL_EXT_draw_buffers"sv }, 1 },
    { "WEBGL_lose_context"_string, {} },

    // Community approved WebGL Extensions
    { "EXT_clip_control"_string, { "GL_EXT_clip_control"sv } },
    { "EXT_color_buffer_float"_string, { "GL_EXT_color_buffer_float"sv }, 2 },
    { "EXT_color_buffer_half_float"_string, { "GL_EXT_color_buffer_half_float"sv } },
    { "EXT_conservative_depth"_string, { "GL_EXT_conservative_depth"sv }, 2 },
    { "EXT_depth_clamp"_string, { "GL_EXT_depth_clamp"sv } },
    { "EXT_disjoint_timer_query"_string, { "GL_EXT_disjoint_timer_query"sv }, 1 },
    { "EXT_disjoint_timer_query_webgl2"_string, { "GL_EXT_disjoint_timer_query"sv }, 2 },
    { "EXT_float_blend"_string, { "GL_EXT_float_blend"sv } },
    { "EXT_polygon_offset_clamp"_string, { "GL_EXT_polygon_offset_clamp"sv } },
    { "EXT_render_snorm"_string, { "GL_EXT_render_snorm"sv }, 2 },
    { "EXT_sRGB"_string, { "GL_EXT_sRGB"sv }, 1 },
    { "EXT_texture_compression_bptc"_string, { "GL_EXT_texture_compression_bptc"sv } },
    { "EXT_texture_compression_rgtc"_string, { "GL_EXT_texture_compression_rgtc"sv } },
    { "EXT_texture_mirror_clamp_to_edge"_string, { "GL_EXT_texture_mirror_clamp_to_edge"sv } },
    { "EXT_texture_norm16"_string, { "GL_EXT_texture_norm16"sv }, 2 },
    { "KHR_parallel_shader_compile"_string, { "GL_KHR_parallel_shader_compile"sv } },
    { "NV_shader_noperspective_interpolation"_string, { "GL_NV_shader_noperspective_interpolation"sv }, 2 },
    { "OES_draw_buffers_indexed"_string, { "GL_OES_draw_buffers_indexed"sv } },
    { "OES_fbo_render_mipmap"_string, { "GL_OES_fbo_render_mipmap"sv }, 1 },
    { "OES_sample_variables"_string, { "GL_OES_sample_variables"sv }, 2 },
    { "OES_shader_multisample_interpolation"_string, { "GL_OES_shader_multisample_interpolation"sv }, 2 },
    { "OVR_multiview2"_string, { "GL_OVR_multiview2"sv }, 2 },
    { "WEBGL_blend_func_extended"_string, { "GL_EXT_blend_func_extended"sv } },
    { "WEBGL_clip_cull_distance"_string, { "GL_EXT_clip_cull_distance"sv }, 2 },
    { "WEBGL_color_buffer_float"_string, { "EXT_color_buffer_half_float"sv, "OES_texture_float"sv }, 1 },
    { "WEBGL_compressed_texture_astc"_string, { "KHR_texture_compression_astc_hdr"sv, "KHR_texture_compression_astc_ldr"sv } },
    { "WEBGL_compressed_texture_etc"_string, { "GL_ANGLE_compressed_texture_etc"sv } },
    { "WEBGL_compressed_texture_etc1"_string, { "GL_OES_compressed_ETC1_RGB8_texture"sv } },
    { "WEBGL_compressed_texture_pvrtc"_string, { "GL_IMG_texture_compression_pvrtc"sv } },
    { "WEBGL_compressed_texture_s3tc_srgb"_string, { "GL_EXT_texture_compression_s3tc_srgb"sv } },
    { "WEBGL_multi_draw"_string, { "GL_ANGLE_multi_draw"sv } },
    { "WEBGL_polygon_mode"_string, { "GL_ANGLE_polygon_mode"sv } },
    { "WEBGL_provoking_vertex"_string, { "GL_ANGLE_provoking_vertex"sv }, 2 },
    { "WEBGL_render_shared_exponent"_string, { "GL_QCOM_render_shared_exponent"sv }, 2 },
    { "WEBGL_stencil_texturing"_string, { "GL_ANGLE_stencil_texturing"sv }, 2 },
};

Vector<String> OpenGLContext::get_supported_extensions()
{
#ifdef AK_OS_MACOS
    if (m_requestable_extensions.has_value())
        return m_requestable_extensions.value();

    make_current();

    auto const* requestable_extensions_string = reinterpret_cast<char const*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE));
    StringView requestable_extensions_view(requestable_extensions_string, strlen(requestable_extensions_string));
    auto requestable_extensions = requestable_extensions_view.split_view(' ');

    Vector<String> extensions;
    for (auto const& available_extension : s_available_webgl_extensions) {
        // FIXME: Check WebGL version.
        bool supported = true;

        for (auto const& required_extension : available_extension.required_angle_extensions) {
            auto maybe_required_extension = requestable_extensions.find_if([&](StringView requestable_extension) {
                return required_extension == requestable_extension;
            });

            if (maybe_required_extension == requestable_extensions.end()) {
                supported = false;
                break;
            }
        }

        if (supported)
            extensions.append(available_extension.webgl_extension_name);
    }

    // We must cache this, because once extensions have been requested, they're no longer requestable extensions and would
    // not appear in this list. However, we must always report every supported extension, regardless of what has already
    // been requested.
    m_requestable_extensions = extensions;
    return extensions;
#else
    return {};
#endif
}

void OpenGLContext::request_extension(char const* extension_name)
{
#ifdef AK_OS_MACOS
    make_current();
    glRequestExtensionANGLE(extension_name);
#else
    (void)extension_name;
#endif
}

}
