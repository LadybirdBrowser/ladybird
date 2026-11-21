/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/kmalloc.h>
#include <Compositor/SkiaBackendContext.h>
#include <LibGfx/Color.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Size.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
namespace Gfx {

struct VulkanImage;

}
#endif

#ifdef USE_DIRECTX
#    include <AK/Error.h>

namespace Gfx {

class D3DSharedTexture;

}
#endif

#ifdef AK_OS_MACOS
#    include <LibGfx/MetalContext.h>
#endif

class SkCanvas;
class SkSurface;

namespace Gfx {

class SharedImage;

// A surface that the Compositor paints into. A pointer to it is the Rust core's surface itself, which the core and the
// C++ around it share by reference counting. Only the Compositor's main thread may reference or release it.
class PaintingSurface {
    AK_MAKE_NONCOPYABLE(PaintingSurface);
    AK_MAKE_NONMOVABLE(PaintingSurface);

public:
    enum class Origin {
        TopLeft,
        BottomLeft,
    };

    enum class TextAntialiasing {
        Grayscale,
        LCD,
    };

    // A surface whose pixels are in the color space, which is sRGB unless one is given.
    static NonnullRefPtr<PaintingSurface> create_with_size(IntSize size, BitmapFormat color_type, AlphaType alpha_type, RefPtr<SkiaBackendContext> = {}, ColorSpace const& = {}, TextAntialiasing = TextAntialiasing::Grayscale);
    // Fails when neither a GPU surface nor a bitmap of the size can be allocated.
    static ErrorOr<NonnullRefPtr<PaintingSurface>> try_create_with_size(IntSize size, BitmapFormat color_type, AlphaType alpha_type, RefPtr<SkiaBackendContext> = {}, ColorSpace const& = {}, TextAntialiasing = TextAntialiasing::Grayscale);
    static NonnullRefPtr<PaintingSurface> wrap_bitmap(Bitmap&, TextAntialiasing = TextAntialiasing::Grayscale);

#ifdef AK_OS_MACOS
    static NonnullRefPtr<PaintingSurface> create_from_shared_image_buffer(SharedImageBuffer&, NonnullRefPtr<SkiaBackendContext>, Origin = Origin::TopLeft);
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
    static NonnullRefPtr<PaintingSurface> create_from_vkimage(NonnullRefPtr<SkiaBackendContext> context, NonnullRefPtr<VulkanImage> vulkan_image, Origin origin, TextAntialiasing = TextAntialiasing::Grayscale);
#endif

#ifdef USE_DIRECTX
    static ErrorOr<NonnullRefPtr<PaintingSurface>> create_from_d3d_texture(NonnullRefPtr<SkiaBackendContext>, NonnullRefPtr<D3DSharedTexture>, Origin = Origin::TopLeft);
#endif

    // The pixels of the bitmap are in the color space, which is sRGB unless one is given. The surface converts them
    // from or to the color space of its own pixels.
    void read_into_bitmap(Bitmap&, IntPoint source_position = {}, ColorSpace const& = {}) const;
    void write_from_bitmap(Bitmap const&, IntPoint destination_position = {}, ColorSpace const& = {});
    void copy_from_surface(PaintingSurface&);

    void notify_content_will_change();

    IntSize size() const;
    IntRect rect() const;

    SkCanvas& canvas() const;
    SkSurface& sk_surface() const;

    template<typename T>
    T sk_image_snapshot() const;

    RefPtr<SkiaBackendContext> skia_backend_context() const;

    // Whether the surface draws into an image shared with another process, rather than one Skia allocated.
    bool wraps_shared_image() const;

    // Runs after every flush.
    void set_on_flush(Function<void(PaintingSurface&)>);
    void flush();

    void ref() const;
    void unref() const;

    PaintingSurface() = delete;
    ~PaintingSurface() = delete;

private:
    struct Impl;

    static NonnullRefPtr<PaintingSurface> adopt_impl(IntSize, NonnullOwnPtr<Impl>);
    static void destroy_impl(void*);
    Impl& impl() const;
};

}

template<>
inline constexpr bool AllocatedWithCustomAllocator<Gfx::PaintingSurface> = true;
