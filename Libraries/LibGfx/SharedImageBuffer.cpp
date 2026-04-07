/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/SharedImageBuffer.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <libdrm/drm_fourcc.h>
#    include <sys/mman.h>
#endif

namespace Gfx {

#ifdef AK_OS_MACOS
static constexpr auto shared_image_buffer_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_buffer_alpha_type = AlphaType::Premultiplied;

static NonnullRefPtr<Bitmap> create_bitmap_from_iosurface(Core::IOSurfaceHandle const& iosurface_handle)
{
    auto size = IntSize(static_cast<int>(iosurface_handle.width()), static_cast<int>(iosurface_handle.height()));
    auto bitmap_handle = Core::IOSurfaceHandle::from_mach_port(iosurface_handle.create_mach_port());
    return MUST(Bitmap::create_wrapper(shared_image_buffer_format, shared_image_buffer_alpha_type, size, iosurface_handle.bytes_per_row(), iosurface_handle.data(), [handle = move(bitmap_handle)] { }));
}

SharedImageBuffer::SharedImageBuffer(Core::IOSurfaceHandle&& iosurface_handle, NonnullRefPtr<Bitmap> bitmap)
    : m_iosurface_handle(move(iosurface_handle))
    , m_bitmap(move(bitmap))
{
}
#else
static constexpr auto shared_image_buffer_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_buffer_alpha_type = AlphaType::Premultiplied;
#    ifdef USE_VULKAN_DMABUF_IMAGES
static constexpr auto shared_image_buffer_drm_format = DRM_FORMAT_ARGB8888;

static NonnullRefPtr<Bitmap> create_bitmap_from_linux_dmabuf(LinuxDmaBufHandle const& dmabuf)
{
    VERIFY(dmabuf.bitmap_format == shared_image_buffer_format);
    VERIFY(dmabuf.alpha_type == shared_image_buffer_alpha_type);
    VERIFY(dmabuf.drm_format == shared_image_buffer_drm_format);
    VERIFY(dmabuf.modifier == DRM_FORMAT_MOD_LINEAR);
    auto data_size = Bitmap::size_in_bytes(dmabuf.pitch, dmabuf.size.height());
    auto* data = ::mmap(nullptr, data_size, PROT_READ | PROT_WRITE, MAP_SHARED, dmabuf.file.fd(), 0);
    VERIFY(data != MAP_FAILED);
    return MUST(Bitmap::create_wrapper(dmabuf.bitmap_format, dmabuf.alpha_type, dmabuf.size, dmabuf.pitch, data, [data, data_size] {
        VERIFY(::munmap(data, data_size) == 0);
    }));
}
#    endif

SharedImageBuffer::SharedImageBuffer(NonnullRefPtr<Bitmap> bitmap)
    : m_bitmap(move(bitmap))
{
}
#endif

SharedImageBuffer SharedImageBuffer::create(IntSize size)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::create(size.width(), size.height());
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return SharedImageBuffer(move(iosurface_handle), move(bitmap));
#else
    return SharedImageBuffer(MUST(Bitmap::create_shareable(shared_image_buffer_format, shared_image_buffer_alpha_type, size)));
#endif
}

SharedImageBuffer SharedImageBuffer::import_from_shared_image(SharedImage shared_image)
{
#ifdef AK_OS_MACOS
    auto iosurface_handle = Core::IOSurfaceHandle::from_mach_port(shared_image.m_port);
    auto bitmap = create_bitmap_from_iosurface(iosurface_handle);
    return SharedImageBuffer(move(iosurface_handle), move(bitmap));
#else
    return shared_image.m_data.visit(
        [](ShareableBitmap& shareable_bitmap) -> SharedImageBuffer {
            return SharedImageBuffer(*shareable_bitmap.bitmap());
        },
        [](LinuxDmaBufHandle& dmabuf) -> SharedImageBuffer {
#    ifdef USE_VULKAN_DMABUF_IMAGES
            return SharedImageBuffer(create_bitmap_from_linux_dmabuf(dmabuf));
#    else
            (void)dmabuf;
            VERIFY_NOT_REACHED();
#    endif
        });
#endif
}

SharedImageBuffer::SharedImageBuffer(SharedImageBuffer&&) = default;

SharedImageBuffer& SharedImageBuffer::operator=(SharedImageBuffer&&) = default;

SharedImageBuffer::~SharedImageBuffer() = default;

SharedImage SharedImageBuffer::export_shared_image() const
{
#ifdef AK_OS_MACOS
    return SharedImage { m_iosurface_handle.create_mach_port() };
#else
    return SharedImage { ShareableBitmap { m_bitmap, ShareableBitmap::ConstructWithKnownGoodBitmap } };
#endif
}

}
