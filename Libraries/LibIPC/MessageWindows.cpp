/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Message.h>

#include <AK/Windows.h>

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
    using namespace Core::System;

    HANDLE handle = fd_to_handle(fd);
    if (handle == INVALID_HANDLE_VALUE)
        return Error::from_string_literal("Invalid file descriptor");

    m_fds.append(adopt_ref(*new AutoCloseFileDescriptor(fd)));
    m_handle_offsets.append(m_data.size());

    if (fd < 0) {
        HandleType type = FileMappingHandle;
        m_data.append((u8*)&type, sizeof(type));
        // the handle will be overwritten by a duplicate handle later in TransportSocketWindows::transfer
        m_data.append((u8*)&handle, sizeof(handle));
    } else {
        HandleType type = SocketHandle;
        m_data.append((u8*)&type, sizeof(type));
        WSAPROTOCOL_INFO pi = {};
        *(HANDLE*)&pi = handle;
        // the handle will be duplicated and WSAPROTOCOL_INFO will be filled later in TransportSocketWindows::transfer
        m_data.append((u8*)&pi, sizeof(pi));
    }
    return {};
}

ErrorOr<void> MessageBuffer::transfer_message(Transport& transport)
{
    VERIFY(m_data.size() >= sizeof(MessageSizeType) && m_data.size() < NumericLimits<MessageSizeType>::max());
    size_t message_size = m_data.size() - sizeof(MessageSizeType);
    *(MessageSizeType*)m_data.data() = message_size;

    TRY(transport.transfer(m_data.span(), m_handle_offsets));
    return {};
}

}
