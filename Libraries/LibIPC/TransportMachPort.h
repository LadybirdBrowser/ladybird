/*
 * Copyright (c) 2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

#if !defined(AK_OS_MACOS)
#    error "TransportMachPort is only available on macOS"
#endif

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/MachPort.h>
#include <LibCore/Notifier.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/Forward.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Thread.h>

namespace IPC {

class TransportMachPort {
    AK_MAKE_NONCOPYABLE(TransportMachPort);
    AK_MAKE_NONMOVABLE(TransportMachPort);

public:
    struct Paired {
        NonnullOwnPtr<TransportMachPort> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();

    static void raise_receive_queue_limit(Core::MachPort const&);

    TransportMachPort(Core::MachPort receive_right, Core::MachPort send_right);
    ~TransportMachPort();

    void set_up_read_hook(Function<void()>);
    bool is_open() const;

    void close();
    void close_after_sending_all_pending_messages();

    void wait_until_readable();

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
        ReceivedMessageBytes bytes;
        Queue<Attachment> attachments;
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&&);

    ErrorOr<TransportHandle> release_for_transfer();

private:
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

    intptr_t io_thread_loop();
    void stop_io_thread(IOThreadState desired_state);
    void wake_io_thread();
    bool schedule_read_notification_if_needed_locked();
    void write_read_notification_byte();
    void mark_peer_eof();
    void release_send_waiters();
    void send_mach_message(PendingMessage&);
    void process_received_message(u8* buffer);

    Core::MachPort m_receive_port;
    Core::MachPort m_send_port;
    Core::MachPort m_port_set;
    Core::MachPort m_wakeup_receive_port;
    Core::MachPort m_wakeup_send_port;

    Atomic<bool> m_is_open { true };
    // True while release_for_transfer() is moving this transport's rights to a new owner. In that state,
    // shutdown from the old endpoint is part of the handoff and must not be reported as peer EOF.
    Atomic<bool> m_is_being_transferred { false };

    RefPtr<Threading::Thread> m_io_thread;
    Atomic<IOThreadState> m_io_thread_state { IOThreadState::Running };
    Atomic<bool> m_peer_eof { false };

    Vector<PendingMessage> m_pending_send_messages;
    Mutex m_send_mutex;
    ConditionVariable m_sent_cv { m_send_mutex };
    // True while the IO thread is sending a batch it has already taken off m_pending_send_messages.
    bool m_send_in_progress { false };
    bool m_send_waiters_released { false };
    Vector<u8> m_send_buffer;

    Mutex m_incoming_mutex;
    ConditionVariable m_incoming_cv { m_incoming_mutex };
    Vector<NonnullOwnPtr<Message>> m_incoming_messages;
    u64 m_receive_barriers_sent { 0 };
    u64 m_receive_barriers_received { 0 };
    bool m_read_notification_pending { false };

    RefPtr<AutoCloseFileDescriptor> m_notify_hook_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_notify_hook_write_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

}
