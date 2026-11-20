/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibCore/MachPort.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibIPC/RawAttachment.h>
#include <LibIPC/RustFFI.h>
#include <LibIPC/TransportMachPort.h>

namespace IPC {

ErrorOr<TransportMachPort::Paired> TransportMachPort::create_paired()
{
    auto port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    auto port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    // The peer may receive messages before it has constructed its transport.
    raise_receive_queue_limit(port_b_recv);

    return Paired {
        make<TransportMachPort>(move(port_a_recv), move(port_b_send)),
        TransportHandle { move(port_b_recv), move(port_a_send) },
    };
}

void TransportMachPort::raise_receive_queue_limit(Core::MachPort const& receive_right)
{
    FFI::ipc_mach_raise_receive_queue_limit(receive_right.port());
}

TransportMachPort::TransportMachPort(Core::MachPort receive_right, Core::MachPort send_right)
{
    // NB: The transport has the pipe from the start, so that what arrives before set_up_read_hook() still leaves a byte
    //     in it.
    auto fds = MUST(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
    m_notify_hook_read_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[0]));
    m_transport = FFI::ipc_mach_transport_create(receive_right.release(), send_right.release(), fds[1]);
    VERIFY(m_transport);
}

TransportMachPort::~TransportMachPort()
{
    // NB: Destroying the transport releases the observer, after which nothing writes to the pipe.
    FFI::ipc_mach_transport_destroy(m_transport);
    m_read_hook_notifier.clear();
}

void TransportMachPort::set_up_read_hook(Function<void()> hook)
{
    m_on_read_hook = move(hook);
    m_read_hook_notifier = Core::Notifier::construct(m_notify_hook_read_fd->value(), Core::NotificationType::Read);
    m_read_hook_notifier->on_activation = [this] {
        char buf[64];
        (void)Core::System::read(m_notify_hook_read_fd->value(), { buf, sizeof(buf) });
        FFI::ipc_mach_transport_acknowledge_read_notification(m_transport);
        if (m_on_read_hook)
            m_on_read_hook();
    };
    FFI::ipc_mach_transport_schedule_read_notification_if_readable(m_transport);
}

void TransportMachPort::set_incoming_message_observer(Function<void(ReadonlyBytes)> observer)
{
    if (!observer) {
        FFI::ipc_mach_transport_set_incoming_message_observer(m_transport, nullptr);
        return;
    }
    // NB: The transport owns the observer, and releases the one it replaces once that no longer runs.
    FFI::CppIncomingMessageObserver const owned_observer {
        .context = new Function<void(ReadonlyBytes)>(move(observer)),
        .observe = [](void* observer, ReadonlyBytes bytes) { (*static_cast<Function<void(ReadonlyBytes)>*>(observer))(bytes); },
        .release = [](void* observer) { delete static_cast<Function<void(ReadonlyBytes)>*>(observer); },
    };
    FFI::ipc_mach_transport_set_incoming_message_observer(m_transport, &owned_observer);
}

bool TransportMachPort::is_open() const
{
    return FFI::ipc_mach_transport_is_open(m_transport);
}

void TransportMachPort::close()
{
    FFI::ipc_mach_transport_close(m_transport);
}

void TransportMachPort::close_after_sending_all_pending_messages()
{
    FFI::ipc_mach_transport_close_after_sending_all_pending_messages(m_transport);
}

void TransportMachPort::wait_until_readable(Optional<MonotonicTime> deadline)
{
    // NB: Rust checks the deadline on the clock MonotonicTime reads, which keeps counting while the system sleeps.
    FFI::ipc_mach_transport_wait_until_readable(m_transport, deadline.has_value(), deadline.has_value() ? deadline->nanoseconds() : 0);
}

void TransportMachPort::interrupt_wait_until_readable()
{
    FFI::ipc_mach_transport_interrupt_wait_until_readable(m_transport);
}

void TransportMachPort::flush()
{
    FFI::ipc_mach_transport_flush(m_transport);
}

void TransportMachPort::wait_until_incoming_is_current()
{
    FFI::ipc_mach_transport_wait_until_incoming_is_current(m_transport);
}

ErrorOr<void> TransportMachPort::post_message(MessageDataType bytes, Vector<Attachment>& attachments)
{
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
    // NB: The transport refuses a message once a stop is asked for or a send has failed, which is_open() reports too.
    if (!FFI::ipc_mach_transport_post(m_transport, bytes.span(), raw_attachments.span(), &bytes, hand_over_payload))
        return Error::from_string_literal("TransportMachPort: The transport takes no more messages");
    return {};
}

TransportMachPort::ShouldShutdown TransportMachPort::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    auto take_message = [](void* callback, ReadonlyBytes bytes, FFI::ReceivedPayload* payload, ReadonlySpan<RawAttachment> raw_attachments) {
        Message message;
        message.bytes = ReceivedMessageBytes::adopt_foreign_storage(bytes, payload, [](void* payload) {
            FFI::ipc_mach_received_payload_destroy(static_cast<FFI::ReceivedPayload*>(payload));
        });
        for (auto& attachment : adopt_raw_attachments(raw_attachments))
            message.attachments.enqueue(move(attachment));
        (*static_cast<Function<void(Message&&)>*>(callback))(move(message));
    };
    auto peer_eof = FFI::ipc_mach_transport_take_incoming(m_transport, &callback, take_message);
    return peer_eof ? ShouldShutdown::Yes : ShouldShutdown::No;
}

ErrorOr<TransportHandle> TransportMachPort::release_for_transfer()
{
    mach_port_t receive_right = MACH_PORT_NULL;
    mach_port_t send_right = MACH_PORT_NULL;
    if (!FFI::ipc_mach_transport_release_for_transfer(m_transport, &receive_right, &send_right))
        return Error::from_string_literal("TransportMachPort: The transport was already handed over");
    return TransportHandle {
        Core::MachPort::adopt_right(receive_right, Core::MachPort::PortRight::Receive),
        Core::MachPort::adopt_right(send_right, Core::MachPort::PortRight::Send),
    };
}

}
