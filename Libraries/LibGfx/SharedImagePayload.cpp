/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SharedImagePayload.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

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
SharedImagePayload::SharedImagePayload(Core::MachPort&& port)
    : m_port(move(port))
{
}
#else
SharedImagePayload::SharedImagePayload(ShareableBitmap shareable_bitmap)
    : m_shareable_bitmap(move(shareable_bitmap))
{
}

SharedImagePayload::SharedImagePayload(LinuxDmaBufBackingStore backing_store)
    : m_linux_dma_buf(move(backing_store))
{
    VERIFY(m_linux_dma_buf->fd.fd() >= 0);
}
#endif

}

namespace IPC {

#if !defined(AK_OS_MACOS)
enum class SharedImageKind : u8 {
    Bitmap,
    LinuxDmaBuf,
};
#endif

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SharedImagePayload const& shared_image)
{
#ifdef AK_OS_MACOS
    TRY(encoder.append_attachment(Attachment::from_mach_port(copy_send_right(shared_image.m_port), Core::MachPort::MessageRight::MoveSend)));
#else
    if (shared_image.m_shareable_bitmap.has_value()) {
        TRY(encoder.encode(static_cast<u8>(SharedImageKind::Bitmap)));
        TRY(encoder.encode(shared_image.m_shareable_bitmap.value()));
    } else {
        VERIFY(shared_image.m_linux_dma_buf.has_value());
        auto const& dma_buf = shared_image.m_linux_dma_buf.value();
        TRY(encoder.encode(static_cast<u8>(SharedImageKind::LinuxDmaBuf)));
        TRY(encoder.encode(dma_buf.drm_format));
        TRY(encoder.encode(dma_buf.modifier));
        TRY(encoder.encode(dma_buf.size.width()));
        TRY(encoder.encode(dma_buf.size.height()));
        TRY(encoder.encode(dma_buf.plane.stride));
        TRY(encoder.encode(dma_buf.plane.offset));
        TRY(encoder.encode(dma_buf.fd));
    }
#endif
    return {};
}

template<>
ErrorOr<Gfx::SharedImagePayload> decode(Decoder& decoder)
{
#ifdef AK_OS_MACOS
    auto attachment = decoder.attachments().dequeue();
    VERIFY(attachment.message_right() == Core::MachPort::MessageRight::MoveSend);
    return Gfx::SharedImagePayload { attachment.release_mach_port() };
#else
    auto kind = static_cast<SharedImageKind>(TRY(decoder.decode<u8>()));
    switch (kind) {
    case SharedImageKind::Bitmap: {
        auto shareable_bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
        VERIFY(shareable_bitmap.is_valid());
        return Gfx::SharedImagePayload { move(shareable_bitmap) };
    }
    case SharedImageKind::LinuxDmaBuf: {
        Gfx::LinuxDmaBufBackingStore dma_buf;
        dma_buf.drm_format = TRY(decoder.decode<u32>());
        dma_buf.modifier = TRY(decoder.decode<u64>());
        int width = TRY(decoder.decode<int>());
        int height = TRY(decoder.decode<int>());
        dma_buf.size = { width, height };

        dma_buf.plane.stride = TRY(decoder.decode<u32>());
        dma_buf.plane.offset = TRY(decoder.decode<u32>());
        dma_buf.fd = TRY(decoder.decode<IPC::File>());

        return Gfx::SharedImagePayload { move(dma_buf) };
    }
    }

    return Error::from_string_literal("Unknown SharedImagePayload kind");
#endif
}

}
