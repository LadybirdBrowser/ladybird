/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2024, stasoid <stasoid@yahoo.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Socket.h>
#include <LibIPC/File.h>

namespace IPC {

class TransportSocketWindows {
    AK_MAKE_NONCOPYABLE(TransportSocketWindows);
    AK_MAKE_DEFAULT_MOVABLE(TransportSocketWindows);

public:
    explicit TransportSocketWindows(NonnullOwnPtr<Core::LocalSocket> socket);
    ~TransportSocketWindows();

    void set_peer_pid(int pid);
    void set_up_read_hook(Function<void()>);
    bool is_open() const;
    void close();

    void wait_until_readable();

    ErrorOr<void> transfer(Bytes, Vector<size_t> const& handle_offsets);

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
    void* m_peer_process_handle = 0;
};

}
