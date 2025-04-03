/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/File.h>

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

    ErrorOr<void> transfer(ReadonlyBytes, Vector<int, 1> const& unowned_fds);

    struct [[nodiscard]] ReadResult {
        Vector<u8> bytes;
        Vector<int> fds;
    };
    ReadResult read_as_much_as_possible_without_blocking(Function<void()> schedule_shutdown);

    // Obnoxious name to make it clear that this is a dangerous operation.
    ErrorOr<int> release_underlying_transport_for_transfer();

    ErrorOr<IPC::File> clone_for_transfer();

private:
    NonnullOwnPtr<Core::LocalSocket> m_socket;
};

}
