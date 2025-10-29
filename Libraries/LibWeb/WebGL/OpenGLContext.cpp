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

#include <EGL/egl.h>
#include <EGL/eglext.h>
#define EGL_EGLEXT_PROTOTYPES 1
extern "C" {
#include <EGL/eglext_angle.h>
}
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>

// Enable WebGL if we're on MacOS and can use Metal or if we can use shareable Vulkan images
#if defined(AK_OS_MACOS) || defined(USE_VULKAN_IMAGES)
#    define ENABLE_WEBGL 1
#endif

namespace Web::WebGL {

struct OpenGLContext::Impl {
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLConfig config { EGL_NO_CONFIG_KHR };
    EGLContext context { EGL_NO_CONTEXT };
    EGLSurface surface { EGL_NO_SURFACE };

    GLuint framebuffer { 0 };
    GLuint color_buffer { 0 };
    GLuint depth_buffer { 0 };
    EGLint texture_target { 0 };

#ifdef USE_VULKAN_IMAGES
    EGLImage egl_image { EGL_NO_IMAGE };
    struct {
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_dma_buf_formats { nullptr };
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dma_buf_modifiers { nullptr };
    } ext_procs;
#endif
};

OpenGLContext::OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl, WebGLVersion webgl_version)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(impl))
    , m_webgl_version(webgl_version)
{
}

OpenGLContext::~OpenGLContext()
{
#ifdef ENABLE_WEBGL
    free_surface_resources();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_impl->display, m_impl->context);
#endif
}

void OpenGLContext::free_surface_resources()
{
#ifdef ENABLE_WEBGL
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    if (m_impl->framebuffer) {
        glDeleteFramebuffers(1, &m_impl->framebuffer);
        m_impl->framebuffer = 0;
    }

    if (m_impl->color_buffer) {
        glDeleteTextures(1, &m_impl->color_buffer);
        m_impl->color_buffer = 0;
    }

    if (m_impl->depth_buffer) {
        glDeleteRenderbuffers(1, &m_impl->depth_buffer);
        m_impl->depth_buffer = 0;
    }

#    ifdef USE_VULKAN_IMAGES
    if (m_impl->egl_image != EGL_NO_IMAGE) {
        eglDestroyImage(m_impl->display, m_impl->egl_image);
        m_impl->egl_image = EGL_NO_IMAGE;
    }
#    endif

    if (m_impl->surface != EGL_NO_SURFACE) {
#    ifdef AK_OS_MACOS
        eglReleaseTexImage(m_impl->display, m_impl->surface, EGL_BACK_BUFFER);
#    endif
        eglDestroySurface(m_impl->display, m_impl->surface);
        m_impl->surface = EGL_NO_SURFACE;
    }
#endif
}

#ifdef ENABLE_WEBGL
static EGLConfig get_egl_config(EGLDisplay display)
{
    EGLint const config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
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
    return number_of_configs > 0 ? configs[0] : EGL_NO_CONFIG_KHR;
}
#endif

OwnPtr<OpenGLContext> OpenGLContext::create(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, WebGLVersion webgl_version)
{
#ifdef ENABLE_WEBGL
    EGLAttrib display_attributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
#    if defined(AK_OS_MACOS)
        EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
#    elif defined(USE_VULKAN_IMAGES)
        EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE,
        EGL_PLATFORM_SURFACELESS_MESA,
#    endif
        EGL_NONE,
    };

    auto display = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY), display_attributes);
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
    if (config == EGL_NO_CONFIG_KHR) {
        dbgln("Failed to find EGLConfig");
        return {};
    }

    EGLint texture_target;
#    if defined(AK_OS_MACOS)
    eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &texture_target);
    VERIFY(texture_target == EGL_TEXTURE_RECTANGLE_ANGLE || texture_target == EGL_TEXTURE_2D);
#    elif defined(USE_VULKAN_IMAGES)
    texture_target = EGL_TEXTURE_2D;
#    endif

    EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION,
        webgl_version == WebGLVersion::WebGL1 ? 2 : 3,
        EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE,
        EGL_TRUE,
        EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE,
        EGL_TRUE,
        EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE,
        EGL_FALSE,
