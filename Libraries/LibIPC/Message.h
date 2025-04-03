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
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibCore/System.h>
#include <LibIPC/Transport.h>
#include <LibIPC/UnprocessedFileDescriptors.h>

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

    int take_fd()
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

private:
    int m_fd;
};

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
    bool m_fds_taken { false };
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

class LargeMessageWrapper : public Message {
public:
    ~LargeMessageWrapper() override = default;

    static constexpr int MESSAGE_ID = 0x0;

    static NonnullOwnPtr<LargeMessageWrapper> create(u32 endpoint_magic, MessageBuffer& buffer_to_wrap);

    u32 endpoint_magic() const override { return m_endpoint_magic; }
    int message_id() const override { return MESSAGE_ID; }
    char const* message_name() const override { return "LargeMessageWrapper"; }
    ErrorOr<MessageBuffer> encode() const override;

    static ErrorOr<NonnullOwnPtr<LargeMessageWrapper>> decode(u32 endpoint_magic, Stream& stream, UnprocessedFileDescriptors& files);

    ReadonlyBytes wrapped_message_data() const { return ReadonlyBytes { m_wrapped_message_data.data<u8>(), m_wrapped_message_data.size() }; }
    auto take_fds() { return move(m_wrapped_fds); }

    LargeMessageWrapper(u32 endpoint_magic, Core::AnonymousBuffer wrapped_message_data, Vector<IPC::File>&& wrapped_fds);

private:
    u32 m_endpoint_magic { 0 };
    Core::AnonymousBuffer m_wrapped_message_data;
    Vector<File> m_wrapped_fds;
};

}
