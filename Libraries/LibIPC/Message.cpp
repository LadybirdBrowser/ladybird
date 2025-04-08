/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
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

ErrorOr<void> MessageBuffer::transfer_message(Transport& transport)
{
    Checked<MessageSizeType> checked_message_size { m_data.size() };
    if (checked_message_size.has_overflow()) {
        return Error::from_string_literal("Message is too large for IPC encoding");
    }

    transport.post_message(m_data, m_fds);
    return {};
}

NonnullOwnPtr<LargeMessageWrapper> LargeMessageWrapper::create(u32 endpoint_magic, MessageBuffer& buffer_to_wrap)
{
    auto size = buffer_to_wrap.data().size();
    auto wrapped_message_data = MUST(Core::AnonymousBuffer::create_with_size(size));
    memcpy(wrapped_message_data.data<void>(), buffer_to_wrap.data().data(), size);
    Vector<File> files;
    for (auto& owned_fd : buffer_to_wrap.take_fds()) {
        files.append(File::adopt_fd(owned_fd->take_fd()));
    }
    return make<LargeMessageWrapper>(endpoint_magic, move(wrapped_message_data), move(files));
}

LargeMessageWrapper::LargeMessageWrapper(u32 endpoint_magic, Core::AnonymousBuffer wrapped_message_data, Vector<File>&& wrapped_fds)
    : m_endpoint_magic(endpoint_magic)
    , m_wrapped_message_data(move(wrapped_message_data))
    , m_wrapped_fds(move(wrapped_fds))
{
}

ErrorOr<MessageBuffer> LargeMessageWrapper::encode() const
{
    MessageBuffer buffer;
    Encoder stream { buffer };
    TRY(stream.encode(m_endpoint_magic));
    TRY(stream.encode(MESSAGE_ID));
    TRY(stream.encode(m_wrapped_message_data));
    TRY(stream.encode(m_wrapped_fds.size()));
    for (auto const& wrapped_fd : m_wrapped_fds) {
        TRY(stream.append_file_descriptor(wrapped_fd.take_fd()));
    }

    return buffer;
}

ErrorOr<NonnullOwnPtr<LargeMessageWrapper>> LargeMessageWrapper::decode(u32 endpoint_magic, Stream& stream, UnprocessedFileDescriptors& files)
{
    Decoder decoder { stream, files };
    auto wrapped_message_data = TRY(decoder.decode<Core::AnonymousBuffer>());

    Vector<File> wrapped_fds;
    auto num_fds = TRY(decoder.decode<u32>());
    for (u32 i = 0; i < num_fds; ++i) {
        auto fd = TRY(decoder.decode<IPC::File>());
        wrapped_fds.append(move(fd));
    }

    return make<LargeMessageWrapper>(endpoint_magic, wrapped_message_data, move(wrapped_fds));
}

}