#    ifdef USE_VULKAN_IMAGES
        // we need GL_OES_EGL_image
        EGL_EXTENSIONS_ENABLED_ANGLE,
        EGL_TRUE,
#    endif
        EGL_NONE,
        EGL_NONE,
    };
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        dbgln("Failed to create EGL context");
        return {};
    }

#    ifdef USE_VULKAN_IMAGES
    auto pfn_egl_query_dma_buf_formats_ext = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    if (!pfn_egl_query_dma_buf_formats_ext) {
        dbgln("eglQueryDmaBufFormatsEXT unavailable");
        return {};
    }

    auto pfn_egl_query_dma_buf_modifiers_ext = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!pfn_egl_query_dma_buf_modifiers_ext) {
        dbgln("eglQueryDmaBufModifiersEXT unavailable");
        return {};
    }
#    endif

    return make<OpenGLContext>(skia_backend_context, Impl {
                                                         .display = display,
                                                         .config = config,
                                                         .context = context,
                                                         .texture_target = texture_target,
#    ifdef USE_VULKAN_IMAGES
                                                         .ext_procs = {
                                                             .query_dma_buf_formats = pfn_egl_query_dma_buf_formats_ext,
                                                             .query_dma_buf_modifiers = pfn_egl_query_dma_buf_modifiers_ext,
                                                         },
#    endif
                                                     },
        webgl_version);
#else
    (void)skia_backend_context;
    (void)webgl_version;
    return {};
#endif
}

void OpenGLContext::notify_content_will_change()
{
#ifdef ENABLE_WEBGL
    m_painting_surface->notify_content_will_change();
#endif
}

void OpenGLContext::clear_buffer_to_default_values()
{
#ifdef ENABLE_WEBGL
    GLint original_framebuffer;
    GLint original_renderbuffer;
    GLenum framebuffer_target = GL_FRAMEBUFFER;
    GLenum framebuffer_binding = GL_FRAMEBUFFER_BINDING;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        framebuffer_target = GL_DRAW_FRAMEBUFFER;
        framebuffer_binding = GL_DRAW_FRAMEBUFFER_BINDING;
    }
    glGetIntegerv(framebuffer_binding, &original_framebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &original_renderbuffer);

    glBindFramebuffer(framebuffer_target, default_framebuffer());
    glBindRenderbuffer(GL_RENDERBUFFER, default_renderbuffer());

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

    glBindFramebuffer(framebuffer_target, original_framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, original_renderbuffer);
#endif
}

#ifdef AK_OS_MACOS
void OpenGLContext::allocate_iosurface_painting_surface()
{
    auto iosurface = Core::IOSurfaceHandle::create(m_size.width(), m_size.height());
    m_painting_surface = Gfx::PaintingSurface::create_from_iosurface(move(iosurface), m_skia_backend_context, Gfx::PaintingSurface::Origin::BottomLeft);

    EGLint const surface_attributes[] = {
        EGL_WIDTH,
        m_size.width(),
        EGL_HEIGHT,
        m_size.height(),
        EGL_IOSURFACE_PLANE_ANGLE,
        0,
        EGL_TEXTURE_TARGET,
        m_impl->texture_target,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,
        GL_BGRA_EXT,
        EGL_TEXTURE_FORMAT,
        EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE,
        GL_UNSIGNED_BYTE,
        EGL_NONE,
        EGL_NONE,
    };
    m_impl->surface = eglCreatePbufferFromClientBuffer(m_impl->display, EGL_IOSURFACE_ANGLE, iosurface.core_foundation_pointer(), m_impl->config, surface_attributes);

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D, m_impl->color_buffer);
    auto result = eglBindTexImage(m_impl->display, m_impl->surface, EGL_BACK_BUFFER);
    VERIFY(result == EGL_TRUE);
}
#endif

