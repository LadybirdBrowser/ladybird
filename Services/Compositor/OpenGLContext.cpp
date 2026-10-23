/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Define GL_GLEXT_PROTOTYPES before OpenGLContext.h pulls in the GL headers without it.
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>

// Define EGL_EGLEXT_PROTOTYPES before eglext.h pulls in the ANGLE extension headers without it.
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
extern "C" {
#include <EGL/eglext_angle.h>
}

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <Compositor/PaintingSurface.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif
#include <Compositor/OpenGLContext.h>
#include <Compositor/SkiaBackendContext.h>
#include <Compositor/SkiaUtils.h>

#include <core/SkCanvas.h>
#include <core/SkImage.h>

// Enable WebGL if we're on macOS and can use Metal, if Linux can use ANGLE's OpenGL backend for CPU-painting tests,
// if Windows can use ANGLE's Direct3D 11 backend, or if we can use shareable Vulkan images.
#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE) || defined(USE_VULKAN_DMABUF_IMAGES)
#    define ENABLE_WEBGL 1
#endif

namespace Compositor {

using namespace Compositing::WebGL;

struct OpenGLContext::ColorBuffer {
    RefPtr<Gfx::PaintingSurface> painting_surface;
#ifdef AK_OS_MACOS
    OwnPtr<Gfx::SharedImageBuffer> shared_image_buffer;
#endif
    EGLSurface surface { EGL_NO_SURFACE };
#ifdef USE_VULKAN_DMABUF_IMAGES
    EGLImage egl_image { EGL_NO_IMAGE };
#endif
    GLuint texture { 0 };

    bool is_allocated() const { return painting_surface; }
};

struct OpenGLContext::Impl {
    AK_ALLOC_WITH_KMALLOC;

    EGLDisplay display { EGL_NO_DISPLAY };
    EGLConfig config { EGL_NO_CONFIG_KHR };
    EGLContext context { EGL_NO_CONTEXT };

    GLuint framebuffer { 0 };
    GLuint depth_buffer { 0 };
    EGLint texture_target { 0 };
    bool uses_cpu_painting_surface { false };
    Optional<bool> supports_copy_texture {};

    // A shared drawing buffer is composited straight from GPU memory, so presenting moves the default framebuffer
    // to another color buffer instead of letting the page draw over the frame being composited. Besides the
    // presented buffer and the one being drawn, a third holds the frame presented before, which a raster submitted
    // before the latest present may still be reading on the GPU. The CPU painting surface is a copy made when
    // presenting, so it only uses the first.
    Array<ColorBuffer, 3> color_buffers {};
    // What Skia draws for an upload into one of the page's textures, at the size of the latest upload.
    ColorBuffer upload_color_buffer {};
    Gfx::IntSize upload_color_buffer_size {};
    size_t drawing_color_buffer_index { 0 };
    Optional<size_t> presented_color_buffer_index {};

#ifdef USE_VULKAN_DMABUF_IMAGES
    struct {
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_dma_buf_formats { nullptr };
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dma_buf_modifiers { nullptr };
    } ext_procs;
#endif
};

OpenGLContext::OpenGLContext(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl, WebGLVersion webgl_version, DrawingBufferOptions drawing_buffer_options)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(move(impl)))
    , m_webgl_version(webgl_version)
    , m_drawing_buffer_options(drawing_buffer_options)
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

    if (m_impl->depth_buffer) {
        glDeleteRenderbuffers(1, &m_impl->depth_buffer);
        m_impl->depth_buffer = 0;
    }

    for (auto& color_buffer : m_impl->color_buffers)
        free_color_buffer(color_buffer);
    free_color_buffer(m_impl->upload_color_buffer);
    m_impl->upload_color_buffer_size = {};
    m_impl->drawing_color_buffer_index = 0;
    m_impl->presented_color_buffer_index.clear();
#endif
}

