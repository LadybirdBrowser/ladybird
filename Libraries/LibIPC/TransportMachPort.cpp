/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NumericLimits.h>
#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <AK/kmalloc.h>
#include <LibCore/MachPort.h>
#include <LibCore/Notifier.h>
#include <LibCore/Platform/ThreadQoS.h>
#include <LibCore/System.h>
#include <LibIPC/Limits.h>
#include <LibIPC/TransportMachPort.h>
#include <LibThreading/Thread.h>

#include <mach/mach.h>

namespace IPC {

static constexpr size_t INLINE_MESSAGE_MAX_SIZE = 4096;

struct InlinePayloadHeader {
    u32 payload_size { 0 };
};

static size_t round_mach_message_size(size_t size)
{
    constexpr size_t alignment = sizeof(natural_t);
    return ((size + alignment - 1) / alignment) * alignment;
}

static size_t mach_message_size_for_inline_payload(size_t payload_size, size_t port_count)
{
    size_t message_size = sizeof(mach_msg_header_t) + sizeof(InlinePayloadHeader) + payload_size;
    if (port_count > 0) {
        message_size += sizeof(mach_msg_body_t);
        message_size += port_count * sizeof(mach_msg_port_descriptor_t);
    }
    return round_mach_message_size(message_size);
}

static size_t mach_message_size_for_out_of_line_payload(size_t port_count)
{
    return round_mach_message_size(sizeof(mach_msg_header_t)
        + sizeof(mach_msg_body_t)
        + (port_count * sizeof(mach_msg_port_descriptor_t))
        + sizeof(mach_msg_ool_descriptor_t));
}

static ReceivedMessageBytes inline_payload_bytes(u8 const* payload, size_t payload_region_size)
{
    VERIFY(payload_region_size >= sizeof(InlinePayloadHeader));

    InlinePayloadHeader payload_header;
    __builtin_memcpy(&payload_header, payload, sizeof(payload_header));

    VERIFY(payload_header.payload_size <= payload_region_size - sizeof(InlinePayloadHeader));

    Vector<u8> bytes;
    MUST(bytes.try_append(payload + sizeof(InlinePayloadHeader), payload_header.payload_size));
    return ReceivedMessageBytes::from_vector(move(bytes));
}

static void set_mach_port_queue_limit(mach_port_t port)
{
    mach_port_limits_t limits { .mpl_qlimit = MACH_PORT_QLIMIT_MAX };
    // Mach receive rights start with a small queue. Raise it so short bursts can wait in the kernel if the IO
    // thread falls behind for a moment.
    mach_port_set_attributes(mach_task_self(), port, MACH_PORT_LIMITS_INFO,
        reinterpret_cast<mach_port_info_t>(&limits), MACH_PORT_LIMITS_INFO_COUNT);
}

static bool is_well_formed_port_descriptor(mach_msg_port_descriptor_t const& descriptor)
{
    if (descriptor.type != MACH_MSG_PORT_DESCRIPTOR || descriptor.name == MACH_PORT_NULL)
        return false;
    switch (descriptor.disposition) {
    case MACH_MSG_TYPE_MOVE_SEND:
    case MACH_MSG_TYPE_MOVE_RECEIVE:
    case MACH_MSG_TYPE_MOVE_SEND_ONCE:
        return true;
    default:
        return false;
    }
}

// Checks the shape of a data message against what send_mach_message() builds: port descriptors, then for an
// out-of-line message the descriptor of its payload, or for an inline one the payload itself. The message comes from
// the peer, so anything else is a malformed message to reject, not an invariant of ours to assert.
static bool is_well_formed_data_message(mach_msg_header_t const* header, bool is_out_of_line)
{
    auto const* message = reinterpret_cast<u8 const*>(header);
    size_t const message_size = header->msgh_size;
    size_t offset = sizeof(mach_msg_header_t);

    size_t descriptor_count = 0;
    if (header->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
        if (message_size < offset + sizeof(mach_msg_body_t))
            return false;
        descriptor_count = reinterpret_cast<mach_msg_body_t const*>(message + offset)->msgh_descriptor_count;
        offset += sizeof(mach_msg_body_t);
    }

    if (is_out_of_line && descriptor_count == 0)
        return false;
    size_t const port_count = is_out_of_line ? descriptor_count - 1 : descriptor_count;
    if (message_size < offset + (port_count * sizeof(mach_msg_port_descriptor_t)))
        return false;
    auto const* port_descriptors = reinterpret_cast<mach_msg_port_descriptor_t const*>(message + offset);
    for (size_t i = 0; i < port_count; ++i) {
        if (!is_well_formed_port_descriptor(port_descriptors[i]))
            return false;
    }
    offset += port_count * sizeof(mach_msg_port_descriptor_t);

    if (is_out_of_line) {
        if (message_size < offset + sizeof(mach_msg_ool_descriptor_t))
            return false;
        return reinterpret_cast<mach_msg_ool_descriptor_t const*>(message + offset)->type == MACH_MSG_OOL_DESCRIPTOR;
    }

    if (message_size < offset + sizeof(InlinePayloadHeader))
        return false;
    InlinePayloadHeader payload_header;
    __builtin_memcpy(&payload_header, message + offset, sizeof(payload_header));
    return payload_header.payload_size <= message_size - offset - sizeof(InlinePayloadHeader);
}

static Attachment attachment_from_descriptor(mach_msg_port_descriptor_t const& descriptor)
{
    VERIFY(descriptor.type == MACH_MSG_PORT_DESCRIPTOR);
    // NB: A send right arrives as MACH_PORT_DEAD if its port died before this message was received — e.g. a transport
    // endpoint whose peer closed while the handle was in flight. That's the kernel reporting a fact about the peer, not
    // a malformed message. So, adopt the dead name like any other, and leave it to whoever consumes the attachment.
    // Gecko/WebKit/Blink do the same: Gecko ChannelMach::ProcessIncomingMessage, WebKit createMessageDecoder, Blink
    // ChannelMac::OnMachMessageReceived — each adopting descriptor.name unchecked.
    VERIFY(descriptor.name != MACH_PORT_NULL);

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

    // The peer may receive messages before it has constructed its transport.
    raise_receive_queue_limit(port_b_recv);

    return Paired {
        make<TransportMachPort>(move(port_a_recv), move(port_b_send)),
        TransportHandle { move(port_b_recv), move(port_a_send) },
    };
}

void TransportMachPort::raise_receive_queue_limit(Core::MachPort const& receive_right)
{
    set_mach_port_queue_limit(receive_right.port());
}

// Answers whether the notification used up the send-possible request, which it does once the peer's queue has room,
// or as a dead-name notification once the peer's port has died.
static bool consume_send_possible_notification(mach_msg_header_t* header)
{
    switch (header->msgh_id) {
    case MACH_NOTIFY_SEND_POSSIBLE:
        return true;
    case MACH_NOTIFY_DEAD_NAME:
        // NB: This task holds a reference to the dead name for the notification, which is to be released with it.
        mach_port_deallocate(mach_task_self(), reinterpret_cast<mach_dead_name_notification_t const*>(header)->not_port);
        return true;
    default:
        mach_msg_destroy(header);
        return false;
    }
}

static constexpr mach_msg_timeout_t IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS = 1000;
static constexpr mach_msg_timeout_t RETRY_DELAY_IN_MILLISECONDS = 1;
static constexpr size_t RECEIVE_BUFFER_SIZE = 65536;
static constexpr size_t MAX_MESSAGES_RECEIVED_BETWEEN_TURNS_OF_SENDING = 64;
static constexpr mach_msg_option_t RECEIVE_WITH_CONTEXT_TRAILER = MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) | MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_CTX);

