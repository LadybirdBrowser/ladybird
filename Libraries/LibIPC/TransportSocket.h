/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Mutex.h>
#include <AK/Platform.h>
#include <AK/Queue.h>
#include <AK/SinglyLinkedList.h>
#include <AK/SinglyLinkedListSizePolicy.h>
#include <LibCore/Socket.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/AutoCloseFileDescriptor.h>
#include <LibIPC/Forward.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Forward.h>

// A descriptor is closed once written, as the kernel then holds its own reference to the file for the message in
// flight. However, the macOS kernel may collect it while it is still in the remote message queue
// (https://openradar.me/9477351), so we must use TransportMachPort there.
#if defined(AK_OS_MACOS) || defined(AK_OS_IOS)
#    error "TransportSocket must not be used on Darwin"
#endif

namespace IPC {

// NB: One sendmsg() carries a limited number of descriptors, and at least one byte with them. A message with more
//     descriptors sends the rest ahead of it, in Attachments frames that have no payload. A Payload frame's fd_count
//     counts every descriptor of its message, including those that arrived ahead of it. A SharedMemoryPayload frame
//     has no bytes, and its shared memory is the last descriptor of its message.
struct SocketMessageHeader {
    enum class Type : u8 {
        Payload = 0,
        Attachments = 1,
        SharedMemoryPayload = 2,
    };
    Type type { Type::Payload };
    u32 payload_size { 0 };
    u32 fd_count { 0 };
};

class SendQueue : public AtomicRefCounted<SendQueue> {
public:
    // Answers whether the queue was empty before this message.
    bool enqueue_message(SocketMessageHeader, MessageDataType payload, Vector<NonnullRefPtr<AutoCloseFileDescriptor>>&& fds);
    struct BytesAndFds {
        Vector<u8> bytes;
        Vector<int> fds;
    };
    BytesAndFds peek(size_t max_bytes);
    void discard(size_t bytes_count, size_t fds_count);
    void wait_until_drained();
    // Called when the IO thread stops, so that a waiter is not left waiting for a write that will never happen.
    void release_drain_waiters();

private:
    struct QueuedMessage {
        SocketMessageHeader header;
        MessageDataType payload;
        Vector<NonnullRefPtr<AutoCloseFileDescriptor>> unsent_fds;
        size_t start_offset { 0 };

        size_t size() const { return sizeof(SocketMessageHeader) + payload.size(); }
    };

    SinglyLinkedList<QueuedMessage, AK::DefaultSizeCalculationPolicy> m_queued_messages;
    size_t m_queued_byte_count { 0 };
    Mutex m_mutex;
    ConditionVariable m_drained_cv { m_mutex };
    bool m_drain_waiters_released { false };
};

class TransportSocket {
    AK_MAKE_NONCOPYABLE(TransportSocket);
    AK_MAKE_NONMOVABLE(TransportSocket);

public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr socklen_t SOCKET_BUFFER_SIZE = 128 * KiB;
    static constexpr size_t MAX_INLINE_PAYLOAD_SIZE = 64 * KiB;

    struct Paired {
        NonnullOwnPtr<TransportSocket> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();
    static ErrorOr<NonnullOwnPtr<TransportSocket>> from_socket(NonnullOwnPtr<Core::LocalSocket> socket);
    static ErrorOr<NonnullOwnPtr<TransportSocket>> adopt_fd(int fd);
    // The bytes and attachments are what the last owner read off the socket without seeing the end of the message.
    static ErrorOr<NonnullOwnPtr<TransportSocket>> adopt_fd(int fd, ByteBuffer unprocessed_bytes, Vector<Attachment> unprocessed_attachments);

    explicit TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket, ByteBuffer unprocessed_bytes = {}, Vector<Attachment> unprocessed_attachments = {});
    ~TransportSocket();

    // Makes the transports that this thread creates from now on share one IO thread with all others made this way, or
    // get an IO thread each, as they do by default. A thread per transport is what a latency-critical connection wants.
    // Sharing is for the many connections of the agents of a process, of which each is rarely busy.
    static void set_share_io_thread_on_this_thread(bool);

    void set_up_read_hook(Function<void()>);

    // Sets what the IO thread shows every message that arrives, before the transport's owner gets to see it, and empty
    // bytes once the peer is gone. This is for a thread that does not get back to its event loop on its own, like one
    // stuck in script. It runs while the IO thread holds a lock, so it should only have a look.
    void set_incoming_message_observer(Function<void(ReadonlyBytes)>);

    bool is_open() const;

    void close();
    void close_after_sending_all_pending_messages();

    void wait_until_readable();

