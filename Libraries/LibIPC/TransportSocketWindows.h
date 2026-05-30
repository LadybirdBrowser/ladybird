/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/MemoryStream.h>
#include <AK/Queue.h>
#include <LibCore/Socket.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/TransportHandle.h>
#include <LibSync/ConditionVariable.h>
#include <LibSync/Mutex.h>
#include <LibThreading/Forward.h>
#include <LibThreading/Thread.h>

namespace IPC {

class SendQueue : public AtomicRefCounted<SendQueue> {
public:
    void enqueue_message(ReadonlyBytes header, ReadonlyBytes attachments, ReadonlyBytes payload);
    struct Bytes {
        Vector<u8> bytes;
    };
    Bytes peek(size_t max_bytes);
    void discard(size_t bytes_count);

private:
    AllocatingMemoryStream m_stream;
    Sync::Mutex m_mutex;
};

class TransportSocketWindows {
    AK_MAKE_NONCOPYABLE(TransportSocketWindows);
    AK_MAKE_NONMOVABLE(TransportSocketWindows);

public:
    struct Paired {
        NonnullOwnPtr<TransportSocketWindows> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();
    static ErrorOr<NonnullOwnPtr<TransportSocketWindows>> from_socket(NonnullOwnPtr<Core::LocalSocket> socket);

    explicit TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket);
    ~TransportSocketWindows();

    void set_peer_pid(int pid);
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
    ErrorOr<Vector<u8>> serialize_attachments(Vector<Attachment>&);
    Attachment deserialize_attachment(ReadonlyBytes&);

    enum class TransferState {
        Continue,
        SocketClosed,
    };
    [[nodiscard]] TransferState transfer_data(ReadonlyBytes& bytes);

    enum class IOThreadState {
        Running,
        SendPendingMessagesAndStop,
        Stopped,
    };
    intptr_t io_thread_loop();
    void stop_io_thread(IOThreadState desired_state);
    void wake_io_thread();
    void read_incoming_messages();
    void notify_read_available();

    NonnullOwnPtr<Core::LocalSocket> m_socket;
    int m_peer_pid = -1;

    RefPtr<Threading::Thread> m_io_thread;
    RefPtr<SendQueue> m_send_queue;
    Atomic<IOThreadState> m_io_thread_state { IOThreadState::Running };
    Atomic<bool> m_is_being_transferred { false };
    Atomic<bool> m_peer_eof { false };

    ByteBuffer m_unprocessed_bytes;
    Queue<Attachment> m_unprocessed_attachments;
    Sync::Mutex m_incoming_mutex;
    Sync::ConditionVariable m_incoming_cv { m_incoming_mutex };
    Vector<NonnullOwnPtr<Message>> m_incoming_messages;

    RefPtr<AutoCloseFileDescriptor> m_wakeup_io_thread_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_wakeup_io_thread_write_fd;

    RefPtr<AutoCloseFileDescriptor> m_notify_hook_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_notify_hook_write_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

}
