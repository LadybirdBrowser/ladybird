/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Message.h>
#include <LibIPC/RawAttachment.h>

namespace IPC {

Vector<Attachment> adopt_raw_attachments(ReadonlySpan<RawAttachment> raw_attachments)
{
    Vector<Attachment> attachments;
    for (auto const& raw : raw_attachments) {
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
        auto disposition = static_cast<Core::MachPort::MessageRight>(raw.disposition);
        auto right = Core::MachPort::PortRight::Send;
        if (disposition == Core::MachPort::MessageRight::MoveReceive || disposition == Core::MachPort::MessageRight::MakeSend || disposition == Core::MachPort::MessageRight::MakeSendOnce)
            right = Core::MachPort::PortRight::Receive;
        else if (disposition == Core::MachPort::MessageRight::MoveSendOnce)
            right = Core::MachPort::PortRight::SendOnce;
        attachments.append(Attachment::from_mach_port(Core::MachPort::adopt_right(static_cast<mach_port_t>(raw.value), right), disposition));
#else
        attachments.append(Attachment::from_fd(raw.value));
#endif
    }
    return attachments;
}

Vector<RawAttachment> release_raw_attachments(Vector<Attachment> attachments)
{
    Vector<RawAttachment> raw;
    for (auto& attachment : attachments) {
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
        auto disposition = to_underlying(attachment.message_right());
        raw.append({ static_cast<int>(attachment.release_mach_port().release()), disposition });
#else
        raw.append({ attachment.to_fd(), 0 });
#endif
    }
    return raw;
}

MessageBuffer adopt_encoded_message(ReadonlyBytes bytes, ReadonlySpan<RawAttachment> attachments)
{
    MessageDataType data;
    MUST(data.try_append(bytes.data(), bytes.size()));
    return { move(data), adopt_raw_attachments(attachments) };
}

}