void OpenGLContext::free_color_buffer([[maybe_unused]] ColorBuffer& color_buffer)
{
#ifdef ENABLE_WEBGL
    if (color_buffer.texture) {
        glDeleteTextures(1, &color_buffer.texture);
        color_buffer.texture = 0;
    }

#    ifdef USE_VULKAN_DMABUF_IMAGES
    if (color_buffer.egl_image != EGL_NO_IMAGE) {
        eglDestroyImage(m_impl->display, color_buffer.egl_image);
        color_buffer.egl_image = EGL_NO_IMAGE;
    }
#    endif

    if (color_buffer.surface != EGL_NO_SURFACE) {
#    ifdef AK_OS_MACOS
        eglReleaseTexImage(m_impl->display, color_buffer.surface, EGL_BACK_BUFFER);
#    endif
        eglDestroySurface(m_impl->display, color_buffer.surface);
        color_buffer.surface = EGL_NO_SURFACE;
    }

    // NB: The compositor may still hold the painting surface of the presented buffer; it keeps the shared image alive.
    color_buffer.painting_surface = nullptr;
#    ifdef AK_OS_MACOS
    color_buffer.shared_image_buffer = nullptr;
#    endif
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

OwnPtr<OpenGLContext> OpenGLContext::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, WebGLVersion webgl_version, [[maybe_unused]] DrawingBufferOptions drawing_buffer_options)
{
#ifdef ENABLE_WEBGL
#    if defined(AK_OS_MACOS) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID))
    bool use_cpu_painting_surface = !skia_backend_context;
#    elif defined(AK_OS_WINDOWS)
    // FIXME: Share the drawing buffer with Skia's Direct3D 12 device (via an NT shared handle opened on ANGLE's
    //        Direct3D 11 device) instead of reading it back to the CPU every frame.
    bool use_cpu_painting_surface = true;
#    else
    bool use_cpu_painting_surface = false;
#    endif

#    if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES)
    if (!use_cpu_painting_surface && !skia_backend_context)
        return {};
#    endif

#    if !defined(AK_OS_MACOS) && !defined(USE_VULKAN_DMABUF_IMAGES)
    if (!use_cpu_painting_surface)
        return {};
#    endif

    EGLAttrib display_attributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
#    if defined(AK_OS_MACOS)
        EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
#    elif defined(USE_VULKAN_DMABUF_IMAGES) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID))
        EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE,
        EGL_PLATFORM_SURFACELESS_MESA,
#    elif defined(AK_OS_WINDOWS)
        EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE,
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
    if (use_cpu_painting_surface) {
        texture_target = EGL_TEXTURE_2D;
    } else {
        eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &texture_target);
        VERIFY(texture_target == EGL_TEXTURE_RECTANGLE_ANGLE || texture_target == EGL_TEXTURE_2D);
    }
#    else
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
#    ifdef USE_VULKAN_DMABUF_IMAGES
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

#    ifdef USE_VULKAN_DMABUF_IMAGES
    PFNEGLQUERYDMABUFFORMATSEXTPROC pfn_egl_query_dma_buf_formats_ext = nullptr;
    PFNEGLQUERYDMABUFMODIFIERSEXTPROC pfn_egl_query_dma_buf_modifiers_ext = nullptr;

    if (!use_cpu_painting_surface) {
        pfn_egl_query_dma_buf_formats_ext = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
        if (!pfn_egl_query_dma_buf_formats_ext) {
            dbgln("eglQueryDmaBufFormatsEXT unavailable");
            return {};
        }

        pfn_egl_query_dma_buf_modifiers_ext = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
        if (!pfn_egl_query_dma_buf_modifiers_ext) {
            dbgln("eglQueryDmaBufModifiersEXT unavailable");
            return {};
        }
    }
#    endif

    return make<OpenGLContext>(skia_backend_context, Impl {
                                                         .display = display,
                                                         .config = config,
                                                         .context = context,
                                                         .texture_target = texture_target,
                                                         .uses_cpu_painting_surface = use_cpu_painting_surface,
#    ifdef USE_VULKAN_DMABUF_IMAGES
                                                         .ext_procs = {
                                                             .query_dma_buf_formats = pfn_egl_query_dma_buf_formats_ext,
                                                             .query_dma_buf_modifiers = pfn_egl_query_dma_buf_modifiers_ext,
                                                         },
#    endif
                                                     },
        webgl_version, drawing_buffer_options);
