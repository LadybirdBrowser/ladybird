/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Attachment.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/Page/SharedBackingStore.h>

#if defined(AK_OS_MACOS)
static Core::MachPort copy_send_right(Core::MachPort const& port)
{
    auto result = mach_port_mod_refs(mach_task_self(), port.port(), MACH_PORT_RIGHT_SEND, +1);
    VERIFY(result == KERN_SUCCESS);
    return Core::MachPort::adopt_right(port.port(), Core::MachPort::PortRight::Send);
}
#endif

namespace Web {

#if defined(AK_OS_MACOS)
SharedBackingStore::SharedBackingStore(Core::MachPort&& port)
    : m_port(move(port))
{
}
#else
SharedBackingStore::SharedBackingStore(Gfx::ShareableBitmap bitmap)
    : m_bitmap(move(bitmap))
{
}
#endif

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::SharedBackingStore const& backing_store)
{
#if defined(AK_OS_MACOS)
    auto port = copy_send_right(backing_store.m_port);
    TRY(encoder.append_attachment(Attachment::from_mach_port(move(port), Core::MachPort::MessageRight::MoveSend)));
#else
    TRY(encoder.encode(backing_store.m_bitmap));
#endif
    return {};
}

template<>
ErrorOr<Web::SharedBackingStore> decode(Decoder& decoder)
{
#if defined(AK_OS_MACOS)
    auto attachment = decoder.attachments().dequeue();
    VERIFY(attachment.message_right() == Core::MachPort::MessageRight::MoveSend);
    return Web::SharedBackingStore { attachment.release_mach_port() };
#else
    auto bitmap = TRY(decoder.decode<Gfx::ShareableBitmap>());
    return Web::SharedBackingStore { move(bitmap) };
#endif
}

}
