/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Atomic.h>
#include <AK/OwnPtr.h>
#include <AK/Time.h>
#include <LibCore/DirIterator.h>
#include <LibSandbox/ConnectBroker.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibTest/TestCase.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/udp.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__NR_newfstatat) && defined(__NR_fstat)
TEST_CASE(fstatat_queries_descriptors_without_allowing_path_queries)
{
    int pipe_fds[2];
    VERIFY(pipe(pipe_fds) == 0);
    struct stat expected_metadata {};
    VERIFY(syscall(__NR_fstat, pipe_fds[0], &expected_metadata) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        MUST(Sandbox::install_no_new_privileges());
        Sandbox::SeccompPolicy policy;
        policy.deny_readonly_filesystem_probes();
        policy.allow_file_descriptor_operations();
        policy.allow_common_runtime();
        MUST(policy.install());

        struct stat metadata {};
        for (u64 i = 0; i < 2; ++i) {
            errno = EDOM;
            VERIFY(syscall(__NR_newfstatat, pipe_fds[0], "", &metadata, AT_EMPTY_PATH) == 0);
            VERIFY(errno == EDOM);
            VERIFY(metadata.st_ino == expected_metadata.st_ino);
            VERIFY(metadata.st_dev == expected_metadata.st_dev);
            VERIFY(metadata.st_mode == expected_metadata.st_mode);
        }

        VERIFY(syscall(__NR_newfstatat, pipe_fds[0], nullptr, &metadata, AT_EMPTY_PATH) == 0);
        VERIFY(metadata.st_ino == expected_metadata.st_ino);
        VERIFY(syscall(__NR_newfstatat, AT_FDCWD, "", &metadata, AT_EMPTY_PATH) == -1);
        VERIFY(errno == EBADF);
        VERIFY(syscall(__NR_newfstatat, -1, "", &metadata, AT_EMPTY_PATH) == -1);
        VERIFY(errno == EBADF);
        VERIFY(syscall(__NR_newfstatat, pipe_fds[0], "", nullptr, AT_EMPTY_PATH) == -1);
        VERIFY(errno == EFAULT);
        VERIFY(syscall(__NR_newfstatat, AT_FDCWD, "/", &metadata, AT_EMPTY_PATH) == -1);
        VERIFY(errno == EACCES);
        VERIFY(syscall(__NR_newfstatat, AT_FDCWD, ".", &metadata, AT_EMPTY_PATH) == -1);
        VERIFY(errno == EACCES);
        VERIFY(syscall(__NR_newfstatat, AT_FDCWD, "/", &metadata, 0) == -1);
        VERIFY(errno == EACCES);
        VERIFY(syscall(__NR_newfstatat, pipe_fds[0], "", &metadata, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW) == -1);
        VERIFY(errno == EACCES);
        VERIFY(close(pipe_fds[0]) == 0);
        VERIFY(close(pipe_fds[1]) == 0);
        _exit(0);
    }

    VERIFY(close(pipe_fds[0]) == 0);
    VERIFY(close(pipe_fds[1]) == 0);
    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

template<typename Configure, typename Body>
static int run_with_policy(Configure configure, Body body)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        MUST(Sandbox::install_no_new_privileges());
        Sandbox::SeccompPolicy policy;
        policy.allow_file_descriptor_operations();
        policy.allow_common_runtime();
        configure(policy);
        MUST(policy.install());

        body();
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    return status;
}

// A domain that no group asked for fails cleanly, so the child survives and sees the error.
template<typename Configure>
static void expect_socket_domain_is_refused(Configure configure, int domain)
{
    auto status = run_with_policy(configure, [domain] {
        VERIFY(socket(domain, SOCK_STREAM, 0) == -1);
        VERIFY(errno == EAFNOSUPPORT);
    });
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

// Whether a domain works at all is settled out here, outside the sandbox. A kernel built without
// IPv6 refuses AF_INET6 with the same EAFNOSUPPORT the policy uses, so without this the check would
// pass just as happily when the policy refused the domain it was supposed to permit.
template<typename Configure>
static void expect_socket_domain_is_allowed(Configure configure, int domain, int type, int protocol)
{
    auto supported = socket(domain, type, protocol);
    if (supported < 0)
        return;
    VERIFY(close(supported) == 0);

    auto status = run_with_policy(configure, [domain, type, protocol] {
        auto fd = socket(domain, type, protocol);
        VERIFY(fd >= 0);
        VERIFY(close(fd) == 0);
    });
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

#ifdef __NR_close_range
TEST_CASE(process_creation_policy_lets_a_child_close_undeclared_descriptors)
{
    // What Core::Process::spawn() does in the child, just before exec().
    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) { policy.allow_process_creation(); },
        [] {
            VERIFY(syscall(__NR_close_range, 1000, ~0U, 0) == 0);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

TEST_CASE(ipc_policy_serves_descriptors_the_browser_already_handed_over)
{
    // Made out here and inherited through fork(), which is how a helper comes by the channel the
    // Browser minted for it. Making one inside the sandbox would prove nothing about that, because
    // the group lets a process pair sockets with itself as well.
    int handed_over[2];
    VERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, handed_over) == 0);

    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); },
        [&handed_over] {
            char byte = 'k';
            VERIFY(send(handed_over[0], &byte, 1, 0) == 1);
            VERIFY(recv(handed_over[1], &byte, 1, 0) == 1);
            VERIFY(byte == 'k');
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    VERIFY(close(handed_over[0]) == 0);
    VERIFY(close(handed_over[1]) == 0);
}