#else
    (void)skia_backend_context;
    (void)webgl_version;
    return {};
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

#ifdef ENABLE_WEBGL
u32 OpenGLContext::color_buffer_texture_target() const
{
    return m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D;
}

// Color buffers are also allocated when the canvas is resized and while presenting, between the page's commands, so
// setting one up must leave the page's texture binding alone.
template<typename Callback>
static void with_texture_bound(GLenum target, GLuint texture, Callback callback)
{
    GLint original_texture = 0;
    glGetIntegerv(target == GL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_BINDING_RECTANGLE_ANGLE : GL_TEXTURE_BINDING_2D, &original_texture);
    glBindTexture(target, texture);
    callback();
    glBindTexture(target, original_texture);
}
#endif

#ifdef AK_OS_MACOS
void OpenGLContext::allocate_iosurface_color_buffer(ColorBuffer& color_buffer, Gfx::IntSize size, Gfx::PaintingSurface::Origin origin)
{
    color_buffer.shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::create(size));
    color_buffer.painting_surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*color_buffer.shared_image_buffer, *m_skia_backend_context, origin);

    EGLint const surface_attributes[] = {
        EGL_WIDTH,
        size.width(),
        EGL_HEIGHT,
        size.height(),
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
    color_buffer.surface = eglCreatePbufferFromClientBuffer(m_impl->display, EGL_IOSURFACE_ANGLE, color_buffer.shared_image_buffer->iosurface_handle().core_foundation_pointer(), m_impl->config, surface_attributes);

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    glGenTextures(1, &color_buffer.texture);
    with_texture_bound(color_buffer_texture_target(), color_buffer.texture, [&] {
        auto result = eglBindTexImage(m_impl->display, color_buffer.surface, EGL_BACK_BUFFER);
        VERIFY(result == EGL_TRUE);
    });
}
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
bool OpenGLContext::allocate_vkimage_color_buffer(ColorBuffer& color_buffer, Gfx::IntSize size, Gfx::PaintingSurface::Origin origin)
{
    VERIFY(m_skia_backend_context);

    VkFormat vulkan_format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t drm_format = Gfx::vk_format_to_drm_format(vulkan_format);

    // Ensure that our format is supported by the implementation.
    // FIXME: try other formats if not?
    EGLint num_formats = 0;
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, 0, nullptr, &num_formats);
    Vector<EGLint> egl_formats;
    egl_formats.resize(num_formats);
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, num_formats, egl_formats.data(), &num_formats);
    if (egl_formats.find(drm_format) == egl_formats.end()) {
        dbgln("Compositor: DMABUF format {:#x} is not supported by this driver; falling back to a CPU painting surface", drm_format);
        return false;
    }

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

    auto vulkan_image_or_error = Gfx::create_shared_vulkan_image(m_skia_backend_context->vulkan_context(), size.width(), size.height(), vulkan_format, renderable_modifiers);
    if (vulkan_image_or_error.is_error()) {
        dbgln("Compositor: create_shared_vulkan_image failed ({}); falling back to a CPU painting surface", vulkan_image_or_error.error());
        return false;
    }
    auto vulkan_image = vulkan_image_or_error.release_value();
    color_buffer.painting_surface = Gfx::PaintingSurface::create_from_vkimage(*m_skia_backend_context, vulkan_image, origin);

    EGLAttrib attribs[] = {
        EGL_WIDTH,
        size.width(),
        EGL_HEIGHT,
        size.height(),
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
    color_buffer.egl_image = eglCreateImage(m_impl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    if (color_buffer.egl_image == EGL_NO_IMAGE) {
        dbgln("Compositor: eglCreateImage for the DMABUF failed; falling back to a CPU painting surface");
        free_color_buffer(color_buffer);
        return false;
    }

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    glGenTextures(1, &color_buffer.texture);
    with_texture_bound(GL_TEXTURE_2D, color_buffer.texture, [&] {
        glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, color_buffer.egl_image);
    });

    return true;
}
#endif

