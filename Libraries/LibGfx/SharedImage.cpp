/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SharedImage.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif

#ifdef AK_OS_MACOS
static Core::MachPort copy_send_right(Core::MachPort const& port)
{
    auto result = mach_port_mod_refs(mach_task_self(), port.port(), MACH_PORT_RIGHT_SEND, +1);
    VERIFY(result == KERN_SUCCESS);
    return Core::MachPort::adopt_right(port.port(), Core::MachPort::PortRight::Send);
}
#endif

namespace Gfx {

#ifdef AK_OS_MACOS
SharedImage::SharedImage(Core::MachPort&& port)
    : m_port(move(port))
{
}
#else
SharedImage::SharedImage(ShareableBitmap shareable_bitmap)
    : m_data(move(shareable_bitmap))
{
}

SharedImage::SharedImage(LinuxDmaBufHandle&& dmabuf)
    : m_data(move(dmabuf))
{
}

#    ifdef USE_VULKAN_DMABUF_IMAGES
static constexpr auto shared_image_bitmap_format = BitmapFormat::BGRA8888;
static constexpr auto shared_image_alpha_type = AlphaType::Premultiplied;

SharedImage duplicate_shared_image(VulkanImage const& vulkan_image)
{
    return SharedImage { duplicate_linux_dmabuf_handle(vulkan_image) };
}

LinuxDmaBufHandle duplicate_linux_dmabuf_handle(VulkanImage const& vulkan_image)
{
    VERIFY(vulkan_image.info.format == VK_FORMAT_B8G8R8A8_UNORM);
    auto fd = vulkan_image.get_dma_buf_fd();
    VERIFY(fd >= 0);
    return LinuxDmaBufHandle {
        .bitmap_format = shared_image_bitmap_format,
        .alpha_type = shared_image_alpha_type,
        .size = IntSize(static_cast<int>(vulkan_image.info.extent.width), static_cast<int>(vulkan_image.info.extent.height)),
        .drm_format = vk_format_to_drm_format(vulkan_image.info.format),
        .pitch = static_cast<size_t>(vulkan_image.info.row_pitch),
        .modifier = vulkan_image.info.modifier,
        .file = IPC::File::adopt_fd(fd),
    };
}
#    endif
#endif

}

namespace IPC {

#ifndef AK_OS_MACOS
enum class SharedImageBackingType : u8 {
    ShareableBitmap,
    LinuxDmaBuf,
};

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::LinuxDmaBufHandle const& dmabuf)
{
    TRY(encoder.encode(dmabuf.bitmap_format));
    TRY(encoder.encode(dmabuf.alpha_type));
    TRY(encoder.encode(dmabuf.size));
    TRY(encoder.encode(dmabuf.drm_format));
    TRY(encoder.encode(dmabuf.pitch));
    TRY(encoder.encode(dmabuf.modifier));
    TRY(encoder.encode(TRY(IPC::File::clone_fd(dmabuf.file.fd()))));
    return {};
}

template<>
ErrorOr<Gfx::LinuxDmaBufHandle> decode(Decoder& decoder)
{
    return Gfx::LinuxDmaBufHandle {
        .bitmap_format = TRY(decoder.decode<Gfx::BitmapFormat>()),
        .alpha_type = TRY(decoder.decode<Gfx::AlphaType>()),
        .size = TRY(decoder.decode<Gfx::IntSize>()),
        .drm_format = TRY(decoder.decode<u32>()),
        .pitch = TRY(decoder.decode<size_t>()),
        .modifier = TRY(decoder.decode<u64>()),
        .file = TRY(decoder.decode<IPC::File>()),
    };
}
#endif

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SharedImage const& shared_image)
{
#ifdef AK_OS_MACOS
    TRY(encoder.append_attachment(Attachment::from_mach_port(copy_send_right(shared_image.m_port), Core::MachPort::MessageRight::MoveSend)));
#else
    return shared_image.m_data.visit(
        [&](Gfx::ShareableBitmap const& shareable_bitmap) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::ShareableBitmap));
            TRY(encoder.encode(shareable_bitmap));
            return {};
        },
        [&](Gfx::LinuxDmaBufHandle const& dmabuf) -> ErrorOr<void> {
            TRY(encoder.encode(SharedImageBackingType::LinuxDmaBuf));
            TRY(encoder.encode(dmabuf));
            return {};
        });
#endif
    return {};
}

template<>
ErrorOr<Gfx::SharedImage> decode(Decoder& decoder)
{
#ifdef AK_OS_MACOS
    auto attachment = decoder.attachments().dequeue();
    VERIFY(attachment.message_right() == Core::MachPort::MessageRight::MoveSend);
    return Gfx::SharedImage { attachment.release_mach_port() };
#else
    switch (TRY(decoder.decode<SharedImageBackingType>())) {
    case SharedImageBackingType::ShareableBitmap:
        return Gfx::SharedImage { TRY(decoder.decode<Gfx::ShareableBitmap>()) };
    case SharedImageBackingType::LinuxDmaBuf:
        return Gfx::SharedImage { TRY(decoder.decode<Gfx::LinuxDmaBufHandle>()) };
    default:
        VERIFY_NOT_REACHED();
    }
#endif
}

}
