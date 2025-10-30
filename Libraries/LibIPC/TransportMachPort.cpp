/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NonnullOwnPtr.h>
#include <LibCore/MachPort.h>
#include <LibCore/Notifier.h>
#include <LibCore/System.h>
#include <LibIPC/TransportMachPort.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Thread.h>

#include <mach/mach.h>

namespace IPC {

static void set_mach_port_queue_limit(mach_port_t port)
{
    mach_port_limits_t limits { .mpl_qlimit = MACH_PORT_QLIMIT_MAX };
    // Mach receive rights start with a small queue. Raise it so short bursts can wait in the kernel if this
    // transport thread falls behind for a moment.
    mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO,
        reinterpret_cast<mach_port_info_t>(&limits), MACH_PORT_LIMITS_INFO_COUNT);
}

static Attachment attachment_from_descriptor(mach_msg_port_descriptor_t const& descriptor)
{
    VERIFY(descriptor.type == MACH_MSG_PORT_DESCRIPTOR);

    switch (descriptor.disposition) {
    case MACH_MSG_TYPE_MOVE_SEND:
        return Attachment::from_mach_port(
            Core::MachPort::adopt_right(descriptor.name, Core::MachPort::PortRight::Send),
            Core::MachPort::MessageRight::MoveSend);
    case MACH_MSG_TYPE_MOVE_RECEIVE:
        return Attachment::from_mach_port(
            Core::MachPort::adopt_right(descriptor.name, Core::MachPort::PortRight::Receive),
            Core::MachPort::MessageRight::MoveReceive);
    case MACH_MSG_TYPE_MOVE_SEND_ONCE:
        return Attachment::from_mach_port(
            Core::MachPort::adopt_right(descriptor.name, Core::MachPort::PortRight::SendOnce),
            Core::MachPort::MessageRight::MoveSendOnce);
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<TransportMachPort::Paired> TransportMachPort::create_paired()
{
    auto port_a_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_a_send = TRY(port_a_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    auto port_b_recv = TRY(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    auto port_b_send = TRY(port_b_recv.insert_right(Core::MachPort::MessageRight::MakeSend));

    return Paired {
        make<TransportMachPort>(move(port_a_recv), move(port_b_send)),
        TransportHandle { move(port_b_recv), move(port_a_send) },
    };
}

TransportMachPort::TransportMachPort(Core::MachPort receive_right, Core::MachPort send_right)
    : m_receive_port(move(receive_right))
    , m_send_port(move(send_right))
{
    m_port_set = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::PortSet));
    // This thread waits on a port set, not one port. Add the main receive port so the same wait can handle
    // messages from the peer.
    auto ret = mach_port_insert_member(mach_task_self(), m_receive_port.port(), m_port_set.port());
    VERIFY(ret == KERN_SUCCESS);

    m_wakeup_receive_port = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
    m_wakeup_send_port = MUST(m_wakeup_receive_port.insert_right(Core::MachPort::MessageRight::MakeSend));
    // Add the private wakeup port to the same set so queued sends can wake the blocking mach_msg() call.
    ret = mach_port_insert_member(mach_task_self(), m_wakeup_receive_port.port(), m_port_set.port());
    VERIFY(ret == KERN_SUCCESS);

    auto fds = MUST(Core::System::pipe2(O_CLOEXEC | O_NONBLOCK));
    m_notify_hook_read_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[0]));
    m_notify_hook_write_fd = adopt_ref(*new AutoCloseFileDescriptor(fds[1]));

    set_mach_port_queue_limit(m_receive_port.port());

    mach_port_t prev = MACH_PORT_NULL;
    // Ask the kernel to send MACH_NOTIFY_NO_SENDERS to our receive port when the peer loses its last send
    // right. This is how the transport notices that the peer went away.
    mach_port_request_notification(mach_task_self(),
        m_receive_port.port(),
        MACH_NOTIFY_NO_SENDERS,
        0,
        m_receive_port.port(),
        MACH_MSG_TYPE_MAKE_SEND_ONCE,
        &prev);
    if (MACH_PORT_VALID(prev)) {
        // If an older notification right was replaced, mach_port_request_notification returns it in `prev`.
        // Drop it here so we do not leak that stale send-once right.
        mach_port_deallocate(mach_task_self(), prev);
    }

    m_io_thread = Threading::Thread::construct("IPC IO (Mach)"sv, [this] { return io_thread_loop(); });
    m_io_thread->start();
}

