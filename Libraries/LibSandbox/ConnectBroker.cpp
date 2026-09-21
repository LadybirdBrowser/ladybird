/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSandbox/ConnectBroker.h>

#if defined(AK_OS_LINUX)
#    include <AK/ScopeGuard.h>
#    include <errno.h>
#    include <fcntl.h>
#    include <poll.h>
#    include <string.h>
#    include <sys/socket.h>
#    include <sys/un.h>
#    include <unistd.h>

namespace Sandbox {

using namespace Detail;

// Every request arrives with the descriptor that its answer goes back on, so the broker never has
// to match answers to requests and one helper's thread can never be confused by another one.
enum class ReceiveResult {
    Received,
    Malformed,
    PeerClosed,
};

static ReceiveResult receive_request(int fd, ConnectBrokerRequest& request, int& reply_fd, int& socket_fd)
{
    reply_fd = -1;
    socket_fd = -1;

    iovec io {
        .iov_base = &request,
        .iov_len = sizeof(request),
    };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(2 * sizeof(int))];
    } control {};

    msghdr message {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &io,
        .msg_iovlen = 1,
        .msg_control = &control,
        .msg_controllen = sizeof(control),
        .msg_flags = 0,
    };

    ssize_t received = 0;
    do {
        received = recvmsg(fd, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    if (received < 0)
        return ReceiveResult::PeerClosed;

    // Take every descriptor the message carries before deciding anything about it, so that no path
    // out of here can leave one behind in the Browser. The buffer holds more than the one we ask
    // for: the padding CMSG_SPACE adds for a single descriptor is exactly the room a second one
    // needs, so a helper can always attach two, and a zero-length packet can carry them as well.
    static constexpr size_t maximum_received_fds = CMSG_SPACE(2 * sizeof(int)) / sizeof(int);
    int received_fds[maximum_received_fds];
    size_t received_fd_count = 0;

    for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
            continue;
        if (header->cmsg_len < CMSG_LEN(0))
            continue;

        auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count && received_fd_count < maximum_received_fds; ++i) {
            memcpy(&received_fds[received_fd_count], CMSG_DATA(header) + i * sizeof(int), sizeof(int));
            ++received_fd_count;
        }
    }

    auto close_received_fds = [&] {
        for (size_t i = 0; i < received_fd_count; ++i)
            close(received_fds[i]);
    };

    // A zero-length read with nothing attached is the helper going away. One with descriptors
    // attached is just a malformed request, and the broker keeps serving.
    if (received == 0 && received_fd_count == 0)
        return ReceiveResult::PeerClosed;

    // A connect carries the socket to connect as well as the channel to answer on; nothing else
    // carries more than the channel.
    auto is_connect = received == static_cast<ssize_t>(sizeof(request))
        && request.magic == connect_broker_magic
        && request.operation == static_cast<u32>(ConnectBrokerOperation::Connect);
    size_t expected_fd_count = is_connect ? 2 : 1;

    if (received_fd_count != expected_fd_count
        || static_cast<size_t>(received) != sizeof(request)
        || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        close_received_fds();
        return ReceiveResult::Malformed;
    }

    reply_fd = received_fds[0];
    socket_fd = is_connect ? received_fds[1] : -1;
    return ReceiveResult::Received;
}

ErrorOr<NonnullOwnPtr<ConnectBroker>> ConnectBroker::create(Vector<ByteString> allowed_paths, RefreshAllowedPaths refresh_allowed_paths)
{
    int fds[2] {};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) < 0)
        return Error::from_syscall("socketpair"sv, errno);

    auto broker = adopt_own(*new ConnectBroker(fds[0], fds[1], move(allowed_paths), move(refresh_allowed_paths)));

    if (pipe2(broker->m_shutdown_pipe, O_CLOEXEC) < 0)
        return Error::from_syscall("pipe2"sv, errno);

    if (auto error = pthread_create(&broker->m_thread, nullptr, &ConnectBroker::run, broker.ptr()); error != 0)
        return Error::from_syscall("pthread_create"sv, error);
    broker->m_thread_started = true;

    return broker;
}

ConnectBroker::ConnectBroker(int broker_fd, int helper_fd, Vector<ByteString> allowed_paths, RefreshAllowedPaths refresh_allowed_paths)
    : m_broker_fd(broker_fd)
    , m_helper_fd(helper_fd)
    , m_allowed_paths(move(allowed_paths))
    , m_refresh_allowed_paths(move(refresh_allowed_paths))
{
}

