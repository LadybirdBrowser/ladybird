/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/Forward.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>

namespace IPC {

class AutoCloseFileDescriptor : public RefCounted<AutoCloseFileDescriptor> {
public:
    AutoCloseFileDescriptor(int fd)
        : m_fd(fd)
    {
    }

    ~AutoCloseFileDescriptor()
    {
        if (m_fd != -1)
            (void)Core::System::close(m_fd);
    }

    int value() const { return m_fd; }

private:
    int m_fd;
};

class MessageBuffer {
public:
    MessageBuffer();

    // Do not copy message buffers.
    MessageBuffer(MessageBuffer const&) = delete;
    MessageBuffer& operator=(MessageBuffer const&) = delete;

    MessageBuffer(MessageBuffer&&) noexcept = default;
    MessageBuffer& operator=(MessageBuffer&&) noexcept = default;

    ErrorOr<void> extend_data_capacity(size_t capacity);
    ErrorOr<void> append_data(u8 const* values, size_t count);

    ErrorOr<void> append_file_descriptor(int fd);

    ErrorOr<void> transfer_message(Transport& transport);

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
    u64 ipc_request_id() const { return m_request_id; }
    void set_ipc_request_id(u64 request_id) { m_request_id = request_id; }
    constexpr bool is_response() const { return (message_id() % 2) == 0; }

protected:
    explicit Message(u64 request_id)
        : m_request_id(request_id)
    {
    }

private:
    u64 m_request_id;
};
}