#if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
void OpenGLContext::allocate_cpu_painting_surface(ColorBuffer& color_buffer)
{
    color_buffer.painting_surface = Gfx::PaintingSurface::create_with_size(
        m_size,
        Gfx::BitmapFormat::BGRA8888,
        Gfx::AlphaType::Premultiplied);

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    glGenTextures(1, &color_buffer.texture);
    with_texture_bound(GL_TEXTURE_2D, color_buffer.texture, [&] {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        GLint original_pixel_unpack_buffer = 0;
        if (m_webgl_version == WebGLVersion::WebGL2) {
            glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &original_pixel_unpack_buffer);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        }
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, m_size.width(), m_size.height(), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        if (m_webgl_version == WebGLVersion::WebGL2)
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, original_pixel_unpack_buffer);
    });
}

void OpenGLContext::copy_default_framebuffer_to_cpu_painting_surface()
{
    VERIFY(m_impl->uses_cpu_painting_surface);
    auto& painting_surface = m_impl->color_buffers[m_impl->drawing_color_buffer_index].painting_surface;
    VERIFY(painting_surface);

    auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, m_size));

    GLenum framebuffer_target = GL_FRAMEBUFFER;
    GLenum framebuffer_binding = GL_FRAMEBUFFER_BINDING;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        framebuffer_target = GL_READ_FRAMEBUFFER;
        framebuffer_binding = GL_READ_FRAMEBUFFER_BINDING;
    }

    GLint original_framebuffer;
    glGetIntegerv(framebuffer_binding, &original_framebuffer);

    GLint original_read_buffer = GL_COLOR_ATTACHMENT0;
    if (m_webgl_version == WebGLVersion::WebGL2)
        glGetIntegerv(GL_READ_BUFFER, &original_read_buffer);

    glBindFramebuffer(framebuffer_target, default_framebuffer());
    if (m_webgl_version == WebGLVersion::WebGL2)
        glReadBuffer(GL_COLOR_ATTACHMENT0);

    GLint original_pack_alignment;
    glGetIntegerv(GL_PACK_ALIGNMENT, &original_pack_alignment);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);

    GLint original_pixel_pack_buffer = 0;
    GLint original_pack_row_length = 0;
    GLint original_pack_skip_pixels = 0;
    GLint original_pack_skip_rows = 0;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &original_pixel_pack_buffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

        glGetIntegerv(GL_PACK_ROW_LENGTH, &original_pack_row_length);
        glGetIntegerv(GL_PACK_SKIP_PIXELS, &original_pack_skip_pixels);
        glGetIntegerv(GL_PACK_SKIP_ROWS, &original_pack_skip_rows);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        glPixelStorei(GL_PACK_SKIP_PIXELS, 0);
        glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    }

    for (int source_y = 0; source_y < m_size.height(); ++source_y)
        glReadPixels(0, source_y, m_size.width(), 1, GL_RGBA, GL_UNSIGNED_BYTE, bitmap->scanline_u8(m_size.height() - source_y - 1));

    if (m_webgl_version == WebGLVersion::WebGL2) {
        glPixelStorei(GL_PACK_ROW_LENGTH, original_pack_row_length);
        glPixelStorei(GL_PACK_SKIP_PIXELS, original_pack_skip_pixels);
        glPixelStorei(GL_PACK_SKIP_ROWS, original_pack_skip_rows);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, original_pixel_pack_buffer);
    }
    glPixelStorei(GL_PACK_ALIGNMENT, original_pack_alignment);

    glBindFramebuffer(framebuffer_target, original_framebuffer);
    if (m_webgl_version == WebGLVersion::WebGL2)
        glReadBuffer(original_read_buffer);

    painting_surface->write_from_bitmap(*bitmap);
}
#endif

