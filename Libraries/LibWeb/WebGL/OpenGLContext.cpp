/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define EGL_EGLEXT_PROTOTYPES

#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/WebGL/OpenGLContext.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <EGL/eglext_angle.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <GLES2/gl2ext_angle.h>

#ifdef USE_VULKAN
#    include <drm/drm_fourcc.h>
#endif

namespace Web::WebGL {

struct OpenGLContext::Impl {
    EGLDisplay display { nullptr };
    EGLConfig config { nullptr };
    EGLContext context { nullptr };
    EGLSurface surface { nullptr };

    GLuint framebuffer { 0 };
    GLuint depth_buffer { 0 };
};

OpenGLContext::OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(impl))
{
}

OpenGLContext::~OpenGLContext()
{
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    glDeleteFramebuffers(1, &m_impl->framebuffer);
    glDeleteRenderbuffers(1, &m_impl->depth_buffer);
    eglDestroyContext(m_impl->display, m_impl->context);
    eglDestroySurface(m_impl->display, m_impl->surface);
}

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

OwnPtr<OpenGLContext> OpenGLContext::create(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context)
{
    EGLDisplay display = EGL_NO_DISPLAY;

    // FIXME: Use egl extension query and command line flags to get the platform type
    EGLAttrib angle_platform_type = EGL_PLATFORM_ANGLE_TYPE_DEFAULT_ANGLE;
#if defined(USE_VULKAN)
    angle_platform_type = EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE;
#elif defined(AK_OS_MACOS) || defined(AK_OS_IOS)
    angle_platform_type = EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE;
#endif

    EGLAttrib attributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
        angle_platform_type,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE,
        EGL_PLATFORM_ANGLE_DEVICE_TYPE_HARDWARE_ANGLE,
        EGL_NONE,
    };
    display = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, EGL_DEFAULT_DISPLAY, attributes);

    if (display == EGL_NO_DISPLAY) {
        dbgln("Unable to get display for preferred angle platform type {:x}, falling back to default display: {:x}", angle_platform_type, eglGetError());
        display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
        if (display == EGL_NO_DISPLAY) {
            dbgln("Failed to get EGL display: {:x}", eglGetError());
            return {};
        }
    }

    char const* extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    dbgln("EGL extensions: {}", extensions);

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

    return make<OpenGLContext>(move(skia_backend_context), Impl { .display = display, .config = config, .context = context });
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
    if (m_painting_surface)
        return;

    VERIFY(!m_size.is_empty());

#ifdef AK_OS_MACOS
    auto iosurface = Core::IOSurfaceHandle::create(m_size.width(), m_size.height());
    m_painting_surface = Gfx::PaintingSurface::wrap_iosurface(iosurface, m_skia_backend_context, Gfx::PaintingSurface::Origin::BottomLeft);
#else
    VkExtent2D const extent { static_cast<uint32_t>(m_size.width()), static_cast<uint32_t>(m_size.height()) };
    auto vulkan_image_or_error = Gfx::Vulkan::create_image(m_skia_backend_context->vulkan_context(), extent, VK_FORMAT_B8G8R8A8_UNORM);
    if (vulkan_image_or_error.is_error()) {
        dbgln("Failed to create Vulkan image: {}", vulkan_image_or_error.error());
        VERIFY_NOT_REACHED();
    }
    auto vulkan_image = vulkan_image_or_error.release_value();
#endif

    auto width = m_size.width();
    auto height = m_size.height();

    auto* display = m_impl->display;
    auto* config = m_impl->config;

#ifdef AK_OS_MACOS
    EGLint target = 0;
    eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &target);
#endif

    EGLint const surface_attributes[] = {
        EGL_WIDTH,
        width,
        EGL_HEIGHT,
        height,
#ifdef AK_OS_MACOS
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
#endif
        EGL_NONE,
    };

#ifdef AK_OS_MACOS
    m_impl->surface = eglCreatePbufferFromClientBuffer(display, EGL_IOSURFACE_ANGLE, iosurface.core_foundation_pointer(), config, surface_attributes);
#else
    int dma_buf_fd = vulkan_image.exported_fd;
    dbgln("Exported dma_buf fd: {}", dma_buf_fd);

    ScopeGuard close_dma_buf_fd = [&] { ::close(dma_buf_fd); };

    EGLint image_attribs[] = {
        EGL_WIDTH, width,
        EGL_HEIGHT, height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, dma_buf_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, width * 4,
        EGL_NONE
    };
    EGLImageKHR egl_image = eglCreateImageKHR(m_impl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, image_attribs);
    VERIFY(egl_image != EGL_NO_IMAGE_KHR);

    m_impl->surface = eglCreatePbufferFromClientBuffer(m_impl->display, EGL_LINUX_DMA_BUF_EXT, egl_image, config, surface_attributes);

    m_painting_surface = Gfx::PaintingSurface::wrap_vkimage(vulkan_image, m_skia_backend_context, Gfx::PaintingSurface::Origin::BottomLeft);