static thread_local bool s_is_shared_io_thread { false };

// The IO thread of every transport in the process. It waits on one port set, which holds the receive right of each
// transport with the transport as the port's context, and the kernel hands that context back with every message. Each
// message costs this thread a copy and a wakeup of its owner, so the messages of other transports hold one up only
// briefly.
class SharedMachIOThread {
    AK_MAKE_NONCOPYABLE(SharedMachIOThread);
    AK_MAKE_NONMOVABLE(SharedMachIOThread);

public:
    AK_ALLOC_WITH_KMALLOC;

    static SharedMachIOThread& the()
    {
        static NeverDestroyed<SharedMachIOThread> shared_io_thread;
        return *shared_io_thread;
    }

    SharedMachIOThread()
    {
        m_port_set = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::PortSet));
        m_wakeup_receive_port = MUST(Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive));
        m_wakeup_send_port = MUST(m_wakeup_receive_port.insert_right(Core::MachPort::MessageRight::MakeSend));
        auto const ret = mach_port_insert_member(mach_task_self(), m_wakeup_receive_port.port(), m_port_set.port());
        VERIFY(ret == KERN_SUCCESS);

        // NB: Nothing waits for this thread, which goes with the process.
        auto thread = Threading::Thread::construct("IPC IO (Mach)"sv, [this] { return loop(); });
        // This thread only relays messages between the kernel and the threads that own the transports. If it ran below
        // them, every message would wait on a lower-priority wakeup.
        thread->set_qos(Core::Platform::ThreadQoS::UserInteractive);
        thread->start();
        thread->detach();
    }

    // Messages that arrive on the port, one of the transport's own, are the transport's from here on.
    [[nodiscard]] bool add(TransportMachPort& transport, mach_port_t port)
    {
        if (mach_port_set_context(mach_task_self(), port, reinterpret_cast<mach_port_context_t>(&transport)) != KERN_SUCCESS)
            return false;
        return mach_port_insert_member(mach_task_self(), port, m_port_set.port()) == KERN_SUCCESS;
    }

    // The task holds one notification request per name, and transports that send to the same port share the name of
    // their send right, so only one of them at a time may have a send-possible request on it. Nor may one take the
    // place of a request that something else has made there. Answers whether the transport may make its request.
    [[nodiscard]] bool claim_send_possible_request(TransportMachPort& transport, mach_port_t destination)
    {
        MutexLocker locker(m_send_possible_request_owners_mutex);
        if (m_send_possible_request_owners.contains(destination))
            return false;
        mach_port_type_t type = 0;
        if (mach_port_type(mach_task_self(), destination, &type) != KERN_SUCCESS || (type & (MACH_PORT_TYPE_DNREQUEST | MACH_PORT_TYPE_SPREQUEST)))
            return false;
        m_send_possible_request_owners.set(destination, &transport);
        return true;
    }

    void release_send_possible_request(TransportMachPort& transport, mach_port_t destination)
    {
        MutexLocker locker(m_send_possible_request_owners_mutex);
        if (m_send_possible_request_owners.get(destination) == &transport)
            m_send_possible_request_owners.remove(destination);
    }

    // Has this thread send the transport's backlog, take note of a peer it found gone, and stop its IO if asked to.
    // A backlog is otherwise sent when the kernel says the peer's queue has room.
    void request_attention(TransportMachPort& transport)
    {
        {
            MutexLocker locker(m_mutex);
            if (transport.m_io_is_finished)
                return;
            if (!transport.m_needs_attention) {
                transport.m_needs_attention = true;
                m_transports_needing_attention.append(&transport);
            }
        }
        wake();
    }

    void wait_until_io_is_finished(TransportMachPort& transport)
    {
        VERIFY(!s_is_shared_io_thread);
        MutexLocker locker(m_mutex);
        while (!transport.m_io_is_finished)
            m_io_finished_cv.wait();
    }

    // Takes over the notification port of a transport whose IO is over, while the kernel still owes it a notification
    // for a request that it has used up. The port goes once that has arrived.
    void keep_until_its_last_notification(Core::MachPort notification_port)
    {
        VERIFY(s_is_shared_io_thread);
        auto const ret = mach_port_set_context(mach_task_self(), notification_port.port(), orphaned_notification_port_context());
        VERIFY(ret == KERN_SUCCESS);
        (void)notification_port.release();
    }

