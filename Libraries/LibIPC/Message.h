/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/Transport.h>

namespace IPC {

class MessageBuffer {
public:
    MessageBuffer();

    MessageBuffer(MessageDataType data, Vector<Attachment> attachments)
        : m_data(move(data))
        , m_attachments(move(attachments))
    {
    }

    ErrorOr<void> extend_data_capacity(size_t capacity);
    ErrorOr<void> append_data(u8 const* values, size_t count);

    ErrorOr<void> append_file_descriptor(int fd);
    ErrorOr<void> append_attachment(Attachment);

    ErrorOr<void> extend(MessageBuffer&& buffer);

    ErrorOr<void> transfer_message(Transport& transport);

    MessageDataType const& data() const { return m_data; }
    MessageDataType take_data() { return move(m_data); }

    Vector<Attachment> const& attachments() const { return m_attachments; }
    Vector<Attachment> take_attachments() { return move(m_attachments); }

private:
    MessageDataType m_data;
    Vector<Attachment> m_attachments;
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
