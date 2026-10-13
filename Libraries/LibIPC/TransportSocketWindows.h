/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <AK/Queue.h>
#include <AK/kmalloc.h>
#include <LibCore/Socket.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/HandleBroker.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

// A Windows socket carries bytes alone, so the attachments of a message are parked with the handle broker and the
// message carries their token. The Browser copies them out of this process and into the peer, so neither needs any
// right over the other.
class TransportSocketWindows {
    AK_MAKE_NONCOPYABLE(TransportSocketWindows);
    AK_MAKE_NONMOVABLE(TransportSocketWindows);

public:
    AK_ALLOC_WITH_KMALLOC;

    struct Paired {
        NonnullOwnPtr<TransportSocketWindows> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();
    static ErrorOr<NonnullOwnPtr<TransportSocketWindows>> from_socket(NonnullOwnPtr<Core::LocalSocket> socket);

    explicit TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket);
    ~TransportSocketWindows();

    void set_up_read_hook(Function<void()>);
    bool is_open() const;
    void close();
    void close_after_sending_all_pending_messages();

    // Fails a send that the peer has no room for, rather than wait for the peer to read.
    void set_disconnects_when_peer_stops_reading(bool value) { m_disconnects_when_peer_stops_reading = value; }

    void wait_until_readable();

    // Both are no-ops here: post_message() writes to the socket before it returns, and the read path below reads the
    // socket itself, so neither direction has a queue that can lag behind another transport.
    void flush() { }
    void wait_until_incoming_is_current() { }

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
    struct MessageHeader;

    ErrorOr<void> transfer(ReadonlyBytes);

    NonnullOwnPtr<Core::LocalSocket> m_socket;
    Atomic<bool> m_socket_is_open { true };
    Mutex m_send_mutex;
    ByteBuffer m_unprocessed_bytes;
    // Names this end to the handle broker, so what it parked can be forgotten once the peer can no longer read it.
    HandleBrokerChannel m_handle_broker_channel { 0 };
    bool m_has_parked_attachments { false };
    bool m_disconnects_when_peer_stops_reading { false };
};

}
