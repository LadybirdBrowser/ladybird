/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS) && !defined(AK_OS_IOS)
#    error "TransportMachPort is only available on Darwin"
#endif

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/IterationDecision.h>
#include <AK/Mutex.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibCore/MachPort.h>
#include <LibCore/Notifier.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/Forward.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

class TransportMachPort {
    AK_MAKE_NONCOPYABLE(TransportMachPort);
    AK_MAKE_NONMOVABLE(TransportMachPort);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct Paired {
        NonnullOwnPtr<TransportMachPort> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();

    static void raise_receive_queue_limit(Core::MachPort const&);

    TransportMachPort(Core::MachPort receive_right, Core::MachPort send_right);
    ~TransportMachPort();

    void set_up_read_hook(Function<void()>);

    // Sets what the IO thread shows every message that arrives, before the transport's owner gets to see it, and empty
    // bytes once the peer is gone. This is for a thread that does not get back to its event loop on its own, like one
    // stuck in script. It runs while the IO thread holds a lock, so it should only have a look.
    void set_incoming_message_observer(Function<void(ReadonlyBytes)>);

    bool is_open() const;

    void close();
    void close_after_sending_all_pending_messages();

    // Returns at the deadline as well, if one is given.
    void wait_until_readable(Optional<MonotonicTime> deadline = {});

    // Makes wait_until_readable() return, even with nothing to read, now or if none is under way, the next time. Any
    // thread may call this.
    void interrupt_wait_until_readable();

    // Wait until everything posted so far has been handed to the kernel.
    void flush();

    // Wait until everything the peer has already sent has been parsed into the incoming queue.
    void wait_until_incoming_is_current();

    ErrorOr<void> post_message(MessageDataType, Vector<Attachment>& attachments);

    enum class ShouldShutdown {
        No,
        Yes,
    };
    struct Message {
        AK_ALLOC_WITH_KMALLOC;

        ReceivedMessageBytes bytes;
        Queue<Attachment> attachments;
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&&);

    ErrorOr<TransportHandle> release_for_transfer();

private:
    friend class SharedMachIOThread;

    static constexpr unsigned int IPC_DATA_MESSAGE_ID = 0x4950C001;
    static constexpr unsigned int IPC_INLINE_DATA_MESSAGE_ID = 0x4950C002;
    static constexpr unsigned int IPC_WAKEUP_MESSAGE_ID = 0x4950C003;
    static constexpr unsigned int IPC_RECEIVE_BARRIER_MESSAGE_ID = 0x4950C004;

    struct PendingMessage {
        MessageDataType bytes;
        Vector<Attachment> attachments;
    };

    enum class IOThreadState {
        Running,
        SendPendingMessagesAndStop,
        Stopped,
    };

    static constexpr size_t MAX_BACKLOG_MESSAGES_SENT_PER_TURN = 64;
    enum class PendingIO {
        None,
        MoreToSendNextTurn,
        // The peer's queue is full, and the kernel could not be asked to say when it has room.
        RetryAfterDelay,
        IOIsOver,
    };
    // One turn of sending on the IO thread: some of the backlog, as far as the peer has room, and the stop asked for.
    PendingIO do_pending_io();
    // Breaks once IO is over for the transport.
    IterationDecision handle_message_on_io_thread(u8* buffer);
    // Runs on the IO thread as IO is over, after the receive right has left the IO thread's port set.
    void finish_io();
    void release_send_right_to_peer();
    void stop_io_thread(IOThreadState desired_state);
    void wake_io_thread();
    bool schedule_read_notification_if_needed_locked();
    void write_read_notification_byte();
    void mark_peer_eof();
    void notify_incoming_message_observer(ReadonlyBytes);
    void stop_sending();
    bool request_send_possible_notification();
    void handle_send_possible_notification(u8* buffer);
    enum class SendOutcome {
        Sent,
        // The peer's queue is full. The message keeps its bytes and its rights, to be sent again.
        WouldBlock,
        // The message is dropped, and the peer is to be treated as gone.
        PeerGone,
    };
    SendOutcome send_mach_message(PendingMessage&);
    // Breaks for a message the peer had no business sending, after which the transport takes nothing more from it.
    IterationDecision process_received_message(u8* buffer);

    Core::MachPort m_receive_port;
    Core::MachPort m_send_port;

    Atomic<bool> m_is_open { true };
    // True while release_for_transfer() is moving this transport's rights to a new owner. In that state,
    // shutdown from the old endpoint is part of the handoff and must not be reported as peer EOF.
    Atomic<bool> m_is_being_transferred { false };

    Atomic<IOThreadState> m_io_thread_state { IOThreadState::Running };
    // NB: These two are guarded by the lock of the IO thread that all transports share.
    bool m_needs_attention { false };
    bool m_io_is_finished { false };
    Atomic<bool> m_peer_eof { false };

    // Held by whichever thread sends, which is the thread that posts a message unless the message has to wait. It
    // guards the send right and the send buffer as well.
    Mutex m_send_mutex;
    ConditionVariable m_sent_cv { m_send_mutex };
    // Messages the peer's queue had no room for, in order. While there are any, a newly posted message waits behind
    // them, and the IO thread sends them as the peer makes room.
    Queue<PendingMessage, 64> m_send_backlog;
    bool m_sending_is_over { false };
    Vector<u8> m_send_buffer;
    // Where the kernel says that the peer's full queue has room, from the first time it was full.
    Core::MachPort m_send_possible_notification_port;
    bool m_send_possible_notification_is_requested { false };
    // Set by a thread that found the peer gone as it posted, for the IO thread to tell the owner, as it does when it
    // finds that itself.
    Atomic<bool> m_posting_thread_found_peer_gone { false };

    Mutex m_incoming_mutex;
    ConditionVariable m_incoming_cv { m_incoming_mutex };
    bool m_wait_until_readable_interrupted { false };
    Vector<NonnullOwnPtr<Message>> m_incoming_messages;
    // NB: This has a lock of its own, as the observer may interrupt a wait, which takes m_incoming_mutex.
    Mutex m_incoming_message_observer_mutex;
    Function<void(ReadonlyBytes)> m_incoming_message_observer;
    u64 m_receive_barriers_sent { 0 };
    u64 m_receive_barriers_received { 0 };
    bool m_read_notification_pending { false };

    RefPtr<AutoCloseFileDescriptor> m_notify_hook_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_notify_hook_write_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

}