TransportMachPort::~TransportMachPort()
{
    stop_io_thread(IOThreadState::Stopped);
    m_read_hook_notifier.clear();
}

void TransportMachPort::stop_io_thread(IOThreadState desired_state)
{
    m_io_thread_state.store(desired_state, AK::MemoryOrder::memory_order_release);
    wake_io_thread();
    if (m_io_thread && m_io_thread->needs_to_be_joined())
        (void)m_io_thread->join();
}

void TransportMachPort::wake_io_thread()
{
    if (!MACH_PORT_VALID(m_wakeup_send_port.port()))
        return;

    mach_msg_header_t header {};
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    header.msgh_size = sizeof(header);
    header.msgh_remote_port = m_wakeup_send_port.port();
    header.msgh_local_port = MACH_PORT_NULL;
    header.msgh_id = IPC_WAKEUP_MESSAGE_ID;

    // Send a header-only wakeup message to the private wakeup port. Because that port is in the same port set,
    // it breaks the blocking mach_msg() call so the thread can flush queued sends.
    mach_msg(&header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(header), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
}

void TransportMachPort::notify_read_available()
{
    if (!m_notify_hook_write_fd)
        return;

    Array<u8, 1> bytes = { 0 };
    (void)Core::System::write(m_notify_hook_write_fd->value(), bytes);
}

void TransportMachPort::mark_peer_eof()
{
    {
        Sync::MutexLocker locker(m_incoming_mutex);
        m_peer_eof = true;
    }
    m_incoming_cv.broadcast();
    notify_read_available();
}

intptr_t TransportMachPort::io_thread_loop()
{
    static constexpr size_t RECV_BUFFER_SIZE = 65536;
    auto buffer = Vector<u8>();
    buffer.resize(RECV_BUFFER_SIZE);

    for (;;) {
        if (m_io_thread_state.load() == IOThreadState::Stopped)
            break;

        Vector<PendingMessage> messages_to_send;
        {
            Sync::MutexLocker locker(m_send_mutex);
            messages_to_send = move(m_pending_send_messages);
        }
        for (auto& message : messages_to_send)
            send_mach_message(message);

        if (m_io_thread_state.load() == IOThreadState::SendPendingMessagesAndStop) {
            Sync::MutexLocker locker(m_send_mutex);
            if (!m_pending_send_messages.is_empty())
                continue;
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        auto* header = reinterpret_cast<mach_msg_header_t*>(buffer.data());
        // Wait on the whole port set so one loop can handle peer messages, internal wakeups, and kernel
        // notifications like MACH_NOTIFY_NO_SENDERS.
        auto const ret = mach_msg(header, MACH_RCV_MSG | MACH_RCV_LARGE, 0, buffer.size(),
            m_port_set.port(), MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);

        if (ret == MACH_RCV_TOO_LARGE) {
            auto needed_size = header->msgh_size + sizeof(mach_msg_trailer_t);
            buffer.resize(needed_size);
            header = reinterpret_cast<mach_msg_header_t*>(buffer.data());
            // MACH_RCV_LARGE tells us the needed size without removing the message. Resize the buffer and try
            // again so large messages, including large attachment batches, still work.
            auto const retry_ret = mach_msg(header, MACH_RCV_MSG, 0, needed_size,
                m_port_set.port(), MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
            if (retry_ret != KERN_SUCCESS) {
                dbgln("TransportMachPort: mach_msg retry failed: {}", mach_error_string(retry_ret));
                m_io_thread_state = IOThreadState::Stopped;
                break;
            }
        } else if (ret != KERN_SUCCESS) {
            dbgln("TransportMachPort: mach_msg receive failed: {}", mach_error_string(ret));
            m_io_thread_state = IOThreadState::Stopped;
            break;
        }

        if (header->msgh_local_port == m_wakeup_receive_port.port()) {
            VERIFY(header->msgh_id == IPC_WAKEUP_MESSAGE_ID);
            continue;
        }

        VERIFY(header->msgh_local_port == m_receive_port.port());

        switch (header->msgh_id) {
        case MACH_NOTIFY_NO_SENDERS:
            mark_peer_eof();
            continue;
        case MACH_NOTIFY_SEND_ONCE:
            continue;
        case IPC_DATA_MESSAGE_ID:
            process_received_message(buffer.data());
            continue;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    VERIFY(m_io_thread_state == IOThreadState::Stopped);
    // Stopping for transfer tears down the old endpoint on purpose. Do not surface that as peer EOF;
    // the receive and send rights are about to be adopted by the new transport owner.
    if (!m_is_being_transferred.load(AK::MemoryOrder::memory_order_acquire))
        mark_peer_eof();
    return 0;
}

void TransportMachPort::send_mach_message(PendingMessage& msg)
{
    auto const& bytes = msg.bytes;
    auto& attachments = msg.attachments;
    size_t port_count = attachments.size();
    size_t total_desc_count = port_count + 1; // +1 for OOL payload

    size_t msg_size = sizeof(mach_msg_header_t)
        + sizeof(mach_msg_body_t)
        + (port_count * sizeof(mach_msg_port_descriptor_t))
        + sizeof(mach_msg_ool_descriptor_t);

    if (m_send_buffer.size() < msg_size)
        m_send_buffer.resize(msg_size);

    auto* header = reinterpret_cast<mach_msg_header_t*>(m_send_buffer.data());
    header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
    header->msgh_size = msg_size;
    header->msgh_remote_port = m_send_port.port();
    header->msgh_local_port = MACH_PORT_NULL;
    header->msgh_id = IPC_DATA_MESSAGE_ID;

    auto* body = reinterpret_cast<mach_msg_body_t*>(header + 1);
    body->msgh_descriptor_count = total_desc_count;

    auto* desc_ptr = reinterpret_cast<mach_msg_port_descriptor_t*>(body + 1);
    for (size_t i = 0; i < port_count; ++i) {
        auto disposition = static_cast<mach_msg_type_name_t>(attachments[i].message_right());
        auto port = attachments[i].release_mach_port();
        desc_ptr[i].name = port.release();
        desc_ptr[i].disposition = disposition;
        desc_ptr[i].type = MACH_MSG_PORT_DESCRIPTOR;
    }

    auto* ool_desc = reinterpret_cast<mach_msg_ool_descriptor_t*>(&desc_ptr[port_count]);
    ool_desc->address = const_cast<void*>(static_cast<void const*>(bytes.data()));
    ool_desc->size = bytes.size();
    ool_desc->deallocate = false;
    ool_desc->copy = MACH_MSG_VIRTUAL_COPY;
    ool_desc->type = MACH_MSG_OOL_DESCRIPTOR;

    // Send one complex Mach message: port descriptors for attachments and one out-of-line region for the byte
    // payload. This keeps right transfer atomic and lets the kernel use virtual-copy for the payload.
    auto const ret = mach_msg(header, MACH_SEND_MSG, msg_size, 0, MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    if (ret != KERN_SUCCESS) {
        dbgln("TransportMachPort: send failed: {} (send_port={:x})", mach_error_string(ret), m_send_port.port());
        mark_peer_eof();
    }
}

void TransportMachPort::process_received_message(u8* buffer)
{
    auto* header = reinterpret_cast<mach_msg_header_t*>(buffer);
    VERIFY(header->msgh_bits & MACH_MSGH_BITS_COMPLEX);
    auto* body = reinterpret_cast<mach_msg_body_t*>(header + 1);
    VERIFY(body->msgh_descriptor_count > 0);
    auto attachment_count = body->msgh_descriptor_count - 1;
    auto* descriptors = reinterpret_cast<mach_msg_port_descriptor_t*>(body + 1);
    auto const* payload = reinterpret_cast<mach_msg_ool_descriptor_t const*>(&descriptors[attachment_count]);

    auto message = make<Message>();
    for (unsigned int i = 0; i < attachment_count; ++i)
        message->attachments.enqueue(attachment_from_descriptor(descriptors[i]));

    VERIFY(payload->type == MACH_MSG_OOL_DESCRIPTOR);
    if (payload->size > 0) {
        message->bytes.append(static_cast<u8 const*>(payload->address), payload->size);
        // The out-of-line payload arrives as a temporary mapping in this task. After copying it into our queue,
        // unmap that region so each receive does not leak virtual memory.
        vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(payload->address), payload->size);
    }

    if (message->bytes.is_empty() && message->attachments.is_empty())
        return;

    {
        Sync::MutexLocker locker(m_incoming_mutex);
        m_incoming_messages.append(move(message));
    }
    m_incoming_cv.signal();
    notify_read_available();
}

void TransportMachPort::set_up_read_hook(Function<void()> hook)
{
    m_on_read_hook = move(hook);
    m_read_hook_notifier = Core::Notifier::construct(m_notify_hook_read_fd->value(), Core::NotificationType::Read);
    m_read_hook_notifier->on_activation = [this] {
        char buf[64];
        (void)Core::System::read(m_notify_hook_read_fd->value(), { buf, sizeof(buf) });
        if (m_on_read_hook)
            m_on_read_hook();
    };

    {
        Sync::MutexLocker locker(m_incoming_mutex);
        if (!m_incoming_messages.is_empty())
            notify_read_available();
    }
}

bool TransportMachPort::is_open() const
{
    return m_is_open && !m_peer_eof;
}

void TransportMachPort::close()
{
    m_is_open = false;
    stop_io_thread(IOThreadState::Stopped);
}

void TransportMachPort::close_after_sending_all_pending_messages()
{
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    m_is_open = false;
}

void TransportMachPort::wait_until_readable()
{
    Sync::MutexLocker lock(m_incoming_mutex);
    while (m_incoming_messages.is_empty() && !m_peer_eof)
        m_incoming_cv.wait();
}

void TransportMachPort::post_message(Vector<u8> const& bytes, Vector<Attachment>& attachments)
{
    {
        Sync::MutexLocker locker(m_send_mutex);
        m_pending_send_messages.append(PendingMessage { bytes, move(attachments) });
    }
    wake_io_thread();
}

TransportMachPort::ShouldShutdown TransportMachPort::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    Vector<NonnullOwnPtr<Message>> messages;
    {
        Sync::MutexLocker locker(m_incoming_mutex);
        messages = move(m_incoming_messages);
    }
    for (auto& message : messages)
        callback(move(*message));
    return m_peer_eof ? ShouldShutdown::Yes : ShouldShutdown::No;
}

ErrorOr<TransportHandle> TransportMachPort::release_for_transfer()
{
    // From this point on, shutdown of the old endpoint is part of handing the transport to another
    // MessagePort, not evidence that the peer disconnected.
    m_is_being_transferred.store(true, AK::MemoryOrder::memory_order_release);
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    m_is_open = false;

    mach_port_t prev = MACH_PORT_NULL;
    // Remove the no-senders registration before giving this receive right to another owner. Otherwise the old
    // transport could still get disconnect notifications for a port it no longer owns.
    auto ret = mach_port_request_notification(mach_task_self(),
        m_receive_port.port(),
        MACH_NOTIFY_NO_SENDERS,
        0,
        MACH_PORT_NULL,
        MACH_MSG_TYPE_MAKE_SEND_ONCE,
        &prev);
    VERIFY(ret == KERN_SUCCESS);
    if (MACH_PORT_VALID(prev))
        // Removing the registration still returns the old send-once notification right in `prev`.
        // Release it so the transferred port does not keep leftover notification state.
        mach_port_deallocate(mach_task_self(), prev);

    // Take the receive right out of this transport's port set before transfer. The next owner should choose its
    // own wait set, if it needs one.
    ret = mach_port_extract_member(mach_task_self(), m_receive_port.port(), m_port_set.port());
    VERIFY(ret == KERN_SUCCESS);

    return TransportHandle { move(m_receive_port), move(m_send_port) };
}

}
