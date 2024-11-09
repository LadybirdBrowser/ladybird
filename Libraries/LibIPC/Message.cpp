/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibIPC/Message.h>

namespace IPC {

using MessageSizeType = u32;

MessageBuffer::MessageBuffer()
{
    m_data.resize(sizeof(MessageSizeType));
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

ErrorOr<void> MessageBuffer::transfer_message(Transport& transport)
{
    Checked<MessageSizeType> checked_message_size { m_data.size() };
    checked_message_size -= sizeof(MessageSizeType);

    if (checked_message_size.has_overflow())
        return Error::from_string_literal("Message is too large for IPC encoding");

    MessageSizeType const message_size = checked_message_size.value();
    m_data.span().overwrite(0, reinterpret_cast<u8 const*>(&message_size), sizeof(message_size));

    auto raw_fds = Vector<int, 1> {};
    auto num_fds_to_transfer = m_fds.size();
    if (num_fds_to_transfer > 0) {
        raw_fds.ensure_capacity(num_fds_to_transfer);
        for (auto& owned_fd : m_fds) {
            raw_fds.unchecked_append(owned_fd->value());
        }
    }

    TRY(transport.transfer(m_data.span(), raw_fds));
    return {};
}

}