// Nothing here may block on a descriptor the helper handed us. It chose that descriptor, so it can
// choose one whose send buffer is already full, and a blocking send would hand it this thread and
// the shutdown that waits on the thread.
void ConnectBroker::send_response(int reply_fd, i32 error, int connected_fd)
{
    ConnectBrokerResponse response {
        .magic = connect_broker_magic,
        .error = error,
    };

    iovec io {
        .iov_base = &response,
        .iov_len = sizeof(response),
    };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
    } control {};

    msghdr message {
        .msg_name = nullptr,
        .msg_namelen = 0,
        .msg_iov = &io,
        .msg_iovlen = 1,
        .msg_control = nullptr,
        .msg_controllen = 0,
        .msg_flags = 0,
    };

    if (connected_fd >= 0) {
        message.msg_control = &control;
        message.msg_controllen = sizeof(control);

        auto* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &connected_fd, sizeof(connected_fd));
    }

    // There is nothing to wait for. The reply socket is one the helper made moments ago and is
    // blocked reading, so it is only ever full because the helper filled it, and waiting on that
    // would be waiting on the one party that gains from us doing so. Drop the answer instead; the
    // helper has denied itself a reply it asked for, which is its own business.
    ssize_t sent = 0;
    do {
        sent = sendmsg(reply_fd, &message, MSG_NOSIGNAL | MSG_DONTWAIT);
    } while (sent < 0 && errno == EINTR);
}

// Waits for the descriptor, for shutdown, or for the deadline, whichever arrives first.
ConnectBroker::WaitResult ConnectBroker::wait_until_ready(int fd, short events)
{
    static constexpr int timeout_in_milliseconds = 5000;

    for (;;) {
        pollfd descriptors[] = {
            { .fd = fd, .events = events, .revents = 0 },
            { .fd = m_shutdown_pipe[0], .events = POLLIN, .revents = 0 },
        };

        auto ready = poll(descriptors, 2, timeout_in_milliseconds);
        if (ready < 0 && errno == EINTR)
            continue;
        if (ready < 0)
            return WaitResult::Failed;
        if (ready == 0)
            return WaitResult::TimedOut;
        if ((descriptors[1].revents & (POLLIN | POLLHUP)) != 0)
            return WaitResult::ShuttingDown;
        return WaitResult::Ready;
    }
}

ConnectBroker::~ConnectBroker()
{
    // This runs on the event loop, so the thread has to be on its way out before we join it.
    // Shutting the socket down releases a recvmsg() that is already blocked on it, which closing
    // the descriptor would not do, and the pipe releases a connection that is still being waited
    // on. Between them there is nothing left for the thread to be stuck in.
    if (m_broker_fd >= 0)
        shutdown(m_broker_fd, SHUT_RDWR);
    if (m_shutdown_pipe[1] >= 0) {
        char const byte = 0;
        (void)!write(m_shutdown_pipe[1], &byte, 1);
    }

    if (m_thread_started)
        pthread_join(m_thread, nullptr);

    for (auto* fd : { &m_broker_fd, &m_helper_fd, &m_shutdown_pipe[0], &m_shutdown_pipe[1] }) {
        if (*fd >= 0)
            close(*fd);
        *fd = -1;
    }
}

// The helper can configure a stream or seqpacket socket, but only the broker can connect it.
// Datagram sockets could send to arbitrary addresses without going through the connect broker.
int ConnectBroker::create_socket(ConnectBrokerRequest const& request)
{
    if (request.socket_domain != AF_UNIX)
        return -EAFNOSUPPORT;

    auto socket_type = request.socket_type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (socket_type != SOCK_STREAM && socket_type != SOCK_SEQPACKET)
        return -ESOCKTNOSUPPORT;

    // Exactly the socket that was asked for, so it blocks or does not block as the caller intended.
    // Connecting is done without blocking regardless, but that is arranged around the connect and
    // put back afterwards rather than baked into the socket here.
    //
    // Close-on-exec is ours to decide: it keeps this descriptor out of anything the Browser spawns
    // while it is in flight. The helper sets its own preference on the copy it receives.
    auto socket_fd = socket(AF_UNIX, request.socket_type | SOCK_CLOEXEC, request.socket_protocol);
    if (socket_fd < 0)
        return -errno;
    return socket_fd;
}