private:
    void wake()
    {
        mach_msg_header_t header {};
        header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
        header.msgh_size = sizeof(header);
        header.msgh_remote_port = m_wakeup_send_port.port();
        header.msgh_local_port = MACH_PORT_NULL;
        header.msgh_id = TransportMachPort::IPC_WAKEUP_MESSAGE_ID;
        // NB: A full wakeup queue already has this thread on its way. The send that found it full comes back
        //     pseudo-received, with a send right of its own on the wakeup port, which must not pile up.
        if (mach_msg(&header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof(header), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL) == MACH_SEND_TIMED_OUT)
            mach_msg_destroy(&header);
    }

    mach_port_context_t orphaned_notification_port_context() const { return reinterpret_cast<mach_port_context_t>(this); }

    intptr_t loop();
    bool has_transports_needing_attention();
    void serve_transports_needing_attention();
    void serve(TransportMachPort&);
    // Waits for one message no longer than the timeout, if there is one, and answers whether one came.
    bool receive_and_handle_message(Optional<mach_msg_timeout_t> timeout);
    void handle_failed_receive(kern_return_t, mach_port_t port);
    TransportMachPort* transport_receiving_on(mach_port_t port);
    // The owner may destroy the transport as soon as this returns.
    void finish(TransportMachPort&);

    Core::MachPort m_port_set;
    Core::MachPort m_wakeup_receive_port;
    Core::MachPort m_wakeup_send_port;

    Mutex m_mutex;
    ConditionVariable m_io_finished_cv { m_mutex };
    Vector<TransportMachPort*> m_transports_needing_attention;

    Mutex m_send_possible_request_owners_mutex;
    HashMap<mach_port_t, TransportMachPort*> m_send_possible_request_owners;

    // NB: Only this thread uses these.
    Vector<u8> m_receive_buffer;
    Vector<TransportMachPort*> m_transports_to_serve;
    // Transports whose peer's queue is full, which the kernel could not be asked to say when it has room.
    Vector<TransportMachPort*> m_transports_retrying_after_delay;
    MonotonicTime m_retry_deadline;
};

intptr_t SharedMachIOThread::loop()
{
    s_is_shared_io_thread = true;

    m_receive_buffer.resize(RECEIVE_BUFFER_SIZE);
    bool released_memory_since_last_message = true;

    for (;;) {
        serve_transports_needing_attention();

        // A backlog that one turn did not get through goes on in the next, once this thread has taken in what came
        // meanwhile.
        if (has_transports_needing_attention()) {
            for (size_t i = 0; i < MAX_MESSAGES_RECEIVED_BETWEEN_TURNS_OF_SENDING; ++i) {
                if (!receive_and_handle_message(0))
                    break;
                released_memory_since_last_message = false;
            }
            continue;
        }

        // Wait on the whole port set, for messages from every peer, wakeups, and kernel notifications like
        // MACH_NOTIFY_NO_SENDERS, or MACH_NOTIFY_SEND_POSSIBLE once a peer's full queue has room.
        bool const retries_after_delay = !m_transports_retrying_after_delay.is_empty();
        Optional<mach_msg_timeout_t> timeout;
        if (retries_after_delay)
            timeout = static_cast<mach_msg_timeout_t>(max<i64>(0, ((m_retry_deadline - MonotonicTime::now()).to_microseconds() + 999) / 1000));
        else if (!released_memory_since_last_message)
            timeout = IDLE_MEMORY_RELEASE_DELAY_IN_MILLISECONDS;
        if (receive_and_handle_message(timeout)) {
            released_memory_since_last_message = false;
            continue;
        }
        // NB: The messages this thread receives are freed by the threads that handle them, and the allocator keeps that
        //     memory until this thread allocates again or collects, which an idle process may never do.
        if (!retries_after_delay) {
            ak_kmalloc_collect();
            released_memory_since_last_message = true;
        }
    }
}

