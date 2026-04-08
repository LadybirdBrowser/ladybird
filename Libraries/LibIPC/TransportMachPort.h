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
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/Vector.h>
#include <LibCore/MachPort.h>
#include <LibCore/Notifier.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/ConditionVariable.h>
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

    TransportMachPort(Core::MachPort receive_right, Core::MachPort send_right);
    ~TransportMachPort();

    void set_up_read_hook(Function<void()>);
    bool is_open() const;

    void close();
    void close_after_sending_all_pending_messages();

    void wait_until_readable();

    void post_message(Vector<u8> const&, Vector<Attachment>& attachments);

    enum class ShouldShutdown {
        No,
        Yes,
    };
    struct Message {
        Vector<u8> bytes;
        Queue<Attachment> attachments;
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&&);

    ErrorOr<TransportHandle> release_for_transfer();

private:
    static constexpr unsigned int IPC_DATA_MESSAGE_ID = 0x4950C001;
    static constexpr unsigned int IPC_WAKEUP_MESSAGE_ID = 0x4950C003;

    struct PendingMessage {
        Vector<u8> bytes;
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
    void notify_read_available();
    void mark_peer_eof();
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
    Threading::Mutex m_send_mutex;
    Vector<u8> m_send_buffer;

    Threading::Mutex m_incoming_mutex;
    Threading::ConditionVariable m_incoming_cv { m_incoming_mutex };
    Vector<NonnullOwnPtr<Message>> m_incoming_messages;

    RefPtr<AutoCloseFileDescriptor> m_notify_hook_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_notify_hook_write_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

}