#ifdef USE_VULKAN_IMAGES
void OpenGLContext::allocate_vkimage_painting_surface()
{
    VkFormat vulkan_format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t drm_format = Gfx::vk_format_to_drm_format(vulkan_format);

    // Ensure that our format is supported by the implementation.
    // FIXME: try other formats if not?
    EGLint num_formats = 0;
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, 0, nullptr, &num_formats);
    Vector<EGLint> egl_formats;
    egl_formats.resize(num_formats);
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, num_formats, egl_formats.data(), &num_formats);
    VERIFY(egl_formats.find(drm_format) != egl_formats.end());

    EGLint num_modifiers = 0;
    m_impl->ext_procs.query_dma_buf_modifiers(m_impl->display, drm_format, 0, nullptr, nullptr, &num_modifiers);
    Vector<uint64_t> egl_modifiers;
    egl_modifiers.resize(num_modifiers);
    Vector<EGLBoolean> external_only;
    external_only.resize(num_modifiers);
    m_impl->ext_procs.query_dma_buf_modifiers(m_impl->display, drm_format, num_modifiers, egl_modifiers.data(), external_only.data(), &num_modifiers);
    Vector<uint64_t> renderable_modifiers;
    for (int i = 0; i < num_modifiers; ++i) {
        if (!external_only[i]) {
            renderable_modifiers.append(egl_modifiers[i]);
        }
    }

    auto vulkan_image = MUST(Gfx::create_shared_vulkan_image(m_skia_backend_context->vulkan_context(), m_size.width(), m_size.height(), vulkan_format, renderable_modifiers.size(), renderable_modifiers.data()));
    m_painting_surface = Gfx::PaintingSurface::create_from_vkimage(m_skia_backend_context, vulkan_image, Gfx::PaintingSurface::Origin::BottomLeft);

    EGLAttrib attribs[] = {
        EGL_WIDTH,
        m_size.width(),
        EGL_HEIGHT,
        m_size.height(),
        EGL_LINUX_DRM_FOURCC_EXT,
        drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        vulkan_image->get_dma_buf_fd(), // EGL takes ownership of the fd
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<uint32_t>(vulkan_image->info.row_pitch),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        static_cast<uint32_t>(vulkan_image->info.modifier & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        static_cast<uint32_t>(vulkan_image->info.modifier >> 32),
        EGL_NONE,
    };
    m_impl->egl_image = eglCreateImage(m_impl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    VERIFY(m_impl->egl_image != EGL_NO_IMAGE);

    m_impl->surface = EGL_NO_SURFACE;
    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(GL_TEXTURE_2D, m_impl->color_buffer);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_impl->egl_image);

    glViewport(0, 0, m_size.width(), m_size.height());
}
#endif

void OpenGLContext::allocate_painting_surface_if_needed()
{
#ifdef ENABLE_WEBGL
    if (m_painting_surface)
        return;

    free_surface_resources();

    VERIFY(!m_size.is_empty());

#    if defined(AK_OS_MACOS)
    allocate_iosurface_painting_surface();
#    elif defined(USE_VULKAN_IMAGES)
    allocate_vkimage_painting_surface();
#    endif
    VERIFY(m_painting_surface);
    VERIFY(eglGetCurrentContext() == m_impl->context);

    glGenFramebuffers(1, &m_impl->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D, m_impl->color_buffer, 0);

    // NOTE: ANGLE doesn't allocate depth buffer for us, so we need to do it manually
    // FIXME: Depth buffer only needs to be allocated if it's configured in WebGL context attributes
    glGenRenderbuffers(1, &m_impl->depth_buffer);
    glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, m_size.width(), m_size.height());
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
    VERIFY(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
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
#ifdef ENABLE_WEBGL
    allocate_painting_surface_if_needed();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);
#endif
}

