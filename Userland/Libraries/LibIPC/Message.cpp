/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibIPC/Message.h>
#include <sched.h>

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

ErrorOr<void> MessageBuffer::transfer_message(Core::LocalSocket& socket)
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

    ReadonlyBytes bytes_to_write { m_data.span() };

    while (!bytes_to_write.is_empty()) {
        ErrorOr<ssize_t> maybe_nwritten = 0;
        if (num_fds_to_transfer > 0) {
            maybe_nwritten = socket.send_message(bytes_to_write, 0, raw_fds);
            if (!maybe_nwritten.is_error())
                num_fds_to_transfer = 0;
        } else {
            maybe_nwritten = socket.write_some(bytes_to_write);
        }

        if (maybe_nwritten.is_error()) {
            if (auto error = maybe_nwritten.release_error(); error.is_errno() && (error.code() == EAGAIN || error.code() == EWOULDBLOCK)) {
                Vector<struct pollfd, 1> pollfds;
                if (pollfds.is_empty())
                    pollfds.append({ .fd = socket.fd().value(), .events = POLLOUT, .revents = 0 });

                ErrorOr<int> result { 0 };
                do {
                    constexpr u32 POLL_TIMEOUT_MS = 100;
                    result = Core::System::poll(pollfds, POLL_TIMEOUT_MS);
                } while (result.is_error() && result.error().code() == EINTR);

                if (!result.is_error() && result.value() != 0)
                    continue;

                switch (error.code()) {
                case EPIPE:
                    return Error::from_string_literal("IPC::transfer_message: Disconnected from peer");
                case EAGAIN:
                    return Error::from_string_literal("IPC::transfer_message: Timed out waiting for socket to become writable");
                default:
                    return Error::from_syscall("IPC::transfer_message write"sv, -error.code());
                }
            } else {
                return error;
            }
        }

        bytes_to_write = bytes_to_write.slice(maybe_nwritten.value());
    }

    return {};
}

}
