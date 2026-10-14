/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/Queue.h>
#include <AK/RefPtr.h>
#include <AK/kmalloc.h>
#include <LibCore/Forward.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/Forward.h>
#include <LibIPC/HandleBroker.h>
#include <LibIPC/ReceivedMessageBytes.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

// Moves messages over a duplex named pipe, or over a socket for the few connections that arrive on one. Neither can
// carry a handle, so the attachments of a message are parked with the handle broker and the message carries their
// token. The Browser copies them out of this process and into the peer, so neither needs any right over the other.
class TransportWindows {
    AK_MAKE_NONCOPYABLE(TransportWindows);
    AK_MAKE_NONMOVABLE(TransportWindows);

public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr size_t MAX_INLINE_PAYLOAD_SIZE = 1 * MiB;

    struct Paired {
        NonnullOwnPtr<TransportWindows> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();
    static ErrorOr<NonnullOwnPtr<TransportWindows>> from_socket(NonnullOwnPtr<Core::LocalSocket>);
    static ErrorOr<NonnullOwnPtr<TransportWindows>> adopt_fd(int handle);

    ~TransportWindows();

    void set_up_read_hook(Function<void()>);
    bool is_open() const;
    void close();
    void close_after_sending_all_pending_messages();

    // Fails a send that the peer has no room for, rather than wait for the peer to read.
    void set_disconnects_when_peer_stops_reading(bool value) { m_disconnects_when_peer_stops_reading = value; }

    void wait_until_readable();

    // Both are no-ops here: post_message() writes to the pipe before it returns, and the read path below reads the
    // pipe itself, so neither direction has a queue that can lag behind another transport.
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
    enum class Kind : u8 {
        Pipe,
        Socket,
    };
    struct MessageHeader;
    enum class ReadOutcome : u8 {
        Read,
        WouldBlock,
        EndOfStream,
        Failed,
    };

    TransportWindows(int handle, Kind);

    ErrorOr<void> transfer(ReadonlyBytes);
    ErrorOr<size_t> write_some(ReadonlyBytes);
    ReadOutcome read_some_without_waiting(Bytes, size_t& bytes_read);
    void stop_watching();

    int m_handle { -1 };
    Kind m_kind { Kind::Pipe };
    Atomic<bool> m_is_open { true };
    // Set once a send has found the peer gone, so is_open() says so before the read side has reached its end.
    Atomic<bool> m_peer_has_hung_up { false };
    Mutex m_send_mutex;
    ByteBuffer m_unprocessed_bytes;
    RefPtr<Core::Notifier> m_read_notifier;
    Function<void()> m_on_read_hook;
    // Names this end to the handle broker, so what it parked can be forgotten once the peer can no longer read it.
    HandleBrokerChannel m_handle_broker_channel { 0 };
    bool m_has_parked_attachments { false };
    bool m_disconnects_when_peer_stops_reading { false };
};

}
