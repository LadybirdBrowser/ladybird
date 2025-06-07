/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <LibIPC/HandleType.h>
#include <LibIPC/Message.h>

#include <AK/Windows.h>

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

ErrorOr<void> MessageBuffer::append_file_descriptor(int handle)
{
    TRY(m_fds.try_append(adopt_ref(*new AutoCloseFileDescriptor(handle))));
    TRY(m_handle_offsets.try_append(m_data.size()));

    if (Core::System::is_socket(handle)) {
        auto type = HandleType::Socket;
        TRY(m_data.try_append(to_underlying(type)));

        // The handle will be duplicated and WSAPROTOCOL_INFO will be filled later in TransportSocketWindows::transfer.
        // It can't be duplicated here because it requires peer process pid, which only TransportSocketWindows knows about.
        WSAPROTOCOL_INFO pi = {};
        static_assert(sizeof(pi) >= sizeof(int));
        ByteReader::store(reinterpret_cast<u8*>(&pi), handle);
        TRY(m_data.try_append(reinterpret_cast<u8*>(&pi), sizeof(pi)));
    } else {
        auto type = HandleType::Generic;
        TRY(m_data.try_append(to_underlying(type)));
        // The handle will be overwritten by a duplicate handle later in TransportSocketWindows::transfer (for the same reason).
        TRY(m_data.try_append(reinterpret_cast<u8*>(&handle), sizeof(handle)));
    }
    return {};
}

ErrorOr<void> MessageBuffer::transfer_message(Transport& transport)
{
    Checked<MessageSizeType> checked_message_size { m_data.size() };
    if (checked_message_size.has_overflow())
        return Error::from_string_literal("Message is too large for IPC encoding");

    TRY(transport.transfer_message(m_data, m_handle_offsets));
    return {};
}

}
