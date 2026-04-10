/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/SharedImage.h>
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
SharedImage::SharedImage(Core::MachPort&& port)
    : m_port(move(port))
{
}
#else
SharedImage::SharedImage(ShareableBitmap shareable_bitmap)
    : m_shareable_bitmap(move(shareable_bitmap))
{
}
#endif

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Gfx::SharedImage const& shared_image)
{
#ifdef AK_OS_MACOS
    TRY(encoder.append_attachment(Attachment::from_mach_port(copy_send_right(shared_image.m_port), Core::MachPort::MessageRight::MoveSend)));
#else
    TRY(encoder.encode(shared_image.m_shareable_bitmap));
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
    auto shareable_bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    VERIFY(shareable_bitmap.is_valid());
    return Gfx::SharedImage { move(shareable_bitmap) };
#endif
}

}
