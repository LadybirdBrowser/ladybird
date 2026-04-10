/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImage.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Gfx {

struct VulkanContext;

class SharedImageBuffer {
    AK_MAKE_NONCOPYABLE(SharedImageBuffer);

public:
    static SharedImageBuffer create(IntSize);
    static ErrorOr<SharedImageBuffer> import_from_payload(SharedImage, [[maybe_unused]] VulkanContext const* = nullptr);

    SharedImageBuffer(SharedImageBuffer&&);
    SharedImageBuffer& operator=(SharedImageBuffer&&);
    ~SharedImageBuffer();

    SharedImage export_payload() const;

    NonnullRefPtr<Bitmap> bitmap() const { return m_bitmap; }

#ifdef AK_OS_MACOS
    Core::IOSurfaceHandle const& iosurface_handle() const { return m_iosurface_handle; }
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
    RefPtr<VulkanImage> vulkan_image() const { return m_vulkan_image; }
#endif

private:
#ifdef AK_OS_MACOS
    SharedImageBuffer(Core::IOSurfaceHandle&&, NonnullRefPtr<Bitmap>);
    Core::IOSurfaceHandle m_iosurface_handle;
#else
    explicit SharedImageBuffer(NonnullRefPtr<Bitmap>
#    ifdef USE_VULKAN_DMABUF_IMAGES
        ,
        RefPtr<VulkanImage> = {}
#    endif
    );
#endif
    NonnullRefPtr<Bitmap> m_bitmap;

#ifdef USE_VULKAN_DMABUF_IMAGES
    RefPtr<VulkanImage> m_vulkan_image;
#endif
};

}