void OpenGLContext::allocate_painting_surface_if_needed()
{
#ifdef ENABLE_WEBGL
    if (!m_drawing_buffer_needs_allocation)
        return;
    m_drawing_buffer_needs_allocation = false;

    free_surface_resources();

    VERIFY(!m_size.is_empty());

    auto& color_buffer = m_impl->color_buffers[m_impl->drawing_color_buffer_index];
#    if defined(AK_OS_MACOS)
    if (m_impl->uses_cpu_painting_surface) {
        allocate_cpu_painting_surface(color_buffer);
    } else {
        allocate_iosurface_color_buffer(color_buffer, m_size, Gfx::PaintingSurface::Origin::BottomLeft);
    }
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
#        if defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)
    if (m_impl->uses_cpu_painting_surface) {
        allocate_cpu_painting_surface(color_buffer);
    } else if (!allocate_vkimage_color_buffer(color_buffer, m_size, Gfx::PaintingSurface::Origin::BottomLeft)) {
        // This driver advertises the DMABUF EGL extensions but can't actually allocate a Vulkan/DMABUF painting surface
        // (unsupported format, modifier, or image import). Fall back to a CPU painting surface.
        m_impl->uses_cpu_painting_surface = true;
        free_surface_resources();
        allocate_cpu_painting_surface(color_buffer);
    }
#        else
    (void)allocate_vkimage_color_buffer(color_buffer, m_size, Gfx::PaintingSurface::Origin::BottomLeft);
#        endif
#    elif (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)) || defined(AK_OS_WINDOWS)
    allocate_cpu_painting_surface(color_buffer);
#    endif
    VERIFY(color_buffer.is_allocated());
    VERIFY(eglGetCurrentContext() == m_impl->context);

    glViewport(0, 0, m_size.width(), m_size.height());

    glGenFramebuffers(1, &m_impl->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, color_buffer_texture_target(), color_buffer.texture, 0);

    if (m_drawing_buffer_options.depth || m_drawing_buffer_options.stencil) {
        glGenRenderbuffers(1, &m_impl->depth_buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);

        if (m_drawing_buffer_options.depth && m_drawing_buffer_options.stencil) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else if (m_drawing_buffer_options.depth) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else {
            VERIFY(m_drawing_buffer_options.stencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        }
    }

    VERIFY(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
#endif
}

void OpenGLContext::set_size(Gfx::IntSize const& size)
{
    if (m_size != size)
        m_drawing_buffer_needs_allocation = true;
    m_size = size;
}

void OpenGLContext::make_current()
{
#ifdef ENABLE_WEBGL
    allocate_painting_surface_if_needed();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);
#endif
}

void OpenGLContext::flush_drawing_buffer()
{
#ifdef ENABLE_WEBGL
    make_current();

    // "Before the drawing buffer is presented for compositing the implementation shall ensure that all rendering operations have been flushed to the drawing buffer."
    // With Metal, glFlush flushes the command buffer, but without waiting for it to be scheduled or completed.
    // eglWaitUntilWorkScheduledANGLE flushes the command buffer, and waits until it has been scheduled, hence the name.
    // eglWaitUntilWorkScheduledANGLE only has an effect on CGL and Metal backends, so we only use it on macOS.
#    if defined(AK_OS_MACOS)
    if (m_impl->uses_cpu_painting_surface)
        glFinish();
    else
        eglWaitUntilWorkScheduledANGLE(m_impl->display);
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
    // FIXME: CPU sync for now, but it would be better to export a fence and have Skia wait for it before reading from the surface
    glFinish();
#    elif (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID)) || defined(AK_OS_WINDOWS)
    glFinish();
#    endif

#    if defined(ENABLE_WEBGL_CPU_PAINTING_SURFACE)
    if (m_impl->uses_cpu_painting_surface)
        copy_default_framebuffer_to_cpu_painting_surface();
#    endif
#endif
}

void OpenGLContext::present_drawing_buffer([[maybe_unused]] bool preserve_drawing_buffer)
{
#ifdef ENABLE_WEBGL
    flush_drawing_buffer();

    auto presented_index = m_impl->drawing_color_buffer_index;
    m_impl->presented_color_buffer_index = presented_index;
    if (m_impl->uses_cpu_painting_surface)
        return;

    // The page drew into the shared image behind Skia's back, so drop any snapshot Skia cached of it.
    auto& presented_color_buffer = m_impl->color_buffers[presented_index];
    presented_color_buffer.painting_surface->notify_content_will_change();

    auto next_index = (presented_index + 1) % m_impl->color_buffers.size();
    auto& next_color_buffer = m_impl->color_buffers[next_index];
    // NB: Without a second color buffer the page keeps drawing into the presented one, which a frame composited
    //     while it draws can catch half drawn.
    if (preserve_drawing_buffer && !supports_copy_texture())
        return;
    if (!next_color_buffer.is_allocated() && !allocate_color_buffer(next_color_buffer))
        return;

    if (preserve_drawing_buffer) {
        glCopySubTextureCHROMIUM(presented_color_buffer.texture, 0, color_buffer_texture_target(), next_color_buffer.texture, 0,
            0, 0, 0, 0, m_size.width(), m_size.height(), GL_FALSE, GL_FALSE, GL_FALSE);
    }
    attach_color_buffer_to_default_framebuffer(next_color_buffer);
    m_impl->drawing_color_buffer_index = next_index;
#endif
}

#ifdef ENABLE_WEBGL
bool OpenGLContext::allocate_color_buffer(ColorBuffer& color_buffer)
{
    return allocate_shared_color_buffer(color_buffer, m_size, Gfx::PaintingSurface::Origin::BottomLeft);
}

bool OpenGLContext::allocate_shared_color_buffer([[maybe_unused]] ColorBuffer& color_buffer, [[maybe_unused]] Gfx::IntSize size, [[maybe_unused]] Gfx::PaintingSurface::Origin origin)
{
#    if defined(AK_OS_MACOS)
    allocate_iosurface_color_buffer(color_buffer, size, origin);
    return true;
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
    return allocate_vkimage_color_buffer(color_buffer, size, origin);
#    else
    return false;
#    endif
}

void OpenGLContext::attach_color_buffer_to_default_framebuffer(ColorBuffer const& color_buffer)
{
    GLenum framebuffer_target = GL_FRAMEBUFFER;
    GLenum framebuffer_binding = GL_FRAMEBUFFER_BINDING;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        framebuffer_target = GL_DRAW_FRAMEBUFFER;
        framebuffer_binding = GL_DRAW_FRAMEBUFFER_BINDING;
    }
    GLint original_framebuffer;
    glGetIntegerv(framebuffer_binding, &original_framebuffer);

    glBindFramebuffer(framebuffer_target, default_framebuffer());
    glFramebufferTexture2D(framebuffer_target, GL_COLOR_ATTACHMENT0, color_buffer_texture_target(), color_buffer.texture, 0);

    glBindFramebuffer(framebuffer_target, original_framebuffer);
}

