/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Mutex.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/Types.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/HandleBroker.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportHandle.h>
#include <LibIPC/TransportSocketWindows.h>

#include <AK/Windows.h>

namespace IPC {

// A message with attachments carries the handle broker token that names them right after its header.
struct TransportSocketWindows::MessageHeader {
    u32 payload_size { 0 };
    u32 attachment_count { 0 };

    size_t token_size() const { return attachment_count > 0 ? sizeof(HandleBrokerToken) : 0; }
};

ErrorOr<NonnullOwnPtr<TransportSocketWindows>> TransportSocketWindows::from_socket(NonnullOwnPtr<Core::LocalSocket> socket)
{
    return make<TransportSocketWindows>(move(socket));
}

ErrorOr<TransportSocketWindows::Paired> TransportSocketWindows::create_paired()
{
    int fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, fds));

    ArmedScopeGuard guard_fd_0 { [&] { MUST(Core::System::close(fds[0])); } };
    ArmedScopeGuard guard_fd_1 { [&] { MUST(Core::System::close(fds[1])); } };

    auto socket0 = TRY(Core::LocalSocket::adopt_fd(fds[0]));
    guard_fd_0.disarm();
    TRY(socket0->set_close_on_exec(true));
    TRY(socket0->set_blocking(false));

    TRY(Core::System::set_close_on_exec(fds[1], true));
    guard_fd_1.disarm();

    return Paired {
        make<TransportSocketWindows>(move(socket0)),
        TransportHandle { File::adopt_fd(fds[1]) },
    };
}

TransportSocketWindows::TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket)
    : m_socket(move(socket))
    , m_handle_broker_channel(get_random<HandleBrokerChannel>())
{
}

TransportSocketWindows::~TransportSocketWindows() = default;

void TransportSocketWindows::set_up_read_hook(Function<void()> hook)
{
    VERIFY(m_socket->is_open());
    m_socket->on_ready_to_read = move(hook);
}

bool TransportSocketWindows::is_open() const
{
    return m_socket_is_open.load(AK::MemoryOrder::memory_order_relaxed);
}

void TransportSocketWindows::close()
{
    m_socket_is_open.store(false, AK::MemoryOrder::memory_order_relaxed);
    MutexLocker locker(m_send_mutex);
    m_socket->close();
}

void TransportSocketWindows::close_after_sending_all_pending_messages()
{
    close();
}

void TransportSocketWindows::wait_until_readable()
{
    auto readable = MUST(m_socket->can_read_without_blocking(-1));
    VERIFY(readable);
}

// Maximum size of accumulated unprocessed bytes before we disconnect the peer
static constexpr size_t MAX_UNPROCESSED_BUFFER_SIZE = 128 * MiB;

ErrorOr<void> TransportSocketWindows::post_message(MessageDataType bytes, Vector<Attachment>& attachments)
{
    VERIFY(bytes.size() <= MAX_MESSAGE_PAYLOAD_SIZE);
    VERIFY(attachments.size() <= MAX_MESSAGE_FD_COUNT);

    MutexLocker locker(m_send_mutex);

    MessageHeader header {
        .payload_size = static_cast<u32>(bytes.size()),
        .attachment_count = static_cast<u32>(attachments.size()),
    };
    HandleBrokerToken attachment_token = 0;
    if (!attachments.is_empty()) {
        attachment_token = TRY(HandleBroker::the().park(m_handle_broker_channel, move(attachments)));
        attachments.clear();
        m_has_parked_attachments = true;
    }

    Vector<u8> message_buffer;
    TRY(message_buffer.try_resize(sizeof(MessageHeader) + header.token_size() + bytes.size()));
    memcpy(message_buffer.data(), &header, sizeof(header));
    if (header.token_size() > 0)
        memcpy(message_buffer.data() + sizeof(MessageHeader), &attachment_token, sizeof(attachment_token));
    if (!bytes.is_empty())
        memcpy(message_buffer.data() + sizeof(MessageHeader) + header.token_size(), bytes.data(), bytes.size());

    TRY(transfer(message_buffer.span()));
    return {};
}

ErrorOr<void> TransportSocketWindows::transfer(ReadonlyBytes bytes_to_write)
{
    // FIXME: The timeout to re-check the open flag will not be needed once sends are moved to an IO thread that
    //        teardown stops, as TransportSocket does.
    static constexpr int SEND_POLL_TIMEOUT_MS = 100;

    while (!bytes_to_write.is_empty()) {
        if (!is_open())
            return Error::from_string_literal("Transport closed during send");

        ErrorOr<size_t> maybe_nwritten = m_socket->write_some(bytes_to_write);

        if (maybe_nwritten.is_error()) {
            auto error = maybe_nwritten.release_error();
            if (error.code() != EWOULDBLOCK)
                return error;
            if (m_disconnects_when_peer_stops_reading)
                return Error::from_string_literal("Peer has stopped reading");

            struct pollfd pollfd = {
                .fd = static_cast<SOCKET>(m_socket->fd().value()),
                .events = POLLOUT,
                .revents = 0
            };

            auto result = WSAPoll(&pollfd, 1, SEND_POLL_TIMEOUT_MS);
            if (result == 1 || result == 0)
                continue;
            if (result == SOCKET_ERROR)
                return Error::from_windows_error();
            dbgln("TransportSocketWindows::transfer: Unexpected WSAPoll result {}", result);
            return Error::from_string_literal("Unexpected WSAPoll result");
        }

        bytes_to_write = bytes_to_write.slice(maybe_nwritten.value());
    }
    return {};
}

