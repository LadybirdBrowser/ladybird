/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
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

class PaintingSurface : public AtomicRefCounted<PaintingSurface> {
public:
    enum class Origin {
        TopLeft,
        BottomLeft,
    };

    Function<void(PaintingSurface&)> on_flush;

    // A surface whose pixels are in the color space, which is sRGB unless one is given.
    static NonnullRefPtr<PaintingSurface> create_with_size(IntSize size, BitmapFormat color_type, AlphaType alpha_type, RefPtr<SkiaBackendContext> = {}, ColorSpace const& = {});
    static NonnullRefPtr<PaintingSurface> wrap_bitmap(Bitmap&);

#ifdef AK_OS_MACOS
    static NonnullRefPtr<PaintingSurface> create_from_shared_image_buffer(SharedImageBuffer&, NonnullRefPtr<SkiaBackendContext>, Origin = Origin::TopLeft);
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
    static NonnullRefPtr<PaintingSurface> create_from_vkimage(NonnullRefPtr<SkiaBackendContext> context, NonnullRefPtr<VulkanImage> vulkan_image, Origin origin);
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

    void flush();

    ~PaintingSurface();

private:
    struct Impl;

    PaintingSurface(NonnullOwnPtr<Impl>&&);

    NonnullOwnPtr<Impl> m_impl;
};

}
