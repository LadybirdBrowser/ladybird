/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Queue.h>
#include <LibCore/Socket.h>
#include <LibIPC/Attachment.h>
#include <LibIPC/TransportHandle.h>

namespace IPC {

class TransportSocketWindows {
    AK_MAKE_NONCOPYABLE(TransportSocketWindows);
    AK_MAKE_DEFAULT_MOVABLE(TransportSocketWindows);

public:
    struct Paired {
        NonnullOwnPtr<TransportSocketWindows> local;
        TransportHandle remote_handle;
    };
    static ErrorOr<Paired> create_paired();
    static ErrorOr<NonnullOwnPtr<TransportSocketWindows>> from_socket(NonnullOwnPtr<Core::LocalSocket> socket);

    explicit TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket);

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
    ErrorOr<void> transfer(ReadonlyBytes);

private:
    NonnullOwnPtr<Core::LocalSocket> m_socket;
    ByteBuffer m_unprocessed_bytes;
    int m_peer_pid = -1;
};

}
