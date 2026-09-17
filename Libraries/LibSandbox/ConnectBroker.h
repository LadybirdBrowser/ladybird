/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Function.h>
#include <AK/Noncopyable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Platform.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

#if defined(AK_OS_LINUX)
#    include <pthread.h>

namespace Sandbox {

namespace Detail {

static constexpr u32 connect_broker_magic = 0x4c42524bu;

// The kernel gives sun_path this many bytes, so no legal target can be longer.
static constexpr size_t connect_broker_maximum_path_length = 108;

// A helper cannot make a socket, so it asks for one, configures it as it likes, and then asks for
// that same socket to be connected. Connecting the caller's own socket is what keeps the options it
// set beforehand, and means no descriptor of the caller's is ever replaced behind its back.
enum class ConnectBrokerOperation : u32 {
    CreateSocket,
    Connect,
};

struct ConnectBrokerRequest {
    u32 magic;
    u32 operation;
    i32 socket_domain;
    i32 socket_type;
    i32 socket_protocol;
    u32 path_length;
    char path[connect_broker_maximum_path_length];
};

struct ConnectBrokerResponse {
    u32 magic;
    i32 error;
};

}

// Seccomp cannot read the address that connect() is given, and Landlock does not mediate UNIX
// sockets, so a helper that may connect at all may reach every socket in the user's session.
// Helpers therefore get no socket() and no connect() of their own. They ask the Browser, which is
// not sandboxed, to connect on their behalf, and it hands back a connected descriptor only for a
// path that it put on the allowlist itself.
//
// Firefox answers the same problem the same way; see security/sandbox/linux/broker in its tree.
class ConnectBroker {
    AK_MAKE_NONCOPYABLE(ConnectBroker);
    AK_MAKE_NONMOVABLE(ConnectBroker);

public:
    AK_ALLOC_WITH_KMALLOC;

    // The allowed paths are worked out before a helper starts, and what a helper asks for can only
    // be known later. When a request names something not on the list, the broker asks for the list
    // again, a bounded number of times, so an endpoint that could not be named at the start is
    // still reachable once it can be.
    using RefreshAllowedPaths = Function<Vector<ByteString>()>;

    static ErrorOr<NonnullOwnPtr<ConnectBroker>> create(Vector<ByteString> allowed_paths, RefreshAllowedPaths = {});
    ~ConnectBroker();

    // Hand this to the helper process, which passes it to set_connect_broker_fd().
    int helper_fd() const { return m_helper_fd; }

private:
    ConnectBroker(int broker_fd, int helper_fd, Vector<ByteString> allowed_paths, RefreshAllowedPaths);

    enum class WaitResult {
        Ready,
        TimedOut,
        ShuttingDown,
        Failed,
    };

    static void* run(void*);
    void serve();
    bool is_allowed(StringView path);
    WaitResult wait_until_ready(int fd, short events);
    int create_socket(Detail::ConnectBrokerRequest const&);
    i32 connect_socket(int socket_fd, Detail::ConnectBrokerRequest const&);
    void send_response(int reply_fd, i32 error, int passed_fd);

    int m_broker_fd { -1 };
    int m_helper_fd { -1 };
    // Written to when the broker is going away, so a connection that is taking too long can be
    // abandoned instead of holding the thread, and with it whoever is waiting to join it.
    int m_shutdown_pipe[2] { -1, -1 };
    Vector<ByteString> m_allowed_paths;
    RefreshAllowedPaths m_refresh_allowed_paths;
    int m_refreshes_remaining { 4 };
    pthread_t m_thread {};
    bool m_thread_started { false };
};

// Called in the helper process, before the seccomp policy is installed.
void set_connect_broker_fd(int);

}

#endif
