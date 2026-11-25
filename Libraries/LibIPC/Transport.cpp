/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AllOf.h>
#include <AK/NonnullOwnPtr.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibIPC/File.h>
#include <LibIPC/RawAttachment.h>
#include <LibIPC/RustFFI.h>
#include <LibIPC/Transport.h>

namespace IPC {

struct ReadNotificationPipe {
    File read_end;
    int write_end { -1 };
};

// NB: The transport has the pipe from the start, so that what arrives before set_up_read_hook() still leaves a byte in
//     it.
static ErrorOr<ReadNotificationPipe> create_read_notification_pipe()
{
    auto fds = TRY(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
    return ReadNotificationPipe { File::adopt_fd(fds[0]), fds[1] };
}

#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)

ErrorOr<Transport::Paired> Transport::create_paired()
{
    auto port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    auto port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    // The peer may receive messages before it has constructed its transport.
    raise_receive_queue_limit(port_b_recv);

    return Paired {
        TRY(create(move(port_a_recv), move(port_b_send))),
        TransportHandle { move(port_b_recv), move(port_a_send) },
    };
}

ErrorOr<NonnullOwnPtr<Transport>> Transport::create(Core::MachPort receive_right, Core::MachPort send_right)
{
    auto pipe = TRY(create_read_notification_pipe());
    auto* transport = FFI::ipc_transport_create_from_mach_ports(receive_right.release(), send_right.release(), pipe.write_end);
    if (!transport)
        return Error::from_string_literal("Transport: Unable to take over the ports");
    return adopt_own(*new Transport(transport, move(pipe.read_end)));
}

void Transport::raise_receive_queue_limit(Core::MachPort const& receive_right)
{
    FFI::ipc_mach_raise_receive_queue_limit(receive_right.port());
}

ErrorOr<TransportHandle> Transport::release_for_transfer()
{
    mach_port_t receive_right = MACH_PORT_NULL;
    mach_port_t send_right = MACH_PORT_NULL;
    if (!FFI::ipc_transport_release_mach_ports_for_transfer(m_transport, &receive_right, &send_right))
        return Error::from_string_literal("Transport: The transport was already handed over");
    return TransportHandle {
        Core::MachPort::adopt_right(receive_right, Core::MachPort::PortRight::Receive),
        Core::MachPort::adopt_right(send_right, Core::MachPort::PortRight::Send),
    };
}

#else

ErrorOr<Transport::Paired> Transport::create_paired()
{
    int fds[2] {};
    // NB: Close-on-exec from the start. Another thread could spawn a process between creating the pair and marking it.
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC, 0, fds));
    auto remote_handle = TransportHandle { File::adopt_fd(fds[1]) };
    return Paired {
        TRY(adopt_fd(fds[0])),
        move(remote_handle),
    };
}

ErrorOr<NonnullOwnPtr<Transport>> Transport::from_socket(NonnullOwnPtr<Core::LocalSocket> socket)
{
    // NB: The transport watches the socket itself. A notifier of the socket's own would outlive the descriptor in the Qt
    //     UI, which defers destroying a QSocketNotifier, and warns about one whose descriptor was closed.
    socket->set_notifications_enabled(false);
    return adopt_fd(TRY(socket->release_fd()));
}

static bool all_descriptors_are_valid(Vector<Attachment> const& attachments)
{
    return all_of(attachments, [](auto const& attachment) { return attachment.fd() >= 0; });
}

ErrorOr<NonnullOwnPtr<Transport>> Transport::adopt_fd(int fd, ByteBuffer unprocessed_bytes, Vector<Attachment> unprocessed_attachments)
{
    auto socket = File::adopt_fd(fd);
    if (socket.fd() < 0 || !all_descriptors_are_valid(unprocessed_attachments))
        return Error::from_errno(EBADF);
    // NB: The Rust transport takes over the socket and the unprocessed attachments even when it can't start.
    auto pipe = TRY(create_read_notification_pipe());
    auto raw_attachments = release_raw_attachments(move(unprocessed_attachments));
    auto* transport = FFI::ipc_transport_create_from_socket(socket.take_fd(), pipe.write_end, unprocessed_bytes.bytes(), raw_attachments.span());
    if (!transport)
        return Error::from_string_literal("Transport: Unable to take over the socket");
    return adopt_own(*new Transport(transport, move(pipe.read_end)));
}

ErrorOr<TransportHandle> Transport::release_for_transfer()
{
    Optional<TransportHandle> handle;
    auto take_handle = [](void* handle, int socket_fd, ReadonlyBytes unprocessed_bytes, ReadonlySpan<RawAttachment> unprocessed_attachments) {
        *static_cast<Optional<TransportHandle>*>(handle) = TransportHandle {
            File::adopt_fd(socket_fd),
            MUST(ByteBuffer::copy(unprocessed_bytes)),
            adopt_raw_attachments(unprocessed_attachments),
        };
    };
    if (!FFI::ipc_transport_release_socket_for_transfer(m_transport, &handle, take_handle))
        return Error::from_string_literal("Transport: The transport was already handed over");
    return handle.release_value();
}