    // Makes wait_until_readable() return, even with nothing to read, now or if none is under way, the next time. Any
    // thread may call this.
    void interrupt_wait_until_readable();

    // Wait until everything posted so far has been written to the socket.
    void flush();

    // Wait until everything the peer has already written has been parsed into the incoming queue.
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

    // Whole messages that have been read stay here for the old owner to take, and anything newer reaches the new owner.
    // A message read in part goes to the new owner, along with the handle.
    ErrorOr<TransportHandle> release_for_transfer();

    // Test seam: When set to a non-zero value, the IO thread wakes the consumer and then pauses for the given duration
    // on EOF — before the messages it has just read are parsed and appended. That forces a consumer drain into the
    // narrow window between observing EOF and the final message becoming available — which is otherwise hit only under
    // rare timing. No-op in production.
    static void set_eof_drain_window_for_test(u32 milliseconds);

    // Test seam: When set, the IO thread skips its in-loop read on POLLIN — modelling a stop path that ends the loop
    // without having read a message already buffered on the socket (e.g. SocketClosed from a failed send). Exercises
    // the loop-exit drain. No-op in production.
    static void set_skip_inloop_read_for_test(bool);

private:
    friend class SharedIOThread;

    enum class TransferState {
        Continue,
        SocketClosed,
    };
    [[nodiscard]] TransferState transfer_data(ReadonlyBytes& bytes, Vector<int>& fds);
    [[nodiscard]] bool send_queued_messages_on_this_thread();

    static ErrorOr<void> send_message(Core::LocalSocket&, ReadonlyBytes& bytes, Vector<int>& unowned_fds);

    enum class IOThreadState {
        Running,
        SendPendingMessagesAndStop,
        Stopped,
    };
    intptr_t io_thread_loop();
    // One round of IO: what to poll the socket for, or nothing once IO is over, and what to do about what poll() found.
    Optional<short> events_to_poll();
    void handle_polled_events(short events);
    // Runs on the IO thread as IO is over. A transport on the shared IO thread can be gone once this returns.
    void finish_io();
    void stop_io_thread(IOThreadState desired_state);
    void wake_io_thread();
    enum class MoreToRead {
        No,
        Yes,
    };
    [[nodiscard]] MoreToRead read_incoming_messages();
    void parse_unprocessed_messages(Vector<NonnullOwnPtr<Message>>&);
    void notify_read_available();
    void notify_incoming_message_observer(ReadonlyBytes);
    bool incoming_is_behind_socket() const;

    NonnullOwnPtr<Core::LocalSocket> m_socket;
    Atomic<bool> m_socket_is_open { true };

    bool m_shares_io_thread { false };
    RefPtr<Threading::Thread> m_io_thread;
    RefPtr<SendQueue> m_send_queue;
    // Held by the thread that takes bytes from the send queue and writes them to the socket, which keeps them in order.
    Mutex m_send_mutex;
    Atomic<IOThreadState> m_io_thread_state { IOThreadState::Running };
    Atomic<bool> m_is_being_transferred { false };
    Atomic<bool> m_peer_eof { false };
    ByteBuffer m_unprocessed_bytes;
    Queue<Attachment> m_unprocessed_attachments;
    Mutex m_incoming_mutex;
    ConditionVariable m_incoming_cv { m_incoming_mutex };
    Vector<NonnullOwnPtr<Message>> m_incoming_messages;
    // NB: This has a lock of its own, as the observer may interrupt a wait, which takes m_incoming_mutex.
    Mutex m_incoming_message_observer_mutex;
    Function<void(ReadonlyBytes)> m_incoming_message_observer;
    // True while the IO thread is between reading the socket and appending what it read.
    bool m_read_in_progress { false };
    // Consumer-visible EOF, guarded by m_incoming_mutex. Distinct from m_peer_eof. This is set only after the final
    // batch of messages have been parsed and appended to m_incoming_messages under the same lock.
    bool m_incoming_eof { false };
    bool m_receive_loop_finished { false };
    bool m_wait_until_readable_interrupted { false };

    static Atomic<u32> s_eof_drain_window_for_test_ms;
    static Atomic<bool> s_skip_inloop_read_for_test;

    RefPtr<AutoCloseFileDescriptor> m_wakeup_io_thread_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_wakeup_io_thread_write_fd;

    RefPtr<AutoCloseFileDescriptor> m_notify_hook_read_fd;
    RefPtr<AutoCloseFileDescriptor> m_notify_hook_write_fd;
    RefPtr<Core::Notifier> m_read_hook_notifier;
    Function<void()> m_on_read_hook;
};

}