bool OpenGLContext::supports_copy_texture()
{
    if (!m_impl->supports_copy_texture.has_value()) {
        auto const* extensions = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS));
        m_impl->supports_copy_texture = extensions && StringView { extensions, strlen(extensions) }.split_view(' ').contains_slow("GL_CHROMIUM_copy_texture"sv);
    }
    return *m_impl->supports_copy_texture;
}
#endif

GLenum OpenGLContext::get_error()
{
    if (!m_stashed_errors.is_empty())
        return m_stashed_errors.take_first();
    return GLFunctions::get_error();
}

bool OpenGLContext::stash_errors()
{
    auto stashed_any_errors = false;
    for (auto error = GLFunctions::get_error(); error != GL_NO_ERROR; error = GLFunctions::get_error()) {
        if (!m_stashed_errors.contains_slow(error))
            m_stashed_errors.append(error);
        stashed_any_errors = true;
    }
    return stashed_any_errors;
}

void OpenGLContext::discard_errors()
{
    for (auto error = GLFunctions::get_error(); error != GL_NO_ERROR; error = GLFunctions::get_error())
        dbgln("Compositor: Discarding GL error {:#x} raised by the Compositor's own work", error);
}

bool OpenGLContext::can_copy_images_into_textures()
{
#if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES)
    return m_skia_backend_context && !m_impl->uses_cpu_painting_surface && supports_copy_texture();
#else
    return false;
#endif
}