bool SharedMachIOThread::receive_and_handle_message(Optional<mach_msg_timeout_t> timeout)
{
    // NB: The buffer lives as long as the process, so it does not keep the size of the largest message a peer sent.
    ScopeGuard shrink_buffer_grown_for_a_large_message = [&] {
        if (m_receive_buffer.size() > RECEIVE_BUFFER_SIZE) {
            m_receive_buffer.clear();
            m_receive_buffer.resize(RECEIVE_BUFFER_SIZE);
        }
    };
    auto const receive_options = MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY | RECEIVE_WITH_CONTEXT_TRAILER
        | (timeout.has_value() ? MACH_RCV_TIMEOUT : 0);
    auto* header = reinterpret_cast<mach_msg_header_t*>(m_receive_buffer.data());
    header->msgh_local_port = MACH_PORT_NULL;
    auto ret = mach_msg(header, receive_options, 0, m_receive_buffer.size(), m_port_set.port(), timeout.value_or(MACH_MSG_TIMEOUT_NONE), MACH_PORT_NULL);
    if (ret == MACH_RCV_TIMED_OUT)
        return false;

    // MACH_RCV_LARGE leaves a message that does not fit in its queue, and MACH_RCV_LARGE_IDENTITY names its port.
    // The retry receives from that port alone, as on the whole set it could take another port's message, which need
    // not fit either.
    while (ret == MACH_RCV_TOO_LARGE) {
        auto const port = header->msgh_local_port;
        size_t needed_size = header->msgh_size + REQUESTED_TRAILER_SIZE(RECEIVE_WITH_CONTEXT_TRAILER);
        if (needed_size <= m_receive_buffer.size())
            needed_size = m_receive_buffer.size() * 2;
        m_receive_buffer.resize(needed_size);
        header = reinterpret_cast<mach_msg_header_t*>(m_receive_buffer.data());
        header->msgh_local_port = port;
        ret = mach_msg(header, MACH_RCV_MSG | MACH_RCV_LARGE | MACH_RCV_LARGE_IDENTITY | RECEIVE_WITH_CONTEXT_TRAILER,
            0, m_receive_buffer.size(), port, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }

    if (ret != KERN_SUCCESS) {
        handle_failed_receive(ret, header->msgh_local_port);
        return true;
    }

    if (header->msgh_local_port == m_wakeup_receive_port.port()) {
        VERIFY(header->msgh_id == TransportMachPort::IPC_WAKEUP_MESSAGE_ID);
        return true;
    }

    auto const& trailer = *reinterpret_cast<mach_msg_context_trailer_t const*>(m_receive_buffer.data() + round_mach_message_size(header->msgh_size));
    if (trailer.msgh_context == orphaned_notification_port_context()) {
        if (consume_send_possible_notification(header))
            (void)mach_port_mod_refs(mach_task_self(), header->msgh_local_port, MACH_PORT_RIGHT_RECEIVE, -1);
        return true;
    }

    auto* transport = reinterpret_cast<TransportMachPort*>(trailer.msgh_context);
    VERIFY(transport);
    if (header->msgh_local_port != transport->m_receive_port.port()) {
        // NB: The only other port with the transport as its context is the one it hears on that its peer's queue has
        //     room, which only the kernel can send to.
        transport->handle_send_possible_notification(m_receive_buffer.data());
        serve(*transport);
        return true;
    }
    if (transport->handle_message_on_io_thread(m_receive_buffer.data()) == IterationDecision::Break)
        finish(*transport);
    return true;
}

bool SharedMachIOThread::has_transports_needing_attention()
{
    MutexLocker locker(m_mutex);
    return !m_transports_needing_attention.is_empty();
}

void SharedMachIOThread::serve_transports_needing_attention()
{
    {
        MutexLocker locker(m_mutex);
        swap(m_transports_to_serve, m_transports_needing_attention);
        for (auto* transport : m_transports_to_serve)
            transport->m_needs_attention = false;
    }
    if (!m_transports_retrying_after_delay.is_empty() && MonotonicTime::now() >= m_retry_deadline) {
        for (auto* transport : m_transports_retrying_after_delay) {
            if (!m_transports_to_serve.contains_slow(transport))
                m_transports_to_serve.append(transport);
        }
        m_transports_retrying_after_delay.clear_with_capacity();
    }

    for (auto* transport : m_transports_to_serve)
        serve(*transport);
    m_transports_to_serve.clear_with_capacity();
}

void SharedMachIOThread::serve(TransportMachPort& transport)
{
    switch (transport.do_pending_io()) {
    case TransportMachPort::PendingIO::None:
        break;
    case TransportMachPort::PendingIO::MoreToSendNextTurn: {
        MutexLocker locker(m_mutex);
        if (!transport.m_needs_attention) {
            transport.m_needs_attention = true;
            m_transports_needing_attention.append(&transport);
        }
        break;
    }
    case TransportMachPort::PendingIO::RetryAfterDelay:
        if (m_transports_retrying_after_delay.is_empty())
            m_retry_deadline = MonotonicTime::now() + AK::Duration::from_milliseconds(RETRY_DELAY_IN_MILLISECONDS);
        if (!m_transports_retrying_after_delay.contains_slow(&transport))
            m_transports_retrying_after_delay.append(&transport);
        break;
    case TransportMachPort::PendingIO::IOIsOver:
        finish(transport);
        break;
    }
}

void SharedMachIOThread::handle_failed_receive(kern_return_t ret, mach_port_t port)
{
    dbgln("TransportMachPort: mach_msg receive failed: {}", mach_error_string(ret));
    // NB: A message that could not be received whole is gone, and the transport it was for can no longer deliver in
    //     order what its peer sends, so its IO is over. That is if the kernel got as far as naming the port it came
    //     in on. Any other failure would be one of the port set itself.
    auto const error = ret & ~MACH_MSG_MASK;
    VERIFY(error == MACH_RCV_HEADER_ERROR || error == MACH_RCV_BODY_ERROR);
    auto* transport = transport_receiving_on(port);
    if (!transport)
        return;
    transport->m_io_thread_state = TransportMachPort::IOThreadState::Stopped;
    finish(*transport);
}

TransportMachPort* SharedMachIOThread::transport_receiving_on(mach_port_t port)
{
    if (!MACH_PORT_VALID(port) || port == m_wakeup_receive_port.port())
        return nullptr;
    mach_port_context_t context = 0;
    if (mach_port_get_context(mach_task_self(), port, &context) != KERN_SUCCESS || context == orphaned_notification_port_context())
        return nullptr;
    return reinterpret_cast<TransportMachPort*>(context);
}

void SharedMachIOThread::finish(TransportMachPort& transport)
{
    // Once the receive right has left the set, nothing more comes in for the transport.
    auto const port = transport.m_receive_port.port();
    auto ret = mach_port_extract_member(mach_task_self(), port, m_port_set.port());
    VERIFY(ret == KERN_SUCCESS);
    ret = mach_port_set_context(mach_task_self(), port, 0);
    VERIFY(ret == KERN_SUCCESS);
    m_transports_retrying_after_delay.remove_all_matching([&](auto* other) { return other == &transport; });

    transport.finish_io();

    MutexLocker locker(m_mutex);
    if (transport.m_needs_attention)
        m_transports_needing_attention.remove_first_matching([&](auto* other) { return other == &transport; });
    transport.m_io_is_finished = true;
    m_io_finished_cv.broadcast();
}

TransportMachPort::TransportMachPort(Core::MachPort receive_right, Core::MachPort send_right)
    : m_receive_port(move(receive_right))
    , m_send_port(move(send_right))
{
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

    auto const added = SharedMachIOThread::the().add(*this, m_receive_port.port());
    VERIFY(added);
}

TransportMachPort::~TransportMachPort()
{
    stop_io_thread(IOThreadState::Stopped);
    m_read_hook_notifier.clear();
}

void TransportMachPort::stop_io_thread(IOThreadState desired_state)
{
    // NB: The IO thread may already have stopped on its own (e.g. the peer hung up). Stopped is terminal, so never
    //     overwrite it with a new stop request.
    auto state = m_io_thread_state.load(AK::MemoryOrder::memory_order_acquire);
    while (state != IOThreadState::Stopped && !m_io_thread_state.compare_exchange_strong(state, desired_state, AK::MemoryOrder::memory_order_acq_rel)) { }
    auto& io_thread = SharedMachIOThread::the();
    io_thread.request_attention(*this);
    io_thread.wait_until_io_is_finished(*this);

    // NB: What was left unsent goes here rather than on the IO thread, which all transports share, as releasing the
    //     rights of a long backlog takes a while.
    MutexLocker locker(m_send_mutex);
    m_send_backlog.clear();
}

void TransportMachPort::wake_io_thread()
{
    SharedMachIOThread::the().request_attention(*this);
}

bool TransportMachPort::schedule_read_notification_if_needed_locked()
{
    if (m_read_notification_pending)
        return false;

    m_read_notification_pending = true;
    return true;
}

void TransportMachPort::write_read_notification_byte()
{
    if (!m_notify_hook_write_fd)
        return;

    Array<u8, 1> bytes = { 0 };
    (void)Core::System::write(m_notify_hook_write_fd->value(), bytes);
}

void TransportMachPort::stop_sending()
{
    MutexLocker locker(m_send_mutex);
    m_sending_is_over = true;
    // NB: A request that cancelling does not take back has been used up, and the kernel owes a notification for it,
    //     which need not be queued yet. One about a dead port holds a reference to the dead name, to be released.
    bool notification_is_owed = false;
    if (m_send_possible_notification_is_requested) {
        mach_port_t previous = MACH_PORT_NULL;
        auto const ret = mach_port_request_notification(mach_task_self(), m_send_port.port(), MACH_NOTIFY_SEND_POSSIBLE,
            0, MACH_PORT_NULL, MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
        if (ret == KERN_SUCCESS && MACH_PORT_VALID(previous))
            mach_port_deallocate(mach_task_self(), previous);
        else
            notification_is_owed = true;
        m_send_possible_notification_is_requested = false;
        SharedMachIOThread::the().release_send_possible_request(*this, m_send_port.port());
    }
    if (MACH_PORT_VALID(m_send_possible_notification_port.port())) {
        Array<u8, 256> buffer {};
        auto* header = reinterpret_cast<mach_msg_header_t*>(buffer.data());
        while (mach_msg(header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, buffer.size(), m_send_possible_notification_port.port(), 0, MACH_PORT_NULL) == KERN_SUCCESS) {
            if (consume_send_possible_notification(header))
                notification_is_owed = false;
        }
        if (notification_is_owed)
            SharedMachIOThread::the().keep_until_its_last_notification(move(m_send_possible_notification_port));
        else
            m_send_possible_notification_port = {}; // NB: Destroying the receive right takes it out of the port set.
    }
    m_sent_cv.broadcast();
}

// The request is armed as it is made, and answered right away if the peer's queue has room by then. Answers whether it
// was made. A transport that can't make one retries after a delay instead.
bool TransportMachPort::request_send_possible_notification()
{
    VERIFY(!m_send_possible_notification_is_requested);
    auto& io_thread = SharedMachIOThread::the();
    if (!MACH_PORT_VALID(m_send_possible_notification_port.port())) {
        auto port = Core::MachPort::create_with_right(Core::MachPort::PortRight::Receive);
        if (port.is_error() || !io_thread.add(*this, port.value().port()))
            return false;
        m_send_possible_notification_port = port.release_value();
    }
    auto const destination = m_send_port.port();
    if (!io_thread.claim_send_possible_request(*this, destination))
        return false;
    mach_port_t previous = MACH_PORT_NULL;
    auto const ret = mach_port_request_notification(mach_task_self(), destination, MACH_NOTIFY_SEND_POSSIBLE, 1,
        m_send_possible_notification_port.port(), MACH_MSG_TYPE_MAKE_SEND_ONCE, &previous);
    if (ret != KERN_SUCCESS) {
        io_thread.release_send_possible_request(*this, destination);
        return false;
    }
    if (MACH_PORT_VALID(previous)) {
        // NB: Something besides the transports made a request on the name after the claim checked it, and this one took
        //     its place. It goes back, as the dead-name request that anything but a transport makes.
        mach_port_t replaced = MACH_PORT_NULL;
        if (mach_port_request_notification(mach_task_self(), destination, MACH_NOTIFY_DEAD_NAME, 1, previous, MACH_MSG_TYPE_MOVE_SEND_ONCE, &replaced) != KERN_SUCCESS)
            mach_port_deallocate(mach_task_self(), previous);
        if (MACH_PORT_VALID(replaced))
            mach_port_deallocate(mach_task_self(), replaced);
        io_thread.release_send_possible_request(*this, destination);
        return false;
    }
    m_send_possible_notification_is_requested = true;
    return true;
}

void TransportMachPort::handle_send_possible_notification(u8* buffer)
{
    if (!consume_send_possible_notification(reinterpret_cast<mach_msg_header_t*>(buffer)))
        return;
    MutexLocker locker(m_send_mutex);
    m_send_possible_notification_is_requested = false;
    SharedMachIOThread::the().release_send_possible_request(*this, m_send_port.port());
}

void TransportMachPort::flush()
{
    if (!m_is_open.load(AK::MemoryOrder::memory_order_relaxed))
        return;

    MutexLocker locker(m_send_mutex);
    while (!m_sending_is_over && !m_send_backlog.is_empty()) {
        // The wakeup is best-effort, so re-arm it rather than wait forever if one is ever dropped.
        if (!m_sent_cv.wait_for(AK::Duration::from_milliseconds(50)))
            wake_io_thread();
    }
}

void TransportMachPort::wait_until_incoming_is_current()
{
    MutexLocker locker(m_incoming_mutex);
    if (m_peer_eof.load() || m_io_thread_state.load() != IOThreadState::Running)
        return;

    // NB: A marker on the same receive port follows all previously sent messages, including one the IO
    //     thread has removed from the port but has not published yet. A send-once right preserves EOF detection
    //     and bypasses the queue limit, so sending while holding m_incoming_mutex cannot block on a full port.
    mach_port_t send_once_port = MACH_PORT_NULL;
    mach_msg_type_name_t right_type = 0;
    auto const extract_ret = mach_port_extract_right(mach_task_self(), m_receive_port.port(),
        MACH_MSG_TYPE_MAKE_SEND_ONCE, &send_once_port, &right_type);
    VERIFY(extract_ret == KERN_SUCCESS);
    auto send_right = Core::MachPort::adopt_right(send_once_port, Core::MachPort::PortRight::SendOnce);
    mach_msg_header_t header {};
    header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
    header.msgh_size = sizeof(header);
    header.msgh_remote_port = send_right.port();
    header.msgh_id = IPC_RECEIVE_BARRIER_MESSAGE_ID;
    auto const ret = mach_msg(&header, MACH_SEND_MSG, sizeof(header), 0, MACH_PORT_NULL,
        MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    VERIFY(ret == KERN_SUCCESS);
    (void)send_right.release();
    VERIFY(m_receive_barriers_sent < NumericLimits<u64>::max());
    auto const barrier = ++m_receive_barriers_sent;
    while (!m_peer_eof.load(AK::MemoryOrder::memory_order_relaxed)
        && m_io_thread_state.load() == IOThreadState::Running
        && m_receive_barriers_received < barrier) {
        m_incoming_cv.wait();
    }
}

void TransportMachPort::mark_peer_eof()
{
    notify_incoming_message_observer({});

    bool should_write_notification = false;
    {
        MutexLocker locker(m_incoming_mutex);
        m_peer_eof = true;
        should_write_notification = schedule_read_notification_if_needed_locked();
    }
    m_incoming_cv.broadcast();
    if (should_write_notification)
        write_read_notification_byte();
}

TransportMachPort::PendingIO TransportMachPort::do_pending_io()
{
    if (m_io_thread_state.load() == IOThreadState::Stopped)
        return PendingIO::IOIsOver;

    // The backlog is retried while the IO thread keeps receiving: a thread that blocked in a send would stop draining
    // its own queue, and two such threads facing each other would wait on each other for good.
    bool peer_is_gone = m_posting_thread_found_peer_gone.exchange(false);
    auto pending_io = PendingIO::None;
    {
        MutexLocker locker(m_send_mutex);
        bool const had_backlog = !m_send_backlog.is_empty();
        // NB: A turn sends a bounded part of a long backlog, as of one that a peer takes again after a while, so that the
        //     IO thread gets back to receiving, and to the other transports, in between.
        auto outcome = SendOutcome::Sent;
        for (size_t sent_count = 0; !m_send_backlog.is_empty() && sent_count < MAX_BACKLOG_MESSAGES_SENT_PER_TURN; ++sent_count) {
            outcome = send_mach_message(m_send_backlog.head());
            if (outcome == SendOutcome::WouldBlock)
                break;
            (void)m_send_backlog.dequeue();
            if (outcome == SendOutcome::PeerGone)
                peer_is_gone = true;
        }
        if (m_send_backlog.is_empty()) {
            if (had_backlog) {
                // NB: A queue that has drained keeps its last segment, which clearing it lets go of.
                m_send_backlog.clear();
                m_sent_cv.broadcast();
            }
            // NB: Posting threads check the state under this lock, so nothing is left behind in the backlog.
            if (m_io_thread_state.load() == IOThreadState::SendPendingMessagesAndStop) {
                m_io_thread_state = IOThreadState::Stopped;
                pending_io = PendingIO::IOIsOver;
            }
        } else if (outcome != SendOutcome::WouldBlock) {
            pending_io = PendingIO::MoreToSendNextTurn;
        } else if (!m_send_possible_notification_is_requested) {
            pending_io = PendingIO::RetryAfterDelay;
        }
    }
    if (peer_is_gone)
        mark_peer_eof();
    return pending_io;
}

IterationDecision TransportMachPort::handle_message_on_io_thread(u8* buffer)
{
    auto* header = reinterpret_cast<mach_msg_header_t*>(buffer);
    switch (header->msgh_id) {
    case IPC_RECEIVE_BARRIER_MESSAGE_ID: {
        MutexLocker locker(m_incoming_mutex);
        ++m_receive_barriers_received;
        m_incoming_cv.broadcast();
        return IterationDecision::Continue;
    }
    case MACH_NOTIFY_NO_SENDERS:
        mark_peer_eof();
        return IterationDecision::Continue;
    case MACH_NOTIFY_SEND_ONCE:
        return IterationDecision::Continue;
    case IPC_DATA_MESSAGE_ID:
    case IPC_INLINE_DATA_MESSAGE_ID:
        if (process_received_message(buffer) == IterationDecision::Break) {
            m_io_thread_state = IOThreadState::Stopped;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    default:
        dbgln("TransportMachPort: Rejecting message with unknown id {:#x}", header->msgh_id);
        mach_msg_destroy(header);
        mark_peer_eof();
        m_io_thread_state = IOThreadState::Stopped;
        return IterationDecision::Break;
    }
}

void TransportMachPort::finish_io()
{
    VERIFY(m_io_thread_state == IOThreadState::Stopped);
    stop_sending();
    {
        MutexLocker locker(m_incoming_mutex);
        m_incoming_cv.broadcast();
    }
    // Stopping for transfer tears down the old endpoint on purpose. Do not surface that as peer EOF;
    // the receive and send rights are about to be adopted by the new transport owner.
    if (!m_is_being_transferred.load(AK::MemoryOrder::memory_order_acquire))
        mark_peer_eof();
}

TransportMachPort::SendOutcome TransportMachPort::send_mach_message(PendingMessage& msg)
{
    auto const& bytes = msg.bytes;
    auto& attachments = msg.attachments;
    size_t port_count = attachments.size();

    auto inline_msg_size = mach_message_size_for_inline_payload(bytes.size(), port_count);
    bool send_payload_inline = inline_msg_size <= INLINE_MESSAGE_MAX_SIZE;
    auto msg_size = send_payload_inline ? inline_msg_size : mach_message_size_for_out_of_line_payload(port_count);

    if (m_send_buffer.size() < msg_size)
        m_send_buffer.resize(msg_size);

    auto* header = reinterpret_cast<mach_msg_header_t*>(m_send_buffer.data());
    header->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
    header->msgh_size = msg_size;
    header->msgh_remote_port = m_send_port.port();
    header->msgh_local_port = MACH_PORT_NULL;
    header->msgh_id = send_payload_inline ? IPC_INLINE_DATA_MESSAGE_ID : IPC_DATA_MESSAGE_ID;

    u8* inline_payload = nullptr;
    mach_msg_port_descriptor_t* port_descriptors = nullptr;
    if (port_count > 0 || !send_payload_inline) {
        header->msgh_bits |= MACH_MSGH_BITS_COMPLEX;

        auto* body = reinterpret_cast<mach_msg_body_t*>(header + 1);
        body->msgh_descriptor_count = port_count + (send_payload_inline ? 0 : 1);

        port_descriptors = reinterpret_cast<mach_msg_port_descriptor_t*>(body + 1);
        for (size_t i = 0; i < port_count; ++i) {
            auto disposition = static_cast<mach_msg_type_name_t>(attachments[i].message_right());
            auto port = attachments[i].release_mach_port();
            port_descriptors[i].name = port.release();
            port_descriptors[i].disposition = disposition;
            port_descriptors[i].type = MACH_MSG_PORT_DESCRIPTOR;
        }

        if (send_payload_inline) {
            inline_payload = reinterpret_cast<u8*>(&port_descriptors[port_count]);
        } else {
            auto* ool_desc = reinterpret_cast<mach_msg_ool_descriptor_t*>(&port_descriptors[port_count]);
            ool_desc->address = const_cast<void*>(static_cast<void const*>(bytes.data()));
            ool_desc->size = bytes.size();
            ool_desc->deallocate = false;
            ool_desc->copy = MACH_MSG_VIRTUAL_COPY;
            ool_desc->type = MACH_MSG_OOL_DESCRIPTOR;
        }
    } else {
        inline_payload = reinterpret_cast<u8*>(header + 1);
    }

    if (send_payload_inline) {
        VERIFY(inline_payload);

        InlinePayloadHeader payload_header { static_cast<u32>(bytes.size()) };
        VERIFY(static_cast<size_t>(payload_header.payload_size) == bytes.size());

        __builtin_memcpy(inline_payload, &payload_header, sizeof(payload_header));
        if (!bytes.is_empty())
            __builtin_memcpy(inline_payload + sizeof(payload_header), bytes.data(), bytes.size());

        auto used_size = static_cast<size_t>(inline_payload - m_send_buffer.data())
            + sizeof(payload_header)
            + bytes.size();
        VERIFY(used_size <= msg_size);
        if (used_size < msg_size)
            __builtin_memset(m_send_buffer.data() + used_size, 0, msg_size - used_size);
    }

    // Send one Mach message so attachment right transfer stays atomic with the serialized IPC payload.
    // Small payloads are copied inline to avoid allocating a VM region for the out-of-line descriptor path.
    // The send never waits for room in the peer's queue. A timed-out send comes back pseudo-received, as if this task
    // had received it: with a send right of its own on the destination, a copy the kernel made of an out-of-line
    // payload, and the attachments' rights under the names and dispositions they have now. The first two are released
    // here, or every timeout would leave a right behind and the peer would never hear that its last sender was gone.
    // The rights go back into the message, which the next try builds anew from them and from msg.bytes.
    auto const ret = mach_msg(header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg_size, 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
    // NB: A send that times out comes back pseudo-received even when the kernel can't hand all of it back, which then
    //     shows as a body error.
    if (auto const error = ret & ~MACH_MSG_MASK; error == MACH_SEND_TIMED_OUT || error == MACH_RCV_BODY_ERROR) {
        if (MACH_MSGH_BITS_REMOTE(header->msgh_bits) == MACH_MSG_TYPE_MOVE_SEND && MACH_PORT_VALID(header->msgh_remote_port))
            mach_port_deallocate(mach_task_self(), header->msgh_remote_port);
        if (!send_payload_inline) {
            auto* ool_desc = reinterpret_cast<mach_msg_ool_descriptor_t*>(&port_descriptors[port_count]);
            if (ool_desc->address && ool_desc->address != bytes.data())
                vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(ool_desc->address), ool_desc->size);
        }
        if (ret == MACH_SEND_TIMED_OUT) {
            for (size_t i = 0; i < port_count; ++i)
                attachments[i] = attachment_from_descriptor(port_descriptors[i]);
            if (!m_send_possible_notification_is_requested)
                (void)request_send_possible_notification();
            return SendOutcome::WouldBlock;
        }
        // NB: The kernel was short of something to hand all of the message back with, so it can't go out whole any more,
        //     and fails below. What did come back is released.
        for (size_t i = 0; i < port_count; ++i) {
            if (port_descriptors[i].name != MACH_PORT_NULL) {
                auto dropped_attachment = attachment_from_descriptor(port_descriptors[i]);
                (void)dropped_attachment;
            }
        }
    }
    if (ret != KERN_SUCCESS) {
        if (ret == MACH_SEND_INVALID_DEST) {
            for (size_t i = 0; i < port_count; ++i) {
                auto dropped_attachment = attachment_from_descriptor(port_descriptors[i]);
                (void)dropped_attachment;
            }
        }
        dbgln("TransportMachPort: send failed: {} (send_port={:x})", mach_error_string(ret), m_send_port.port());
        return SendOutcome::PeerGone;
    }
    return SendOutcome::Sent;
}

IterationDecision TransportMachPort::process_received_message(u8* buffer)
{
    auto* header = reinterpret_cast<mach_msg_header_t*>(buffer);
    if (!is_well_formed_data_message(header, header->msgh_id == IPC_DATA_MESSAGE_ID)) {
        dbgln("TransportMachPort: Rejecting malformed message");
        mach_msg_destroy(header);
        mark_peer_eof();
        return IterationDecision::Break;
    }

    auto message = make<Message>();

    if (header->msgh_id == IPC_INLINE_DATA_MESSAGE_ID) {
        VERIFY(header->msgh_size >= sizeof(mach_msg_header_t) + sizeof(InlinePayloadHeader));

        u8 const* payload = nullptr;
        size_t payload_region_size = 0;
        mach_msg_size_t attachment_count = 0;
        mach_msg_port_descriptor_t const* descriptors = nullptr;

        if (header->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
            auto* body = reinterpret_cast<mach_msg_body_t*>(header + 1);
            attachment_count = body->msgh_descriptor_count;
            descriptors = reinterpret_cast<mach_msg_port_descriptor_t*>(body + 1);

            size_t size_with_descriptors = sizeof(mach_msg_header_t)
                + sizeof(mach_msg_body_t)
                + (attachment_count * sizeof(mach_msg_port_descriptor_t));
            VERIFY(size_with_descriptors <= header->msgh_size);

            payload = buffer + size_with_descriptors;
            payload_region_size = header->msgh_size - size_with_descriptors;
        } else {
            payload = reinterpret_cast<u8 const*>(header + 1);
            payload_region_size = header->msgh_size - sizeof(mach_msg_header_t);
        }

        for (mach_msg_size_t i = 0; i < attachment_count; ++i) {
            auto attachment = attachment_from_descriptor(descriptors[i]);
            message->attachments.enqueue(move(attachment));
        }

        message->bytes = inline_payload_bytes(payload, payload_region_size);
    } else {
        VERIFY(header->msgh_id == IPC_DATA_MESSAGE_ID);
        VERIFY(header->msgh_bits & MACH_MSGH_BITS_COMPLEX);
        auto* body = reinterpret_cast<mach_msg_body_t*>(header + 1);
        VERIFY(body->msgh_descriptor_count > 0);
        auto attachment_count = body->msgh_descriptor_count - 1;
        auto* descriptors = reinterpret_cast<mach_msg_port_descriptor_t*>(body + 1);
        auto const* payload = reinterpret_cast<mach_msg_ool_descriptor_t const*>(&descriptors[attachment_count]);

        VERIFY(payload->type == MACH_MSG_OOL_DESCRIPTOR);
        auto payload_bytes = ReceivedMessageBytes::adopt_vm_region(payload->address, payload->size);

        for (unsigned int i = 0; i < attachment_count; ++i) {
            auto attachment = attachment_from_descriptor(descriptors[i]);
            message->attachments.enqueue(move(attachment));
        }

        message->bytes = move(payload_bytes);
    }

    // NB: The rights and the payload region are ours once received, so adopt them first. Dropping the message then
    //     releases them.
    if (message->attachments.size() > MAX_MESSAGE_FD_COUNT) {
        dbgln("TransportMachPort: Rejecting message with {} attachments exceeding limit {}", message->attachments.size(), MAX_MESSAGE_FD_COUNT);
        mark_peer_eof();
        return IterationDecision::Break;
    }

    if (message->bytes.is_empty() && message->attachments.is_empty())
        return IterationDecision::Continue;

    notify_incoming_message_observer(message->bytes.bytes());

    bool should_write_notification = false;
    {
        MutexLocker locker(m_incoming_mutex);
        auto const was_empty = m_incoming_messages.is_empty();
        m_incoming_messages.append(move(message));
        if (was_empty)
            should_write_notification = schedule_read_notification_if_needed_locked();
    }
    m_incoming_cv.signal();
    if (should_write_notification)
        write_read_notification_byte();
    return IterationDecision::Continue;
}

void TransportMachPort::set_incoming_message_observer(Function<void(ReadonlyBytes)> observer)
{
    MutexLocker locker(m_incoming_message_observer_mutex);
    m_incoming_message_observer = move(observer);
}

void TransportMachPort::notify_incoming_message_observer(ReadonlyBytes bytes)
{
    MutexLocker locker(m_incoming_message_observer_mutex);
    if (m_incoming_message_observer)
        m_incoming_message_observer(bytes);
}

void TransportMachPort::set_up_read_hook(Function<void()> hook)
{
    m_on_read_hook = move(hook);
    m_read_hook_notifier = Core::Notifier::construct(m_notify_hook_read_fd->value(), Core::NotificationType::Read);
    m_read_hook_notifier->on_activation = [this] {
        char buf[64];
        (void)Core::System::read(m_notify_hook_read_fd->value(), { buf, sizeof(buf) });
        {
            MutexLocker locker(m_incoming_mutex);
            m_read_notification_pending = false;
        }
        if (m_on_read_hook)
            m_on_read_hook();
    };

    bool should_write_notification = false;
    {
        MutexLocker locker(m_incoming_mutex);
        if (!m_incoming_messages.is_empty() || m_peer_eof)
            should_write_notification = schedule_read_notification_if_needed_locked();
    }
    if (should_write_notification)
        write_read_notification_byte();
}

bool TransportMachPort::is_open() const
{
    return m_is_open && !m_peer_eof;
}

void TransportMachPort::close()
{
    m_is_open = false;
    stop_io_thread(IOThreadState::Stopped);
    release_send_right_to_peer();
}

void TransportMachPort::close_after_sending_all_pending_messages()
{
    stop_io_thread(IOThreadState::SendPendingMessagesAndStop);
    m_is_open = false;
    release_send_right_to_peer();
}

// As closing a socket does, closing the transport tells the peer: its receive port gets MACH_NOTIFY_NO_SENDERS once
// this send right is gone. Sending is over by now, as IO has stopped.
void TransportMachPort::release_send_right_to_peer()
{
    MutexLocker locker(m_send_mutex);
    m_send_port = {};
}

void TransportMachPort::wait_until_readable(Optional<MonotonicTime> deadline)
{
    MutexLocker lock(m_incoming_mutex);
    while (m_incoming_messages.is_empty() && !m_peer_eof && !m_wait_until_readable_interrupted) {
        if (!deadline.has_value()) {
            m_incoming_cv.wait();
            continue;
        }
        auto now = MonotonicTime::now();
        if (now >= *deadline)
            break;
        (void)m_incoming_cv.wait_for(*deadline - now);
    }
    m_wait_until_readable_interrupted = false;
}

void TransportMachPort::interrupt_wait_until_readable()
{
    MutexLocker lock(m_incoming_mutex);
    m_wait_until_readable_interrupted = true;
    m_incoming_cv.broadcast();
}

ErrorOr<void> TransportMachPort::post_message(MessageDataType bytes, Vector<Attachment>& attachments)
{
    PendingMessage message { move(bytes), move(attachments) };
    bool should_wake_io_thread = false;
    {
        MutexLocker locker(m_send_mutex);
        if (m_sending_is_over || m_io_thread_state.load() == IOThreadState::Stopped)
            return {};
        auto outcome = SendOutcome::WouldBlock;
        if (m_send_backlog.is_empty())
            outcome = send_mach_message(message);
        switch (outcome) {
        case SendOutcome::Sent:
            break;
        case SendOutcome::WouldBlock:
            should_wake_io_thread = m_send_backlog.is_empty();
            m_send_backlog.enqueue(move(message));
            break;
        case SendOutcome::PeerGone:
            m_posting_thread_found_peer_gone = true;
            should_wake_io_thread = true;
            break;
        }
    }
    if (should_wake_io_thread)
        wake_io_thread();
    return {};
}

TransportMachPort::ShouldShutdown TransportMachPort::read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&& callback)
{
    Vector<NonnullOwnPtr<Message>> messages;
    bool eof;
    {
        MutexLocker locker(m_incoming_mutex);
        messages = move(m_incoming_messages);
        // Snapshot EOF with the messages so later arrivals cannot be skipped by reporting shutdown.
        eof = m_peer_eof;
    }
    for (auto& message : messages)
        callback(move(*message));
    return eof ? ShouldShutdown::Yes : ShouldShutdown::No;
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

    MutexLocker locker(m_send_mutex);
    return TransportHandle { move(m_receive_port), move(m_send_port) };
}

}
