/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Queue.h>
#include <LibCore/Socket.h>
#include <LibIPC/File.h>

namespace IPC {

class Message;

class TransportSocketWindows {
    AK_MAKE_NONCOPYABLE(TransportSocketWindows);
    AK_MAKE_DEFAULT_MOVABLE(TransportSocketWindows);

public:
    // IPC::Connection path: Set a decoder to parse raw bytes into IPC::Message objects
    // and a handler to receive decoded messages.
    // NOTE: Windows does not use a separate I/O thread; decoding happens on the main thread.
    using MessageDecoder = Function<OwnPtr<IPC::Message>(ReadonlyBytes, Queue<File>&)>;
    using MessageHandler = Function<void(NonnullOwnPtr<IPC::Message>)>;
    using PeerClosedHandler = Function<void()>;

    void set_message_decoder(MessageDecoder decoder);
    void set_message_handler(MessageHandler handler);
    void set_peer_closed_handler(PeerClosedHandler handler);
    void start();

    explicit TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket);

    void set_peer_pid(int pid);
    void set_up_read_hook(Function<void()>);
    bool is_open() const;
    void close();
    void close_after_sending_all_pending_messages();

    void wait_until_readable();

    ErrorOr<void> transfer_message(ReadonlyBytes, Vector<size_t> const& handle_offsets);

    enum class ShouldShutdown {
        No,
        Yes,
    };
    struct Message {
        Vector<u8> bytes;
        Queue<File> fds; // always empty, present to avoid OS #ifdefs in Connection.cpp
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message&&)>&&);

    // Obnoxious name to make it clear that this is a dangerous operation.
    ErrorOr<int> release_underlying_transport_for_transfer();

    ErrorOr<IPC::File> clone_for_transfer();

private:
    ErrorOr<void> duplicate_handles(Bytes, Vector<size_t> const& handle_offsets);
    ErrorOr<void> transfer(ReadonlyBytes);

private:
    NonnullOwnPtr<Core::LocalSocket> m_socket;
    ByteBuffer m_unprocessed_bytes;
    int m_peer_pid = -1;

    MessageDecoder m_decoder;
    MessageHandler m_message_handler;
    PeerClosedHandler m_peer_closed_handler;
};

}