#endif

Transport::Transport(FFI::RustTransport* transport, File notify_hook_read_fd)
    : m_transport(transport)
    , m_notify_hook_read_fd(move(notify_hook_read_fd))
{
}

Transport::~Transport()
{
    // NB: Destroying the transport releases the observer, after which nothing writes to the pipe.
    FFI::ipc_transport_destroy(m_transport);
    m_read_hook_notifier.clear();
}

void Transport::set_up_read_hook(Function<void()> hook)
{
    m_on_read_hook = move(hook);
    m_read_hook_notifier = Core::Notifier::construct(m_notify_hook_read_fd.fd(), Core::NotificationType::Read);
    m_read_hook_notifier->on_activation = [this] {
        char buf[64];
        (void)Core::System::read(m_notify_hook_read_fd.fd(), { buf, sizeof(buf) });
        FFI::ipc_transport_acknowledge_read_notification(m_transport);
        if (m_on_read_hook)
            m_on_read_hook();
    };
    FFI::ipc_transport_schedule_read_notification_if_readable(m_transport);
}

void Transport::set_incoming_message_observer(Function<void(ReadonlyBytes)> observer)
{
    if (!observer) {
        FFI::ipc_transport_set_incoming_message_observer(m_transport, nullptr);
        return;
    }
    // NB: The transport owns the observer, and releases the one it replaces once that no longer runs.
    FFI::CppIncomingMessageObserver const owned_observer {
        .context = new Function<void(ReadonlyBytes)>(move(observer)),
        .observe = [](void* observer, ReadonlyBytes bytes) { (*static_cast<Function<void(ReadonlyBytes)>*>(observer))(bytes); },
        .release = [](void* observer) { delete static_cast<Function<void(ReadonlyBytes)>*>(observer); },
    };
    FFI::ipc_transport_set_incoming_message_observer(m_transport, &owned_observer);
}

bool Transport::is_open() const
{
    return FFI::ipc_transport_is_open(m_transport);
}

void Transport::close()
{
    FFI::ipc_transport_close(m_transport);
}

void Transport::close_after_sending_all_pending_messages()
{
    FFI::ipc_transport_close_after_sending_all_pending_messages(m_transport);
}

void Transport::wait_until_readable(Optional<MonotonicTime> deadline)
{
    // NB: Rust checks the deadline on the clock MonotonicTime reads, which keeps counting while the system sleeps.
    FFI::ipc_transport_wait_until_readable(m_transport, deadline.has_value(), deadline.has_value() ? deadline->nanoseconds() : 0);
}

void Transport::interrupt_wait_until_readable()
{
    FFI::ipc_transport_interrupt_wait_until_readable(m_transport);
}

void Transport::flush()
{
    FFI::ipc_transport_flush(m_transport);
}

void Transport::wait_until_incoming_is_current()
{
    FFI::ipc_transport_wait_until_incoming_is_current(m_transport);
}

ErrorOr<void> Transport::post_message(MessageDataType bytes, Vector<Attachment>& attachments)
{
#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS)
    // NB: Every descriptor goes to the Rust transport, which can't hold an invalid one.
    if (!all_descriptors_are_valid(attachments))
        return Error::from_errno(EBADF);
#endif
    auto raw_attachments = release_raw_attachments(move(attachments));
    // NB: A message that has to wait behind others keeps the buffer it was encoded into, rather than a copy.
    auto hand_over_payload = [](void* bytes) -> FFI::HandedOverPayload {
        auto* payload = new MessageDataType(move(*static_cast<MessageDataType*>(bytes)));
        return {
            .data = payload->data(),
            .size = payload->size(),
            .owner = payload,
            .release = [](void* payload) { delete static_cast<MessageDataType*>(payload); },
        };
    };
    // NB: The transport refuses a message once it no longer counts as open, and one it can't prepare for sending, as a
    //     payload whose shared memory it can't create.
    if (!FFI::ipc_transport_post(m_transport, bytes.span(), raw_attachments.span(), &bytes, hand_over_payload))
        return Error::from_string_literal("Transport: The transport refused the message");
    return {};
}

Transport::ShouldShutdown Transport::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    auto take_message = [](void* callback, ReadonlyBytes bytes, FFI::ReceivedPayload* payload, ReadonlySpan<RawAttachment> raw_attachments) {
        Message message;
        message.bytes = ReceivedMessageBytes::adopt_foreign_storage(bytes, payload, [](void* payload) {
            FFI::ipc_received_payload_destroy(static_cast<FFI::ReceivedPayload*>(payload));
        });
        for (auto& attachment : adopt_raw_attachments(raw_attachments))
            message.attachments.enqueue(move(attachment));
        (*static_cast<Function<void(Message&&)>*>(callback))(move(message));
    };
    auto peer_eof = FFI::ipc_transport_take_incoming(m_transport, &callback, take_message);
    return peer_eof ? ShouldShutdown::Yes : ShouldShutdown::No;
}

}
