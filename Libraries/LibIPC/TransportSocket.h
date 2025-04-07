/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/UnprocessedFileDescriptors.h>

namespace IPC {

class TransportSocket {
    AK_MAKE_NONCOPYABLE(TransportSocket);
    AK_MAKE_DEFAULT_MOVABLE(TransportSocket);

public:
    static constexpr socklen_t SOCKET_BUFFER_SIZE = 128 * KiB;

    explicit TransportSocket(NonnullOwnPtr<Core::LocalSocket> socket);
    ~TransportSocket();

    void set_up_read_hook(Function<void()>);
    bool is_open() const;
    void close();

    void wait_until_readable();

    ErrorOr<void> transfer_message(ReadonlyBytes, Vector<int, 1> const& unowned_fds);

    enum class ShouldShutdown {
        No,
        Yes,
    };
    struct Message {
        Vector<u8> bytes;
        Vector<File> fds;
    };
    ShouldShutdown read_as_many_messages_as_possible_without_blocking(Function<void(Message)>&& schedule_shutdown);

    // Obnoxious name to make it clear that this is a dangerous operation.
    ErrorOr<int> release_underlying_transport_for_transfer();

    ErrorOr<IPC::File> clone_for_transfer();

private:
    ErrorOr<void> transfer(ReadonlyBytes, Vector<int, 1> const& unowned_fds);

    NonnullOwnPtr<Core::LocalSocket> m_socket;
    ByteBuffer m_unprocessed_bytes;
    UnprocessedFileDescriptors m_unprocessed_fds;
};

}