TEST_CASE(ipc_policy_lets_a_process_pair_sockets_with_itself)
{
    // LibIPC makes a pair inside a helper whenever it hands one end to another process, so this has
    // to keep working even though creating any other kind of socket does not.
    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); },
        [] {
            for (auto type : { SOCK_STREAM, SOCK_SEQPACKET }) {
                for (auto flags : Array<int, 4> { 0, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_CLOEXEC | SOCK_NONBLOCK }) {
                    int fds[2];
                    VERIFY(socketpair(AF_UNIX, type | flags, 0, fds) == 0);

                    char byte = 'k';
                    VERIFY(send(fds[0], &byte, 1, 0) == 1);
                    VERIFY(recv(fds[1], &byte, 1, 0) == 1);
                    VERIFY(byte == 'k');

                    VERIFY(close(fds[0]) == 0);
                    VERIFY(close(fds[1]) == 0);
                }
            }
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(ipc_policy_refuses_datagram_socketpairs)
{
    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); },
        [] {
            for (auto type : Array<int, 3> { SOCK_DGRAM, SOCK_RAW, SOCK_STREAM | 0x100 }) {
                for (auto flags : Array<int, 4> { 0, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_CLOEXEC | SOCK_NONBLOCK }) {
                    int fds[2];
                    VERIFY(socketpair(AF_UNIX, type | flags, 0, fds) == -1);
                    VERIFY(errno == ESOCKTNOSUPPORT);
                }
            }
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(ipc_policy_refuses_addressed_datagrams)
{
    char directory_template[] = "/tmp/ladybird-datagram-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);
    auto path = ByteString::formatted("{}/socket", directory);

    for (bool abstract : { false, true }) {
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.characters(), path.length());
        if (abstract)
            address.sun_path[0] = '\0';

        auto receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
        VERIFY(receiver >= 0);
        VERIFY(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

        // Inherit a datagram socketpair to test sendto() independently of the creation restrictions.
        int sender[2];
        VERIFY(socketpair(AF_UNIX, SOCK_DGRAM, 0, sender) == 0);
        VERIFY(sendto(sender[0], "k", 1, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 1);
        char byte = 0;
        VERIFY(recv(receiver, &byte, 1, 0) == 1);
        VERIFY(byte == 'k');

        auto status = run_with_policy(
            [](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); },
            [&] {
                VERIFY(sendto(sender[0], "k", 1, 0, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == -1);
                VERIFY(errno == EPERM);

                // A pointer with a zero low word must not pass the null destination check.
                VERIFY(sendto(sender[0], "k", 1, 0, reinterpret_cast<sockaddr*>(1ULL << 32), sizeof(address)) == -1);
                VERIFY(errno == EPERM);
            });

        EXPECT(WIFEXITED(status));
        if (WIFEXITED(status))
            EXPECT_EQ(WEXITSTATUS(status), 0);

        // The child has exited, so no send is pending and this needs no timeout.
        EXPECT_EQ(recv(receiver, &byte, 1, 0), -1);
        EXPECT_EQ(errno, EAGAIN);
        VERIFY(close(sender[0]) == 0);
        VERIFY(close(sender[1]) == 0);
        VERIFY(close(receiver) == 0);
    }

    VERIFY(unlink(path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}

TEST_CASE(brokered_socket_creation_refuses_datagrams_before_contacting_the_broker)
{
    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) {
            Sandbox::set_connect_broker_fd(-1);
            policy.allow_ipc();
            policy.broker_unix_socket_connections();
        },
        [] {
            for (auto type : Array<int, 3> { SOCK_DGRAM, SOCK_RAW, SOCK_STREAM | 0x100 }) {
                for (auto flags : Array<int, 4> { 0, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_CLOEXEC | SOCK_NONBLOCK }) {
                    VERIFY(socket(AF_UNIX, type | flags, 0) == -1);
                    VERIFY(errno == ESOCKTNOSUPPORT);
                }
            }
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(ipc_policy_refuses_to_create_sockets)
{
    expect_socket_domain_is_refused([](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); }, AF_UNIX);
    expect_socket_domain_is_refused([](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); }, AF_INET);
}

TEST_CASE(ipc_policy_refuses_socket_options_that_reach_into_the_kernel)
{
    int handed_over[2];
    VERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, handed_over) == 0);

    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) { policy.allow_ipc(); },
        [&handed_over] {
            // LibIPC sizes its buffers, and that has to keep working.
            int buffer_size = 64 * KiB;
            VERIFY(setsockopt(handed_over[0], SOL_SOCKET, SO_SNDBUF, &buffer_size, sizeof(buffer_size)) == 0);
            VERIFY(setsockopt(handed_over[0], SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size)) == 0);
            int error = 0;
            socklen_t error_size = sizeof(error);
            VERIFY(getsockopt(handed_over[0], SOL_SOCKET, SO_ERROR, &error, &error_size) == 0);

            // A classic BPF program that accepts every packet. The kernel would take it, if we let it.
            sock_filter accept_all[] = { BPF_STMT(BPF_RET | BPF_K, 0xffffffff) };
            sock_fprog program { .len = 1, .filter = accept_all };
            for (int option : { SO_ATTACH_FILTER, SO_ATTACH_REUSEPORT_CBPF }) {
                VERIFY(setsockopt(handed_over[0], SOL_SOCKET, option, &program, sizeof(program)) == -1);
                VERIFY(errno == EPERM);
            }
            int bpf_fd = -1;
            for (int option : { SO_ATTACH_BPF, SO_ATTACH_REUSEPORT_EBPF }) {
                VERIFY(setsockopt(handed_over[0], SOL_SOCKET, option, &bpf_fd, sizeof(bpf_fd)) == -1);
                VERIFY(errno == EPERM);
            }
            int one = 1;
            for (int option : { SO_LOCK_FILTER, SO_DETACH_FILTER }) {
                VERIFY(setsockopt(handed_over[0], SOL_SOCKET, option, &one, sizeof(one)) == -1);
                VERIFY(errno == EPERM);
            }

            // Options that only the audio connection needs are not part of this group.
            VERIFY(setsockopt(handed_over[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) == -1);
            VERIFY(errno == EPERM);

            sock_filter filter[1] {};
            socklen_t filter_size = sizeof(filter);
            VERIFY(getsockopt(handed_over[0], SOL_SOCKET, SO_GET_FILTER, filter, &filter_size) == -1);
            VERIFY(errno == EPERM);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    VERIFY(close(handed_over[0]) == 0);
    VERIFY(close(handed_over[1]) == 0);
}

TEST_CASE(brokered_connections_accept_the_options_that_libpulse_sets)
{
    int handed_over[2];
    VERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, handed_over) == 0);

    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) {
            policy.allow_ipc();
            policy.broker_unix_socket_connections();
        },
        [&handed_over] {
            int one = 1;
            VERIFY(setsockopt(handed_over[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof(one)) == 0);
            int priority = 6;
            VERIFY(setsockopt(handed_over[0], SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) == 0);

            sock_filter accept_all[] = { BPF_STMT(BPF_RET | BPF_K, 0xffffffff) };
            sock_fprog program { .len = 1, .filter = accept_all };
            VERIFY(setsockopt(handed_over[0], SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) == -1);
            VERIFY(errno == EPERM);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    VERIFY(close(handed_over[0]) == 0);
    VERIFY(close(handed_over[1]) == 0);
}

TEST_CASE(brokered_unix_socket_connections_refuse_every_other_domain)
{
    auto configure = [](Sandbox::SeccompPolicy& policy) {
        policy.allow_ipc();
        policy.broker_unix_socket_connections();
    };

    // The caller gets no socket of its own, so without a broker to make one there is nothing to
    // hand back. That is an error rather than a death: a renderer started without a broker should
    // lose audio, not fall over.
    auto status = run_with_policy(configure, [] {
        VERIFY(socket(AF_UNIX, SOCK_STREAM, 0) == -1);
        VERIFY(errno == EACCES);
    });
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    // Any other domain is refused before a broker would come into it.
    expect_socket_domain_is_refused(configure, AF_INET);
}

TEST_CASE(network_policy_allows_only_the_internet_socket_options_we_use)
{
    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) {
            policy.allow_ipc();
            policy.allow_network();
        },
        [] {
            int one = 1;
            auto tcp_socket = socket(AF_INET, SOCK_STREAM, 0);
            VERIFY(tcp_socket >= 0);
            VERIFY(setsockopt(tcp_socket, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one)) == 0);
            // Attaching an upper layer protocol loads kernel code, such as kTLS.
            VERIFY(setsockopt(tcp_socket, IPPROTO_TCP, TCP_ULP, "tls", 3) == -1);
            VERIFY(errno == EPERM);

            auto udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
            VERIFY(udp_socket >= 0);
            VERIFY(setsockopt(udp_socket, IPPROTO_IP, IP_RECVERR, &one, sizeof(one)) == 0);
            // What libcurl sets on a QUIC socket for HTTP/3.
            int path_mtu_discovery = IP_PMTUDISC_DO;
            VERIFY(setsockopt(udp_socket, IPPROTO_IP, IP_MTU_DISCOVER, &path_mtu_discovery, sizeof(path_mtu_discovery)) == 0);
            VERIFY(setsockopt(udp_socket, IPPROTO_UDP, UDP_GRO, &one, sizeof(one)) == 0);
            sock_filter accept_all[] = { BPF_STMT(BPF_RET | BPF_K, 0xffffffff) };
            sock_fprog program { .len = 1, .filter = accept_all };
            VERIFY(setsockopt(udp_socket, SOL_SOCKET, SO_ATTACH_FILTER, &program, sizeof(program)) == -1);
            VERIFY(errno == EPERM);

            VERIFY(close(tcp_socket) == 0);
            VERIFY(close(udp_socket) == 0);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(network_policy_allows_addressed_datagrams)
{
    auto receiver = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    VERIFY(receiver >= 0);
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    VERIFY(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    socklen_t address_length = sizeof(address);
    VERIFY(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);

    auto status = run_with_policy(
        [](Sandbox::SeccompPolicy& policy) {
            policy.allow_ipc();
            policy.allow_network();
        },
        [&] {
            auto sender = socket(AF_INET, SOCK_DGRAM, 0);
            VERIFY(sender >= 0);
            VERIFY(sendto(sender, "k", 1, 0, reinterpret_cast<sockaddr*>(&address), address_length) == 1);
            VERIFY(close(sender) == 0);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
    char byte = 0;
    EXPECT_EQ(recv(receiver, &byte, 1, 0), 1);
    EXPECT_EQ(byte, 'k');
    VERIFY(close(receiver) == 0);
}

TEST_CASE(network_policy_is_limited_to_internet_sockets)
{
    auto configure = [](Sandbox::SeccompPolicy& policy) {
        policy.allow_ipc();
        policy.allow_network();
    };

    expect_socket_domain_is_allowed(configure, AF_INET, SOCK_STREAM, 0);
    expect_socket_domain_is_allowed(configure, AF_INET6, SOCK_STREAM, 0);
    expect_socket_domain_is_allowed(configure, AF_NETLINK, SOCK_RAW, 0);
    expect_socket_domain_is_refused(configure, AF_UNIX);
}

// Waits for a descriptor to become readable. The deadline is far longer than any of this work takes,
// so a slow machine cannot trip it; it is here so that a broker which never answers is reported as a
// failure instead of leaving the test to hang.
static bool wait_until_readable(int fd)
{
    static constexpr int deadline_in_milliseconds = 30'000;

    for (;;) {
        pollfd descriptor { .fd = fd, .events = POLLIN, .revents = 0 };
        auto ready = poll(&descriptor, 1, deadline_in_milliseconds);
        if (ready < 0 && errno == EINTR)
            continue;
        return ready > 0;
    }
}

static ErrorOr<int> listen_on_unix_socket(ByteString const& path)
{
    auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return Error::from_syscall("socket"sv, errno);

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    VERIFY(path.length() < sizeof(address.sun_path));
    memcpy(address.sun_path, path.characters(), path.length());

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
        return Error::from_syscall("bind"sv, errno);
    if (listen(fd, 4) < 0)
        return Error::from_syscall("listen"sv, errno);
    return fd;
}

static int connect_to_unix_socket(ByteString const& path)
{
    auto fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.characters(), path.length());

    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

TEST_CASE(the_broker_connects_only_to_paths_on_its_allowlist)
{
    char directory_template[] = "/tmp/ladybird-broker-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto allowed_path = ByteString::formatted("{}/allowed", directory);
    auto denied_path = ByteString::formatted("{}/denied", directory);

    auto allowed_listener = MUST(listen_on_unix_socket(allowed_path));
    auto denied_listener = MUST(listen_on_unix_socket(denied_path));

    auto broker = MUST(Sandbox::ConnectBroker::create({ allowed_path }));

    auto status = run_with_policy(
        [&](Sandbox::SeccompPolicy& policy) {
            Sandbox::set_connect_broker_fd(broker->helper_fd());
            policy.allow_ipc();
            policy.broker_unix_socket_connections();
        },
        [&] {
            auto allowed_fd = connect_to_unix_socket(allowed_path);
            VERIFY(allowed_fd >= 0);
            VERIFY(send(allowed_fd, "k", 1, 0) == 1);
            VERIFY(close(allowed_fd) == 0);

            // The broker refuses the path, so this fails rather than reaching the listener.
            VERIFY(connect_to_unix_socket(denied_path) == -1);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    VERIFY(wait_until_readable(allowed_listener));
    auto accepted = accept4(allowed_listener, nullptr, nullptr, SOCK_CLOEXEC);
    EXPECT(accepted >= 0);
    if (accepted >= 0) {
        char byte = 0;
        EXPECT_EQ(recv(accepted, &byte, 1, 0), 1);
        EXPECT_EQ(byte, 'k');
        VERIFY(close(accepted) == 0);
    }

    VERIFY(close(allowed_listener) == 0);
    VERIFY(close(denied_listener) == 0);
    VERIFY(unlink(allowed_path.characters()) == 0);
    VERIFY(unlink(denied_path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}

// Speaks the broker protocol directly, so the test controls exactly what the broker is asked and
// when. Returns the reply socket to read the answer from.
// A socket of the kind the broker hands out: AF_UNIX, not connected to anything yet.
static int make_unconnected_socket()
{
    auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    VERIFY(fd >= 0);
    return fd;
}

// Speaks the broker protocol directly, so the test controls exactly what is asked and when. A
// connect carries the socket to connect as well as the channel to answer on.
static int submit_broker_request(int helper_fd, ByteString const& path, int socket_fd, int reply_socket_buffer_size = 0)
{
    Sandbox::Detail::ConnectBrokerRequest request {};
    request.magic = Sandbox::Detail::connect_broker_magic;
    request.operation = static_cast<u32>(Sandbox::Detail::ConnectBrokerOperation::Connect);
    request.socket_domain = AF_UNIX;
    request.socket_type = SOCK_STREAM;
    request.path_length = path.length();
    memcpy(request.path, path.characters(), path.length());

    int reply_fds[2];
    VERIFY(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply_fds) == 0);

    if (reply_socket_buffer_size > 0) {
        // Shrink the buffer so it can be filled, which is what a helper would do to try to make the
        // broker block while answering it.
        VERIFY(setsockopt(reply_fds[1], SOL_SOCKET, SO_SNDBUF, &reply_socket_buffer_size, sizeof(reply_socket_buffer_size)) == 0);
        VERIFY(setsockopt(reply_fds[0], SOL_SOCKET, SO_RCVBUF, &reply_socket_buffer_size, sizeof(reply_socket_buffer_size)) == 0);

        char filler[512] = {};
        while (send(reply_fds[1], filler, sizeof(filler), MSG_DONTWAIT) > 0)
            ;
    }

    int passed_fds[2] = { reply_fds[1], socket_fd };

    iovec io { .iov_base = &request, .iov_len = sizeof(request) };
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

    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(header), passed_fds, sizeof(passed_fds));

    VERIFY(sendmsg(helper_fd, &message, MSG_NOSIGNAL) == sizeof(request));
    VERIFY(close(reply_fds[1]) == 0);
    return reply_fds[0];
}

static i32 await_broker_error(int reply_fd)
{
    VERIFY(wait_until_readable(reply_fd));

    Sandbox::Detail::ConnectBrokerResponse response {};
    iovec io { .iov_base = &response, .iov_len = sizeof(response) };

    union {
        cmsghdr header;
        char space[CMSG_SPACE(sizeof(int))];
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
        received = recvmsg(reply_fd, &message, MSG_CMSG_CLOEXEC);
    } while (received < 0 && errno == EINTR);

    VERIFY(received == sizeof(response));
    VERIFY(response.magic == Sandbox::Detail::connect_broker_magic);

    // A successful answer carries the connected socket. Close it here rather than leaving it to the
    // kernel, so that a test counting descriptors is counting something it fully accounts for.
    for (auto* header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
        if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS)
            continue;
        auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int fd = -1;
            memcpy(&fd, CMSG_DATA(header) + i * sizeof(int), sizeof(fd));
            VERIFY(close(fd) == 0);
        }
    }

    return response.error;
}

TEST_CASE(the_broker_refuses_datagrams_even_without_the_seccomp_policy)
{
    auto broker = MUST(Sandbox::ConnectBroker::create({}));
    for (auto type : Array<int, 5> { SOCK_DGRAM, SOCK_RAW, SOCK_STREAM | 0x100, SOCK_STREAM, SOCK_SEQPACKET }) {
        for (auto flags : Array<int, 4> { 0, SOCK_CLOEXEC, SOCK_NONBLOCK, SOCK_CLOEXEC | SOCK_NONBLOCK }) {
            Sandbox::Detail::ConnectBrokerRequest request {};
            request.magic = Sandbox::Detail::connect_broker_magic;
            request.operation = static_cast<u32>(Sandbox::Detail::ConnectBrokerOperation::CreateSocket);
            request.socket_domain = AF_UNIX;
            request.socket_type = type | flags;

            int reply_fds[2];
            VERIFY(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply_fds) == 0);
            iovec io { .iov_base = &request, .iov_len = sizeof(request) };
            union {
                cmsghdr header;
                char space[CMSG_SPACE(sizeof(int))];
            } control {};
            msghdr message {};
            message.msg_iov = &io;
            message.msg_iovlen = 1;
            message.msg_control = &control;
            message.msg_controllen = sizeof(control);
            auto* header = CMSG_FIRSTHDR(&message);
            header->cmsg_level = SOL_SOCKET;
            header->cmsg_type = SCM_RIGHTS;
            header->cmsg_len = CMSG_LEN(sizeof(int));
            memcpy(CMSG_DATA(header), &reply_fds[1], sizeof(int));

            VERIFY(sendmsg(broker->helper_fd(), &message, MSG_NOSIGNAL) == sizeof(request));
            VERIFY(close(reply_fds[1]) == 0);
            EXPECT_EQ(await_broker_error(reply_fds[0]), type == SOCK_STREAM || type == SOCK_SEQPACKET ? 0 : ESOCKTNOSUPPORT);
            VERIFY(close(reply_fds[0]) == 0);
        }
    }
}

// The broker closes the connected socket and then the reply channel, both after answering. Waiting
// for the channel to end is therefore the point at which it has finished with the request, and it
// is an event rather than an interval, so nothing here depends on how fast the machine is.
static void await_broker_finishing_with_the_request(int reply_fd)
{
    VERIFY(wait_until_readable(reply_fd));

    char byte = 0;
    ssize_t received = 0;
    do {
        received = recv(reply_fd, &byte, sizeof(byte), 0);
    } while (received < 0 && errno == EINTR);

    VERIFY(received == 0);
}

static Vector<int> fill_listener_backlog(ByteString const& path)
{
    Vector<int> fillers;
    for (int i = 0; i < 64; ++i) {
        auto fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        VERIFY(fd >= 0);

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.characters(), path.length());

        if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
            VERIFY(close(fd) == 0);
            return fillers;
        }
        fillers.append(fd);
    }
    VERIFY_NOT_REACHED();
}

TEST_CASE(a_wedged_endpoint_is_refused_rather_than_waited_on)
{
    char directory_template[] = "/tmp/ladybird-broker-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto wedged_path = ByteString::formatted("{}/wedged", directory);
    auto listener = MUST(listen_on_unix_socket(wedged_path));

    // Nothing ever accepts here. Once the backlog is full, connecting blocks rather than failing,
    // which is how a UNIX socket differs from a TCP one, and is what used to pin the broker.
    auto fillers = fill_listener_backlog(wedged_path);
    EXPECT(!fillers.is_empty());

    OwnPtr<Sandbox::ConnectBroker> broker = MUST(Sandbox::ConnectBroker::create({ wedged_path }));

    auto first_socket = make_unconnected_socket();
    auto reply_fd = submit_broker_request(broker->helper_fd(), wedged_path, first_socket);
    EXPECT_EQ(await_broker_error(reply_fd), EAGAIN);
    VERIFY(close(reply_fd) == 0);
    VERIFY(close(first_socket) == 0);

    // The broker is still serving, so it did not lose its thread to that request.
    auto second_socket = make_unconnected_socket();
    auto second_reply_fd = submit_broker_request(broker->helper_fd(), wedged_path, second_socket);
    EXPECT_EQ(await_broker_error(second_reply_fd), EAGAIN);
    VERIFY(close(second_reply_fd) == 0);
    VERIFY(close(second_socket) == 0);

    broker = nullptr;

    for (auto fd : fillers)
        VERIFY(close(fd) == 0);
    VERIFY(close(listener) == 0);
    VERIFY(unlink(wedged_path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}

TEST_CASE(a_reply_socket_that_cannot_be_written_to_does_not_stall_the_broker)
{
    char directory_template[] = "/tmp/ladybird-broker-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto allowed_path = ByteString::formatted("{}/allowed", directory);
    auto listener = MUST(listen_on_unix_socket(allowed_path));

    OwnPtr<Sandbox::ConnectBroker> broker = MUST(Sandbox::ConnectBroker::create({ allowed_path }));

    // A helper is free to hand over a reply socket it has already filled. The broker must give up
    // on answering it rather than hand over its thread.
    auto stalled_socket = make_unconnected_socket();
    auto stalled_reply_fd = submit_broker_request(broker->helper_fd(), allowed_path, stalled_socket, 1024);

    // If the broker were stuck on the request above, this answer would never arrive.
    auto socket_fd = make_unconnected_socket();
    auto reply_fd = submit_broker_request(broker->helper_fd(), allowed_path, socket_fd);
    EXPECT_EQ(await_broker_error(reply_fd), 0);

    VERIFY(close(reply_fd) == 0);
    VERIFY(close(socket_fd) == 0);
    VERIFY(close(stalled_reply_fd) == 0);
    VERIFY(close(stalled_socket) == 0);
    broker = nullptr;

    VERIFY(close(listener) == 0);
    VERIFY(unlink(allowed_path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}

static size_t count_open_descriptors()
{
    // A directory that cannot be opened iterates as an empty one, so without these the count would
    // be zero both times and the comparison that uses it would hold without having looked at
    // anything.
    auto iterator = Core::DirIterator { "/proc/self/fd", Core::DirIterator::SkipDots };
    VERIFY(!iterator.has_error());

    size_t count = 0;
    while (iterator.has_next()) {
        (void)iterator.next_path();
        ++count;
    }

    // The standard descriptors are always there, and so is the one this iteration is using.
    VERIFY(count > 0);
    return count;
}

// A request carrying more descriptors than its operation asks for. Asking for a socket needs only
// the channel to answer on, and the control buffer has room for a second descriptor, so a helper
// can always attach one that is not wanted.
static void submit_request_with_an_unwanted_descriptor(int helper_fd)
{
    Sandbox::Detail::ConnectBrokerRequest request {};
    request.magic = Sandbox::Detail::connect_broker_magic;
    request.operation = static_cast<u32>(Sandbox::Detail::ConnectBrokerOperation::CreateSocket);
    request.socket_domain = AF_UNIX;
    request.socket_type = SOCK_STREAM;

    int reply_fds[2];
    VERIFY(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, reply_fds) == 0);

    int passed_fds[2] = { reply_fds[1], make_unconnected_socket() };

    iovec io { .iov_base = &request, .iov_len = sizeof(request) };
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

    auto* header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(2 * sizeof(int));
    memcpy(CMSG_DATA(header), passed_fds, sizeof(passed_fds));

    VERIFY(sendmsg(helper_fd, &message, MSG_NOSIGNAL) == sizeof(request));

    // Our own copies go away, so anything still open afterwards is the broker holding them.
    VERIFY(close(reply_fds[0]) == 0);
    VERIFY(close(reply_fds[1]) == 0);
    VERIFY(close(passed_fds[1]) == 0);
}

TEST_CASE(a_malformed_request_leaves_no_descriptors_behind_and_keeps_the_broker_serving)
{
    char directory_template[] = "/tmp/ladybird-broker-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto allowed_path = ByteString::formatted("{}/allowed", directory);
    auto listener = MUST(listen_on_unix_socket(allowed_path));

    OwnPtr<Sandbox::ConnectBroker> broker = MUST(Sandbox::ConnectBroker::create({ allowed_path }));

    auto descriptors_before = count_open_descriptors();

    submit_request_with_an_unwanted_descriptor(broker->helper_fd());

    // Answering this proves the broker dealt with the request above and is still serving, which is
    // what makes the count below meaningful without waiting on the clock.
    auto socket_fd = make_unconnected_socket();
    auto reply_fd = submit_broker_request(broker->helper_fd(), allowed_path, socket_fd);
    EXPECT_EQ(await_broker_error(reply_fd), 0);
    await_broker_finishing_with_the_request(reply_fd);
    VERIFY(close(reply_fd) == 0);
    VERIFY(close(socket_fd) == 0);

    EXPECT_EQ(count_open_descriptors(), descriptors_before);

    broker = nullptr;
    VERIFY(close(listener) == 0);
    VERIFY(unlink(allowed_path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}

TEST_CASE(a_brokered_connection_behaves_like_a_real_one)
{
    char directory_template[] = "/tmp/ladybird-broker-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto allowed_path = ByteString::formatted("{}/allowed", directory);
    auto listener = MUST(listen_on_unix_socket(allowed_path));

    // Mapped here, so the child does not need to be allowed to map anything itself. The second page
    // is unreadable, which is what lets an address be placed so that it runs off the end of the
    // first one.
    auto page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    auto* pages = mmap(nullptr, page_size * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    VERIFY(pages != MAP_FAILED);
    VERIFY(mprotect(static_cast<char*>(pages) + page_size, page_size, PROT_NONE) == 0);

    OwnPtr<Sandbox::ConnectBroker> broker = MUST(Sandbox::ConnectBroker::create({ allowed_path }));

    auto status = run_with_policy(
        [&](Sandbox::SeccompPolicy& policy) {
            Sandbox::set_connect_broker_fd(broker->helper_fd());
            policy.allow_ipc();
            policy.broker_unix_socket_connections();
        },
        [&] {
            auto fd = socket(AF_UNIX, SOCK_STREAM, 0);
            VERIFY(fd >= 0);

            // Asked for a blocking socket, so it must be one, both before and after connecting.
            // Otherwise a later read or write fails with EAGAIN where the caller expected to wait.
            VERIFY((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);
            VERIFY((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);

            // What libpulse does: it sets this on the socket and then connects it.
            int priority = 6;
            VERIFY(setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority)) == 0);

            struct stat before {};
            VERIFY(fstat(fd, &before) == 0);

            sockaddr_un address {};
            address.sun_family = AF_UNIX;
            memcpy(address.sun_path, allowed_path.characters(), allowed_path.length());
            VERIFY(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

            // The option the caller set before connecting still applies afterwards.
            int connected_priority = 0;
            socklen_t length = sizeof(connected_priority);
            VERIFY(getsockopt(fd, SOL_SOCKET, SO_PRIORITY, &connected_priority, &length) == 0);
            VERIFY(connected_priority == priority);

            VERIFY((fcntl(fd, F_GETFL) & O_NONBLOCK) == 0);
            VERIFY((fcntl(fd, F_GETFD) & FD_CLOEXEC) == 0);

            // And a socket that did ask for those keeps them.
            auto nonblocking_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            VERIFY(nonblocking_fd >= 0);
            VERIFY((fcntl(nonblocking_fd, F_GETFL) & O_NONBLOCK) != 0);
            VERIFY((fcntl(nonblocking_fd, F_GETFD) & FD_CLOEXEC) != 0);
            VERIFY(close(nonblocking_fd) == 0);

            // And it is still the same open file, not a replacement wearing the same number.
            struct stat after {};
            VERIFY(fstat(fd, &after) == 0);
            VERIFY(after.st_ino == before.st_ino);
            VERIFY(after.st_dev == before.st_dev);
            VERIFY(close(fd) == 0);

            // An address the caller cannot read is an error, not a crash. The real syscall reports
            // EFAULT for both of these, and so must anything standing in for it.
            auto bad_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            VERIFY(bad_fd >= 0);

            errno = 0;
            VERIFY(connect(bad_fd, reinterpret_cast<sockaddr*>(1), sizeof(sockaddr_un)) == -1);
            VERIFY(errno == EFAULT);

            // Starts on a readable page and runs off the end of it.
            auto* straddling = static_cast<char*>(pages) + page_size - 4;
            errno = 0;
            VERIFY(connect(bad_fd, reinterpret_cast<sockaddr*>(straddling), sizeof(sockaddr_un)) == -1);
            VERIFY(errno == EFAULT);

            VERIFY(close(bad_fd) == 0);
        });

    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    broker = nullptr;
    VERIFY(munmap(pages, page_size * 2) == 0);
    VERIFY(close(listener) == 0);
    VERIFY(unlink(allowed_path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}

TEST_CASE(the_broker_asks_again_for_a_path_it_did_not_know_about)
{
    char directory_template[] = "/tmp/ladybird-broker-XXXXXX";
    auto* directory = mkdtemp(directory_template);
    VERIFY(directory);

    auto known_path = ByteString::formatted("{}/known", directory);
    auto later_path = ByteString::formatted("{}/later", directory);
    auto known_listener = MUST(listen_on_unix_socket(known_path));
    auto later_listener = MUST(listen_on_unix_socket(later_path));

    // Stands for an endpoint the Browser could not name when the renderer started, such as a server
    // that was not running yet or one further down a fallback list.
    // Written by the broker's own thread and read here. Passing descriptors back and forth does not
    // order those two against each other, so the counter has to do it itself.
    Atomic<size_t> refresh_count { 0 };
    OwnPtr<Sandbox::ConnectBroker> broker = MUST(Sandbox::ConnectBroker::create({ known_path }, [&] {
        ++refresh_count;
        return Vector<ByteString> { later_path };
    }));

    auto connect_to = [&](ByteString const& path) {
        auto socket_fd = make_unconnected_socket();
        auto reply_fd = submit_broker_request(broker->helper_fd(), path, socket_fd);
        auto error = await_broker_error(reply_fd);
        VERIFY(close(reply_fd) == 0);
        VERIFY(close(socket_fd) == 0);
        return error;
    };

    EXPECT_EQ(connect_to(later_path), 0);
    EXPECT(refresh_count.load() > 0);

    // What was already allowed stays allowed, so a later answer cannot take an endpoint away.
    EXPECT_EQ(connect_to(known_path), 0);

    // A helper cannot spend the Browser's time guessing: asking again runs out.
    auto refreshes_after_success = refresh_count.load();
    for (int i = 0; i < 8; ++i)
        EXPECT_EQ(connect_to(ByteString::formatted("{}/never-{}", directory, i)), EACCES);
    EXPECT(refresh_count.load() - refreshes_after_success <= 4u);

    // And the broker is still serving afterwards.
    EXPECT_EQ(connect_to(known_path), 0);

    broker = nullptr;
    VERIFY(close(known_listener) == 0);
    VERIFY(close(later_listener) == 0);
    VERIFY(unlink(known_path.characters()) == 0);
    VERIFY(unlink(later_path.characters()) == 0);
    VERIFY(rmdir(directory) == 0);
}
