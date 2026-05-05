/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImagePayload.h>
#ifdef AK_OS_MACOS
#    include <LibCore/MachPort.h>
#endif
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif

namespace Gfx {

class SharedImage {
    AK_MAKE_NONCOPYABLE(SharedImage);

public:
    static SharedImage create(IntSize);

    static SharedImage create(BitmapInfo const&);               // MetalImage.mm if macOS, VulkanImage.cpp if USE_VULKAN_DMABUF_IMAGES
    static SharedImage import_from_payload(SharedImagePayload); // MetalImage.mm if macOS, VulkanImage.cpp if USE_VULKAN_DMABUF_IMAGES
    SharedImage(SharedImage&&);                                 // MetalImage.mm if macOS, VulkanImage.cpp if USE_VULKAN_DMABUF_IMAGES
    SharedImage& operator=(SharedImage&&);                      // MetalImage.mm if macOS, VulkanImage.cpp if USE_VULKAN_DMABUF_IMAGES
    ~SharedImage();                                             // MetalImage.mm if macOS, VulkanImage.cpp if USE_VULKAN_DMABUF_IMAGES
    SharedImagePayload export_payload() const;                  // MetalImage.mm if macOS, VulkanImage.cpp if USE_VULKAN_DMABUF_IMAGES

    static SharedImage create_bitmap_backed(BitmapInfo const&, BitmapAlpha);
#ifdef USE_VULKAN_DMABUF_IMAGES
    static SharedImage create_from_vulkan_image(NonnullRefPtr<VulkanImage>, BitmapInfo const&); // VulkanImage.cpp
#endif

    NonnullRefPtr<PaintingSurface> create_painting_surface(NonnullRefPtr<SkiaBackendContext>, PaintingSurface::Origin = PaintingSurface::Origin::TopLeft);
    bool is_shareable_bitmap_backed() const;
    BitmapInfo const& info() const { return m_info; }
    ColorSpace const& color_space() const { return m_color_space; }
    void set_color_space(ColorSpace color_space) { m_color_space = move(color_space); }
    NonnullRefPtr<Bitmap> bitmap() const { return m_bitmap; }

#ifdef AK_OS_MACOS
    void* platform_surface_handle() const { return m_platform_surface_handle; }
#endif

private:
    friend class PaintingSurface;
    enum class BackingKind : u8 {
        ShareableBitmap,
#ifdef AK_OS_MACOS
        IOSurface,
#endif
#ifdef USE_VULKAN_DMABUF_IMAGES
        LinuxDmaBuf,
        VulkanImage,
#endif
    };

    SharedImage(BitmapInfo const&, NonnullRefPtr<Bitmap>);

#ifdef AK_OS_MACOS
    SharedImage(Core::MachPort&&, BitmapInfo const&, NonnullRefPtr<Bitmap>, void* platform_surface_handle); // defined in MetalImage.mm
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
    SharedImage(LinuxDmaBufPayload&&, BitmapInfo const&, NonnullRefPtr<Bitmap>);       // VulkanImage.cpp
    SharedImage(NonnullRefPtr<VulkanImage>, BitmapInfo const&, NonnullRefPtr<Bitmap>); // VulkanImage.cpp
    VulkanImage* vulkan_image() const { return m_vulkan_image.ptr(); }
    LinuxDmaBufPayload const& linux_dma_buf_payload() const { return m_linux_dma_buf_payload; }
#endif

    static SharedImagePayload make_shareable_bitmap_payload(BitmapInfo const&, NonnullRefPtr<Bitmap> const&, ColorSpace const&);

    BackingKind m_backing_kind { BackingKind::ShareableBitmap };
    BitmapInfo m_info;
    ColorSpace m_color_space;
    NonnullRefPtr<Bitmap> m_bitmap;

#ifdef AK_OS_MACOS
    Core::MachPort m_mach_port;
    void* m_platform_surface_handle { nullptr };
#endif
#ifdef USE_VULKAN_DMABUF_IMAGES
    LinuxDmaBufPayload m_linux_dma_buf_payload;
    RefPtr<VulkanImage> m_vulkan_image;
#endif
};

}
