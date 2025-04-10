/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Queue.h>
#include <LibCore/Socket.h>
#include <LibIPC/UnprocessedFileDescriptors.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/MutexProtected.h>
#include <LibThreading/RWLock.h>
#include <LibThreading/Thread.h>

namespace IPC {

class AutoCloseFileDescriptor : public RefCounted<AutoCloseFileDescriptor> {
public:
    AutoCloseFileDescriptor(int fd)
        : m_fd(fd)
    {
    }

    ~AutoCloseFileDescriptor()
    {
        if (m_fd != -1)
            (void)Core::System::close(m_fd);
    }

    int value() const { return m_fd; }

    int take_fd()
    {
        int fd = m_fd;
        m_fd = -1;
        return fd;
    }

private:
    int m_fd;
};

class SendQueue : public AtomicRefCounted<SendQueue> {
public:
    enum class Running {
        No,
        Yes,
    };
    Running block_until_message_enqueued();
    void stop();

    void enqueue_message(Vector<u8>&& bytes, Vector<int>&& fds);
    struct BytesAndFds {
        Vector<u8> bytes;
        Vector<int> fds;
    };
    BytesAndFds dequeue(size_t max_bytes);
    void return_unsent_data_to_front_of_queue(ReadonlyBytes const& bytes, Vector<int> const& fds);

private:
    Vector<u8> m_bytes;
    Vector<int> m_fds;
    Threading::Mutex m_mutex;
    Threading::ConditionVariable m_condition { m_mutex };
    bool m_running { true };
};

class TransportSocket {
    AK_MAKE_NONCOPYABLE(TransportSocket);
    AK_MAKE_NONMOVABLE(TransportSocket);

public:
    static constexpr socklen_t SOCKET_BUFFER_SIZE = 128 * KiB;

    explicit TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket);
    ~TransportSocket();

    void set_up_read_hook(Function<void()>);
    bool is_open() const;
    void close();

    void wait_until_readable();

    void post_message(Vector<u8> const&, Vector<NonnullRefPtr<AutoCloseFileDescriptor>> const&);

    enum class ShouldShutdown {
        No,
        Yes,
    };
    struct Message {
        Vector<u8> bytes;
        Vector<File> fds;
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message)>&&);

    // Obnoxious name to make it clear that this is a dangerous operation.
    ErrorOr<int> release_underlying_transport_for_transfer();

    ErrorOr<IPC::File> clone_for_transfer();

private:
    static ErrorOr<void> send_message(Core::LocalSocket&, ReadonlyBytes& bytes, Vector<int>& unowned_fds);

    NonnullOwnPtr<Core::LocalSocket> m_socket;
    mutable Threading::RWLock m_socket_rw_lock;
    ByteBuffer m_unprocessed_bytes;
    UnprocessedFileDescriptors m_unprocessed_fds;

    // After file descriptor is sent, it is moved to the wait queue until an acknowledgement is received from the peer.
    // This is necessary to handle a specific behavior of the macOS kernel, which may prematurely garbage-collect the file
    // descriptor contained in the message before the peer receives it. https://openradar.me/9477351
    Queue<NonnullRefPtr<AutoCloseFileDescriptor>> m_fds_retained_until_received_by_peer;

    RefPtr<Threading::Thread> m_send_thread;
    RefPtr<SendQueue> m_send_queue;
};

}