#ifdef ENABLE_WEBGL
static Optional<GLenum> texture_binding_for_copy_destination(GLenum target)
{
    switch (target) {
    case GL_TEXTURE_2D:
        return GL_TEXTURE_BINDING_2D;
    case GL_TEXTURE_CUBE_MAP_POSITIVE_X:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_X:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Y:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Y:
    case GL_TEXTURE_CUBE_MAP_POSITIVE_Z:
    case GL_TEXTURE_CUBE_MAP_NEGATIVE_Z:
        return GL_TEXTURE_BINDING_CUBE_MAP;
    default:
        return {};
    }
}
#endif

bool OpenGLContext::copy_image_into_bound_texture([[maybe_unused]] SkImage const& image, [[maybe_unused]] Gfx::IntSize source_size, [[maybe_unused]] Gfx::IntSize size, [[maybe_unused]] GLenum target, [[maybe_unused]] GLint level, [[maybe_unused]] GLint xoffset, [[maybe_unused]] GLint yoffset, [[maybe_unused]] bool flip_y, [[maybe_unused]] bool premultiply_alpha)
{
#ifdef ENABLE_WEBGL
    auto destination_binding = texture_binding_for_copy_destination(target);
    if (!destination_binding.has_value() || size.is_empty() || !can_copy_images_into_textures())
        return false;

    auto& upload_color_buffer = m_impl->upload_color_buffer;
    if (m_impl->upload_color_buffer_size != size) {
        free_color_buffer(upload_color_buffer);
        m_impl->upload_color_buffer_size = {};
        if (!allocate_shared_color_buffer(upload_color_buffer, size, Gfx::PaintingSurface::Origin::TopLeft))
            return false;
        m_impl->upload_color_buffer_size = size;
    }

    auto& canvas = upload_color_buffer.painting_surface->canvas();
    canvas.clear(SK_ColorTRANSPARENT);
    canvas.drawImageRect(&image, SkRect::MakeIWH(source_size.width(), source_size.height()), SkRect::MakeIWH(size.width(), size.height()), Gfx::to_skia_sampling_options(Gfx::ScalingMode::Bilinear), nullptr, SkCanvas::kStrict_SrcRectConstraint);
    m_skia_backend_context->flush_and_submit(*upload_color_buffer.painting_surface);

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);
    GLint destination_texture = 0;
    glGetIntegerv(destination_binding.value(), &destination_texture);
    glCopySubTextureCHROMIUM(upload_color_buffer.texture, 0, target, destination_texture, level, xoffset, yoffset, 0, 0, size.width(), size.height(), flip_y, premultiply_alpha, GL_FALSE);
    return true;
#else
    return false;
#endif
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::drawing_buffer_surface()
{
    return m_impl->color_buffers[m_impl->drawing_color_buffer_index].painting_surface;
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::presented_surface()
{
    if (!m_impl->presented_color_buffer_index.has_value())
        return nullptr;
    return m_impl->color_buffers[*m_impl->presented_color_buffer_index].painting_surface;
}

u32 OpenGLContext::default_renderbuffer() const
{
    return m_impl->depth_buffer;
}

u32 OpenGLContext::default_framebuffer() const
{
    return m_impl->framebuffer;
}

Vector<String> OpenGLContext::get_supported_opengl_extensions()
{
#ifdef ENABLE_WEBGL
    make_current();

    Vector<String> extensions;

    if (auto const* extensions_string = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS))) {
        StringView extensions_view(extensions_string, strlen(extensions_string));
        for (auto extension : extensions_view.split_view(' '))
            extensions.append(MUST(String::from_utf8(extension)));
    }

    if (auto const* requestable_extensions_string = reinterpret_cast<char const*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE))) {
        StringView requestable_extensions_view(requestable_extensions_string, strlen(requestable_extensions_string));
        for (auto extension : requestable_extensions_view.split_view(' '))
            extensions.append(MUST(String::from_utf8(extension)));
    }

    return extensions;
#else
    (void)m_webgl_version;
    return {};
#endif
}

}