void OpenGLContext::present(bool preserve_drawing_buffer)
{
#ifdef ENABLE_WEBGL
    make_current();

    // "Before the drawing buffer is presented for compositing the implementation shall ensure that all rendering operations have been flushed to the drawing buffer."
    // With Metal, glFlush flushes the command buffer, but without waiting for it to be scheduled or completed.
    // eglWaitUntilWorkScheduledANGLE flushes the command buffer, and waits until it has been scheduled, hence the name.
    // eglWaitUntilWorkScheduledANGLE only has an effect on CGL and Metal backends, so we only use it on macOS.
#    if defined(AK_OS_MACOS)
    eglWaitUntilWorkScheduledANGLE(m_impl->display);
#    elif defined(USE_VULKAN_IMAGES)
    // FIXME: CPU sync for now, but it would be better to export a fence and have Skia wait for it before reading from the surface
    glFinish();
#    endif

    // "By default, after compositing the contents of the drawing buffer shall be cleared to their default values, as shown in the table above.
    // This default behavior can be changed by setting the preserveDrawingBuffer attribute of the WebGLContextAttributes object.
    // If this flag is true, the contents of the drawing buffer shall be preserved until the author either clears or overwrites them."
    if (!preserve_drawing_buffer) {
        // FIXME: we're assuming the clear operation won't actually be submitted to the GPU
        clear_buffer_to_default_values();
    }
#else
    (void)preserve_drawing_buffer;
#endif
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::surface()
{
    return m_painting_surface;
}

u32 OpenGLContext::default_renderbuffer() const
{
    return m_impl->depth_buffer;
}

u32 OpenGLContext::default_framebuffer() const
{
    return m_impl->framebuffer;
}

struct Extension {
    String webgl_extension_name;
    Vector<StringView> required_angle_extensions;
    Optional<OpenGLContext::WebGLVersion> only_for_webgl_version { OptionalNone {} };
};

Vector<Extension> s_available_webgl_extensions {
    // Khronos ratified WebGL Extensions
    { "ANGLE_instanced_arrays"_string, { "GL_ANGLE_instanced_arrays"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "EXT_blend_minmax"_string, { "GL_EXT_blend_minmax"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "EXT_frag_depth"_string, { "GL_EXT_frag_depth"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "EXT_shader_texture_lod"_string, { "GL_EXT_shader_texture_lod"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "EXT_texture_filter_anisotropic"_string, { "GL_EXT_texture_filter_anisotropic"sv } },
    { "OES_element_index_uint"_string, { "GL_OES_element_index_uint"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "OES_standard_derivatives"_string, { "GL_OES_standard_derivatives"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "OES_texture_float"_string, { "GL_OES_texture_float"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "OES_texture_float_linear"_string, { "GL_OES_texture_float_linear"sv } },
    { "OES_texture_half_float"_string, { "GL_OES_texture_half_float"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "OES_texture_half_float_linear"_string, { "GL_OES_texture_half_float_linear"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "OES_vertex_array_object"_string, { "GL_OES_vertex_array_object"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "WEBGL_compressed_texture_s3tc"_string, { "GL_EXT_texture_compression_dxt1"sv, "GL_ANGLE_texture_compression_dxt3"sv, "GL_ANGLE_texture_compression_dxt5"sv } },
    { "WEBGL_debug_renderer_info"_string, {} },
    { "WEBGL_debug_shaders"_string, {} },
    { "WEBGL_depth_texture"_string, { "GL_ANGLE_depth_texture"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "WEBGL_draw_buffers"_string, { "GL_EXT_draw_buffers"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "WEBGL_lose_context"_string, {} },

    // Community approved WebGL Extensions
    { "EXT_clip_control"_string, { "GL_EXT_clip_control"sv } },
    { "EXT_color_buffer_float"_string, { "GL_EXT_color_buffer_float"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "EXT_color_buffer_half_float"_string, { "GL_EXT_color_buffer_half_float"sv } },
    { "EXT_conservative_depth"_string, { "GL_EXT_conservative_depth"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "EXT_depth_clamp"_string, { "GL_EXT_depth_clamp"sv } },
    { "EXT_disjoint_timer_query"_string, { "GL_EXT_disjoint_timer_query"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "EXT_disjoint_timer_query_webgl2"_string, { "GL_EXT_disjoint_timer_query"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "EXT_float_blend"_string, { "GL_EXT_float_blend"sv } },
    { "EXT_polygon_offset_clamp"_string, { "GL_EXT_polygon_offset_clamp"sv } },
    { "EXT_render_snorm"_string, { "GL_EXT_render_snorm"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "EXT_sRGB"_string, { "GL_EXT_sRGB"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "EXT_texture_compression_bptc"_string, { "GL_EXT_texture_compression_bptc"sv } },
    { "EXT_texture_compression_rgtc"_string, { "GL_EXT_texture_compression_rgtc"sv } },
    { "EXT_texture_mirror_clamp_to_edge"_string, { "GL_EXT_texture_mirror_clamp_to_edge"sv } },
    { "EXT_texture_norm16"_string, { "GL_EXT_texture_norm16"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "KHR_parallel_shader_compile"_string, { "GL_KHR_parallel_shader_compile"sv } },
    { "NV_shader_noperspective_interpolation"_string, { "GL_NV_shader_noperspective_interpolation"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "OES_draw_buffers_indexed"_string, { "GL_OES_draw_buffers_indexed"sv } },
    { "OES_fbo_render_mipmap"_string, { "GL_OES_fbo_render_mipmap"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "OES_sample_variables"_string, { "GL_OES_sample_variables"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "OES_shader_multisample_interpolation"_string, { "GL_OES_shader_multisample_interpolation"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "OVR_multiview2"_string, { "GL_OVR_multiview2"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "WEBGL_blend_func_extended"_string, { "GL_EXT_blend_func_extended"sv } },
    { "WEBGL_clip_cull_distance"_string, { "GL_EXT_clip_cull_distance"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "WEBGL_color_buffer_float"_string, { "EXT_color_buffer_half_float"sv, "OES_texture_float"sv }, OpenGLContext::WebGLVersion::WebGL1 },
    { "WEBGL_compressed_texture_astc"_string, { "KHR_texture_compression_astc_hdr"sv, "KHR_texture_compression_astc_ldr"sv } },
    { "WEBGL_compressed_texture_etc"_string, { "GL_ANGLE_compressed_texture_etc"sv } },
    { "WEBGL_compressed_texture_etc1"_string, { "GL_OES_compressed_ETC1_RGB8_texture"sv } },
    { "WEBGL_compressed_texture_pvrtc"_string, { "GL_IMG_texture_compression_pvrtc"sv } },
    { "WEBGL_compressed_texture_s3tc_srgb"_string, { "GL_EXT_texture_compression_s3tc_srgb"sv } },
    { "WEBGL_multi_draw"_string, { "GL_ANGLE_multi_draw"sv } },
    { "WEBGL_polygon_mode"_string, { "GL_ANGLE_polygon_mode"sv } },
    { "WEBGL_provoking_vertex"_string, { "GL_ANGLE_provoking_vertex"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "WEBGL_render_shared_exponent"_string, { "GL_QCOM_render_shared_exponent"sv }, OpenGLContext::WebGLVersion::WebGL2 },
    { "WEBGL_stencil_texturing"_string, { "GL_ANGLE_stencil_texturing"sv }, OpenGLContext::WebGLVersion::WebGL2 },
};

Vector<String> OpenGLContext::get_supported_extensions()
{
#ifdef ENABLE_WEBGL
    if (m_requestable_extensions.has_value())
        return m_requestable_extensions.value();

    make_current();

    auto const* requestable_extensions_string = reinterpret_cast<char const*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE));
    StringView requestable_extensions_view(requestable_extensions_string, strlen(requestable_extensions_string));
    auto requestable_extensions = requestable_extensions_view.split_view(' ');

    Vector<String> extensions;
    for (auto const& available_extension : s_available_webgl_extensions) {
        bool supported = !available_extension.only_for_webgl_version.has_value()
            || m_webgl_version == available_extension.only_for_webgl_version;

        if (supported) {
            for (auto const& required_extension : available_extension.required_angle_extensions) {
                auto maybe_required_extension = requestable_extensions.find_if([&](StringView requestable_extension) {
                    return required_extension == requestable_extension;
                });

                if (maybe_required_extension == requestable_extensions.end()) {
                    supported = false;
                    break;
                }
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
    (void)m_webgl_version;
    return {};
#endif
}

void OpenGLContext::request_extension(char const* extension_name)
{
#ifdef ENABLE_WEBGL
    make_current();
    glRequestExtensionANGLE(extension_name);
#else
    (void)extension_name;
#endif
}

}