#endif

    if (m_impl->surface == EGL_NO_SURFACE) {
        dbgln("Failed to create EGL surface: {:x}", eglGetError());
        VERIFY_NOT_REACHED();
    }

    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);

    EGLint texture_target = 0;
#ifdef AK_OS_MACOS
    eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &texture_target);
    VERIFY(texture_target == EGL_TEXTURE_RECTANGLE_ANGLE);
#else
    texture_target = GL_TEXTURE_2D;
#endif

    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(texture_target, texture);

    auto result = eglBindTexImage(display, m_impl->surface, EGL_BACK_BUFFER);
    if (result == EGL_FALSE) {
        dbgln("Failed to bind texture image to EGL surface: {:x}", eglGetError());
        VERIFY_NOT_REACHED();
    }

    glGenFramebuffers(1, &m_impl->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture_target, texture, 0);

    // NOTE: ANGLE doesn't allocate depth buffer for us, so we need to do it manually
    // FIXME: Depth buffer only needs to be allocated if it's configured in WebGL context attributes
    glGenRenderbuffers(1, &m_impl->depth_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
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
    allocate_painting_surface_if_needed();
    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::surface()
{
    return m_painting_surface;
}

Vector<StringView> s_available_webgl_extensions {
    // Khronos ratified WebGL Extensions
    "ANGLE_instanced_arrays"sv,
    "EXT_blend_minmax"sv,
    "EXT_frag_depth"sv,
    "EXT_shader_texture_lod"sv,
    "EXT_texture_filter_anisotropic"sv,
    "OES_element_index_uint"sv,
    "OES_standard_derivatives"sv,
    "OES_texture_float"sv,
    "OES_texture_float_linear"sv,
    "OES_texture_half_float"sv,
    "OES_texture_half_float_linear"sv,
    "OES_vertex_array_object"sv,
    "WEBGL_compressed_texture_s3tc"sv,
    "WEBGL_debug_renderer_info"sv,
    "WEBGL_debug_shaders"sv,
    "WEBGL_depth_texture"sv,
    "WEBGL_draw_buffers"sv,
    "WEBGL_lose_context"sv,

    // Community approved WebGL Extensions
    "EXT_clip_control"sv,
    "EXT_color_buffer_float"sv,
    "EXT_color_buffer_half_float"sv,
    "EXT_conservative_depth"sv,
    "EXT_depth_clamp"sv,
    "EXT_disjoint_timer_query"sv,
    "EXT_disjoint_timer_query_webgl2"sv,
    "EXT_float_blend"sv,
    "EXT_polygon_offset_clamp"sv,
    "EXT_render_snorm"sv,
    "EXT_sRGB"sv,
    "EXT_texture_compression_bptc"sv,
    "EXT_texture_compression_rgtc"sv,
    "EXT_texture_mirror_clamp_to_edge"sv,
    "EXT_texture_norm16"sv,
    "KHR_parallel_shader_compile"sv,
    "NV_shader_noperspective_interpolation"sv,
    "OES_draw_buffers_indexed"sv,
    "OES_fbo_render_mipmap"sv,
    "OES_sample_variables"sv,
    "OES_shader_multisample_interpolation"sv,
    "OVR_multiview2"sv,
    "WEBGL_blend_func_extended"sv,
    "WEBGL_clip_cull_distance"sv,
    "WEBGL_color_buffer_float"sv,
    "WEBGL_compressed_texture_astc"sv,
    "WEBGL_compressed_texture_etc"sv,
    "WEBGL_compressed_texture_etc1"sv,
    "WEBGL_compressed_texture_pvrtc"sv,
    "WEBGL_compressed_texture_s3tc_srgb"sv,
    "WEBGL_multi_draw"sv,
    "WEBGL_polygon_mode"sv,
    "WEBGL_provoking_vertex"sv,
    "WEBGL_render_shared_exponent"sv,
    "WEBGL_stencil_texturing"sv,
};

Vector<String> OpenGLContext::get_supported_extensions()
{
    make_current();

    auto const* extensions_string = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS));
    StringView extensions_view(extensions_string, strlen(extensions_string));

    Vector<String> extensions;
    for (auto const& extension : extensions_view.split_view(' ')) {
        auto extension_name_without_gl_prefix = extension.substring_view(3);
        // FIXME: WebGL 1 and WebGL 2 have different sets of available extensions, but for now we simply
        //        filter out everything that is not listed in https://registry.khronos.org/webgl/extensions/
        if (s_available_webgl_extensions.contains_slow(extension_name_without_gl_prefix))
            extensions.append(MUST(String::from_utf8(extension_name_without_gl_prefix)));
    }

    return extensions;
}

}
