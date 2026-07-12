/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImage.h>

#ifdef AK_OS_MACOS
#    include <LibCore/IOSurface.h>
#endif

namespace Gfx {

class SharedImageBuffer {
    AK_MAKE_NONCOPYABLE(SharedImageBuffer);

public:
    static SharedImageBuffer create(IntSize);
    static SharedImageBuffer import_from_shared_image(SharedImage);

    SharedImageBuffer(SharedImageBuffer&&);
    SharedImageBuffer& operator=(SharedImageBuffer&&);
    ~SharedImageBuffer();

    SharedImage export_shared_image() const;

    NonnullRefPtr<Bitmap> bitmap() const
    {
        VERIFY(m_bitmap);
        return *m_bitmap;
    }
    RefPtr<Bitmap> bitmap_if_present() const { return m_bitmap; }

#ifdef AK_OS_MACOS
    Core::IOSurfaceHandle const& iosurface_handle() const { return m_iosurface_handle; }
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    LinuxDmaBufHandle const* linux_dmabuf_handle() const { return m_linux_dmabuf_handle.has_value() ? &m_linux_dmabuf_handle.value() : nullptr; }
#elif defined(USE_DIRECTX)
    WindowsD3DHandle const* windows_d3d_handle() const { return m_windows_d3d_handle.has_value() ? &m_windows_d3d_handle.value() : nullptr; }
#endif

private:
#ifdef AK_OS_MACOS
    SharedImageBuffer(Core::IOSurfaceHandle&&, NonnullRefPtr<Bitmap>);
    Core::IOSurfaceHandle m_iosurface_handle;
#else
    explicit SharedImageBuffer(NonnullRefPtr<Bitmap>);
#    ifdef USE_VULKAN_DMABUF_IMAGES
    SharedImageBuffer(NonnullRefPtr<Bitmap>, LinuxDmaBufHandle&&);
    Optional<LinuxDmaBufHandle> m_linux_dmabuf_handle;
#    endif
#    ifdef USE_DIRECTX
    explicit SharedImageBuffer(WindowsD3DHandle&&);
    Optional<WindowsD3DHandle> m_windows_d3d_handle;
#    endif
#endif
    RefPtr<Bitmap> m_bitmap;
};

}