TransportSocketWindows::ShouldShutdown TransportSocketWindows::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    auto should_shutdown = ShouldShutdown::No;

    while (is_open()) {

        u8 buffer[4096];
        auto maybe_bytes_read = m_socket->read_without_waiting({ buffer, sizeof(buffer) });

        if (maybe_bytes_read.is_error()) {
            auto error = maybe_bytes_read.release_error();
            if (error.code() == EWOULDBLOCK)
                break;
            if (error.code() == ECONNRESET) {
                should_shutdown = ShouldShutdown::Yes;
                break;
            }
            dbgln("TransportSocketWindows::read_as_many_messages_as_possible_without_blocking: {}", error);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        auto bytes_read = maybe_bytes_read.release_value();
        if (bytes_read.is_empty()) {
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        if (m_unprocessed_bytes.size() + bytes_read.size() > MAX_UNPROCESSED_BUFFER_SIZE) {
            dbgln("TransportSocketWindows: Unprocessed buffer would exceed {} bytes, disconnecting peer", MAX_UNPROCESSED_BUFFER_SIZE);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        if (m_unprocessed_bytes.try_append(bytes_read.data(), bytes_read.size()).is_error()) {
            dbgln("TransportSocketWindows: Failed to append to unprocessed_bytes buffer");
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
    }

    size_t index = 0;
    while (index + sizeof(MessageHeader) <= m_unprocessed_bytes.size()) {
        MessageHeader header;
        memcpy(&header, m_unprocessed_bytes.data() + index, sizeof(MessageHeader));
        if (header.payload_size > MAX_MESSAGE_PAYLOAD_SIZE) {
            dbgln("TransportSocketWindows: Rejecting message with payload_size {} exceeding limit {}", header.payload_size, MAX_MESSAGE_PAYLOAD_SIZE);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        if (header.attachment_count > MAX_MESSAGE_FD_COUNT) {
            dbgln("TransportSocketWindows: Rejecting message with attachment_count {} exceeding limit {}", header.attachment_count, MAX_MESSAGE_FD_COUNT);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }

        Checked<size_t> message_size = header.payload_size;
        message_size += sizeof(MessageHeader);
        message_size += header.token_size();
        if (message_size.has_overflow() || message_size.value() > m_unprocessed_bytes.size() - index)
            break;

        Message message;
        if (header.attachment_count > 0) {
            HandleBrokerToken attachment_token = 0;
            memcpy(&attachment_token, m_unprocessed_bytes.data() + index + sizeof(MessageHeader), sizeof(attachment_token));
            auto attachments = HandleBroker::the().redeem(attachment_token);
            if (attachments.is_error() || attachments.value().size() != header.attachment_count) {
                dbgln("TransportSocketWindows: Rejecting message whose attachments the handle broker does not hold");
                should_shutdown = ShouldShutdown::Yes;
                break;
            }
            for (auto& attachment : attachments.value())
                message.attachments.enqueue(move(attachment));
        }

        auto const* payload = m_unprocessed_bytes.data() + index + sizeof(MessageHeader) + header.token_size();
        Vector<u8> payload_bytes;
        if (payload_bytes.try_append(payload, header.payload_size).is_error()) {
            dbgln("TransportSocketWindows: Failed to allocate message buffer for payload_size {}", header.payload_size);
            should_shutdown = ShouldShutdown::Yes;
            break;
        }
        message.bytes = ReceivedMessageBytes::from_vector(move(payload_bytes));
        callback(move(message));
        index = message_size.value() + index;
    }

    if (index < m_unprocessed_bytes.size()) {
        auto remaining_bytes_or_error = ByteBuffer::copy(m_unprocessed_bytes.span().slice(index));
        if (remaining_bytes_or_error.is_error()) {
            dbgln("TransportSocketWindows: Failed to copy remaining bytes");
            should_shutdown = ShouldShutdown::Yes;
        } else {
            m_unprocessed_bytes = remaining_bytes_or_error.release_value();
        }
    } else {
        m_unprocessed_bytes.clear();
    }

    // Nothing the peer has not read by now will ever be read, so the handles that were waiting for it can go.
    if (should_shutdown == ShouldShutdown::Yes && m_has_parked_attachments) {
        HandleBroker::the().forget_channel(m_handle_broker_channel);
        m_has_parked_attachments = false;
    }

    return should_shutdown;
}

ErrorOr<TransportHandle> TransportSocketWindows::release_for_transfer()
{
    m_socket_is_open.store(false, AK::MemoryOrder::memory_order_relaxed);
    MutexLocker locker(m_send_mutex);
    auto fd = TRY(m_socket->release_fd());
    return TransportHandle { File::adopt_fd(fd) };
}

}
