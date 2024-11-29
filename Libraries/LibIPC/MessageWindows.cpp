/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Message.h>
#include <io.h>

namespace IPC {

static void invalid_parameter_handler(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
{
}
// Make _get_osfhandle return -1 instead of crashing on invalid fd in release (debug still __debugbreak's)
static auto dummy = _set_invalid_parameter_handler(invalid_parameter_handler);

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
    intptr_t handle = _get_osfhandle(fd);
    if (handle == -1)
        return Error::from_string_literal("Invalid file descriptor");

    m_handle_offsets.append(m_data.size());
    m_data.append((u8*)&handle, sizeof(handle));
    m_fds.append(adopt_ref(*new AutoCloseFileDescriptor(fd)));
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
