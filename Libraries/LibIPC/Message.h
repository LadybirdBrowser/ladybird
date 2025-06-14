/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/Transport.h>

namespace IPC {

class MessageBuffer {
public:
    MessageBuffer();

    MessageBuffer(Vector<u8, 1024> data, Vector<NonnullRefPtr<AutoCloseFileDescriptor>, 1> fds)
        : m_data(move(data))
        , m_fds(move(fds))
    {
    }

    ErrorOr<void> extend_data_capacity(size_t capacity);
    ErrorOr<void> append_data(u8 const* values, size_t count);

    ErrorOr<void> append_file_descriptor(int fd);

    ErrorOr<void> transfer_message(Transport& transport);

    auto const& data() const { return m_data; }
    auto take_fds() { return move(m_fds); }

private:
    Vector<u8, 1024> m_data;
    Vector<NonnullRefPtr<AutoCloseFileDescriptor>, 1> m_fds;
#ifdef AK_OS_WINDOWS
    Vector<size_t> m_handle_offsets;
#endif
};

enum class ErrorCode : u32 {
    PeerDisconnected
};

template<typename Value>
using IPCErrorOr = ErrorOr<Value, ErrorCode>;

class Message {
public:
    virtual ~Message() = default;

    virtual u32 endpoint_magic() const = 0;
    virtual int message_id() const = 0;
    virtual char const* message_name() const = 0;
    virtual ErrorOr<MessageBuffer> encode() const = 0;

protected:
    Message() = default;
};

}