// Connects the socket the helper was given and has since configured, so whatever it set on that
// socket still applies and no descriptor of its own is replaced underneath it.
i32 ConnectBroker::connect_socket(int socket_fd, ConnectBrokerRequest const& request)
{
    // Only a socket of the kind we hand out, and only one that is not connected already.
    int domain = 0;
    socklen_t domain_length = sizeof(domain);
    if (getsockopt(socket_fd, SOL_SOCKET, SO_DOMAIN, &domain, &domain_length) < 0)
        return errno;
    if (domain != AF_UNIX)
        return EAFNOSUPPORT;

    sockaddr_un peer {};
    socklen_t peer_length = sizeof(peer);
    if (getpeername(socket_fd, reinterpret_cast<sockaddr*>(&peer), &peer_length) == 0)
        return EISCONN;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, request.path, request.path_length);

    // The helper may have asked for a blocking socket. Connecting is ours to do, so it happens
    // without blocking either way, and the flag it chose is put back before we answer.
    auto status_flags = fcntl(socket_fd, F_GETFL);
    if (status_flags < 0)
        return errno;
    if (fcntl(socket_fd, F_SETFL, status_flags | O_NONBLOCK) < 0)
        return errno;

    ScopeGuard restore_status_flags = [&] { (void)fcntl(socket_fd, F_SETFL, status_flags); };

    int result = 0;
    do {
        result = connect(socket_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    } while (result < 0 && errno == EINTR);

    // EAGAIN here means the backlog is full. There is nothing to wait for, so say so at once.
    if (result < 0 && errno != EINPROGRESS)
        return errno;

    if (result < 0) {
        switch (wait_until_ready(socket_fd, POLLOUT)) {
        case WaitResult::Ready:
            break;
        case WaitResult::TimedOut:
            return ETIMEDOUT;
        case WaitResult::ShuttingDown:
            return ECANCELED;
        case WaitResult::Failed:
            return EIO;
        }

        int connect_error = 0;
        socklen_t connect_error_length = sizeof(connect_error);
        if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &connect_error, &connect_error_length) < 0)
            return errno;
        if (connect_error != 0)
            return connect_error;
    }

    return 0;
}

void* ConnectBroker::run(void* self)
{
    static_cast<ConnectBroker*>(self)->serve();
    return nullptr;
}

bool ConnectBroker::is_allowed(StringView path)
{
    // Exact matches only. The Browser worked these paths out itself, so there is nothing here that
    // a helper could talk it into widening.
    auto matches = [&] {
        for (auto const& allowed_path : m_allowed_paths) {
            if (allowed_path.view() == path)
                return true;
        }
        return false;
    };

    if (matches())
        return true;

    // A helper can name an endpoint the Browser could not see when the list was built: an audio
    // server that was not running yet, or one further down a fallback list than the address the
    // audio library reported. Asking again is bounded, so a helper cannot spend our time by
    // guessing paths.
    while (m_refreshes_remaining > 0 && m_refresh_allowed_paths) {
        --m_refreshes_remaining;

        // Added to what we already allow rather than replacing it. A later answer that is shorter,
        // because a server went away between asking, must not take away an endpoint that works.
        for (auto& refreshed_path : m_refresh_allowed_paths()) {
            if (!m_allowed_paths.contains_slow(refreshed_path))
                m_allowed_paths.append(move(refreshed_path));
        }

        if (matches())
            return true;
    }

    return false;
}

void ConnectBroker::serve()
{
    for (;;) {
        ConnectBrokerRequest request {};
        int reply_fd = -1;
        int socket_fd = -1;

        auto received = receive_request(m_broker_fd, request, reply_fd, socket_fd);
        if (received == ReceiveResult::PeerClosed)
            break;
        if (received == ReceiveResult::Malformed)
            continue;

        ScopeGuard close_descriptors = [&] {
            close(reply_fd);
            if (socket_fd >= 0)
                close(socket_fd);
        };

        if (request.magic != connect_broker_magic) {
            send_response(reply_fd, EINVAL, -1);
            continue;
        }

        if (request.operation == static_cast<u32>(ConnectBrokerOperation::CreateSocket)) {
            auto created_fd = create_socket(request);
            if (created_fd < 0) {
                send_response(reply_fd, -created_fd, -1);
                continue;
            }
            send_response(reply_fd, 0, created_fd);
            close(created_fd);
            continue;
        }

        if (request.operation != static_cast<u32>(ConnectBrokerOperation::Connect)) {
            send_response(reply_fd, EINVAL, -1);
            continue;
        }

        if (request.path_length == 0 || request.path_length > sizeof(request.path)) {
            send_response(reply_fd, EINVAL, -1);
            continue;
        }

        auto path = StringView { request.path, request.path_length };
        if (path.contains('\0') || !is_allowed(path)) {
            send_response(reply_fd, EACCES, -1);
            continue;
        }

        auto error = connect_socket(socket_fd, request);
        send_response(reply_fd, error, -1);
        if (error == ECANCELED)
            break;
    }
}

}

#endif
