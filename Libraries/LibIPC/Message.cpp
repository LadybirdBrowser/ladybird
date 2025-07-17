/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Message.h>

namespace IPC {

using MessageSizeType = u32;

MessageBuffer::MessageBuffer()
{
}

ErrorOr<void> MessageBuffer::extend_data_capacity(size_t capacity)
{
    TRY(m_data.try_ensure_capacity(m_data.size() + capacity));
    return {};
}

ErrorOr<void> MessageBuffer::append_data(u8 const* values, size_t count)
{
    TRY(m_data.try_append(values, count));
    return {};
}

ErrorOr<void> MessageBuffer::append_file_descriptor(int fd)
{
    auto auto_fd = TRY(adopt_nonnull_ref_or_enomem(new (nothrow) AutoCloseFileDescriptor(fd)));
    TRY(m_fds.try_append(move(auto_fd)));
    return {};
}

ErrorOr<void> MessageBuffer::extend(MessageBuffer&& buffer)
{
    TRY(m_data.try_extend(move(buffer.m_data)));
    TRY(m_fds.try_extend(move(buffer.m_fds)));
    return {};
}

ErrorOr<void> MessageBuffer::transfer_message(Transport& transport)
{
    Checked<MessageSizeType> checked_message_size { m_data.size() };
    if (checked_message_size.has_overflow()) {
        return Error::from_string_literal("Message is too large for IPC encoding");
    }

    transport.post_message(m_data, m_fds);
    return {};
}

}
