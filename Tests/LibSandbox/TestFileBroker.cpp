/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Platform.h>
#include <AK/StringBuilder.h>
#include <LibSandbox/FileBroker.h>
#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibTest/TestCase.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/netlink.h>
#include <linux/seccomp.h>
#include <netinet/in.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/prctl.h>
#include <sys/ptrace.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <unistd.h>

using Sandbox::file_broker_allows;
using Sandbox::FileBrokerAccess;
using Sandbox::PathRule;
using Sandbox::ResolvedPathRule;

static ResolvedPathRule rule(StringView path, PathRule::Access access, bool is_directory)
{
    return { path, path, access, is_directory };
}

TEST_CASE(a_directory_rule_covers_what_is_inside_it)
{
    Vector<ResolvedPathRule> rules { rule("/a/b"sv, PathRule::Access::ReadOnly, true) };
    EXPECT(file_broker_allows(rules, "/a/b"sv, FileBrokerAccess::Read));
    EXPECT(file_broker_allows(rules, "/a/b/c/d"sv, FileBrokerAccess::Read));
    EXPECT(!file_broker_allows(rules, "/a/bc"sv, FileBrokerAccess::Read));
    EXPECT(!file_broker_allows(rules, "/a/b/c"sv, FileBrokerAccess::Write));
}

TEST_CASE(a_file_rule_covers_only_that_file)
{
    Vector<ResolvedPathRule> rules { rule("/a/file"sv, PathRule::Access::ReadWrite, false) };
    EXPECT(file_broker_allows(rules, "/a/file"sv, FileBrokerAccess::Write));
    EXPECT(!file_broker_allows(rules, "/a/file/x"sv, FileBrokerAccess::Read));
    EXPECT(!file_broker_allows(rules, "/a/other"sv, FileBrokerAccess::Read));
}

TEST_CASE(directories_above_a_rule_expose_their_metadata_and_nothing_else)
{
    Vector<ResolvedPathRule> rules { rule("/a/b"sv, PathRule::Access::ReadOnly, true) };
    EXPECT(file_broker_allows(rules, "/"sv, FileBrokerAccess::Metadata));
    EXPECT(file_broker_allows(rules, "/a"sv, FileBrokerAccess::Metadata));
    EXPECT(!file_broker_allows(rules, "/a"sv, FileBrokerAccess::Read));
    EXPECT(!file_broker_allows(rules, "/a/other"sv, FileBrokerAccess::Metadata));
}

TEST_CASE(rules_add_up_along_a_path)
{
    Vector<ResolvedPathRule> rules { rule("/a"sv, PathRule::Access::ReadWrite, true), rule("/a/b"sv, PathRule::Access::ReadOnly, true) };
    EXPECT(file_broker_allows(rules, "/a/b/c"sv, FileBrokerAccess::Write));
}

TEST_CASE(a_link_named_by_a_rule_exposes_its_metadata)
{
    Vector<ResolvedPathRule> rules { { "/a/link"sv, "/b/target"sv, PathRule::Access::ReadOnly, false } };
    EXPECT(file_broker_allows(rules, "/a/link"sv, FileBrokerAccess::Metadata));
    EXPECT(file_broker_allows(rules, "/a"sv, FileBrokerAccess::Metadata));
    EXPECT(!file_broker_allows(rules, "/a/link"sv, FileBrokerAccess::Read));
    EXPECT(file_broker_allows(rules, "/b/target"sv, FileBrokerAccess::Read));
}

TEST_CASE(only_a_writable_directory_lets_its_entries_change)
{
    Vector<ResolvedPathRule> rules { rule("/a/file"sv, PathRule::Access::ReadWrite, false), rule("/b"sv, PathRule::Access::ReadWrite, true), rule("/c"sv, PathRule::Access::ReadOnly, true) };
    EXPECT(!file_broker_allows(rules, "/a/file"sv, FileBrokerAccess::ChangeEntry));
    EXPECT(!file_broker_allows(rules, "/b"sv, FileBrokerAccess::ChangeEntry));
    EXPECT(file_broker_allows(rules, "/b/entry"sv, FileBrokerAccess::ChangeEntry));
    EXPECT(!file_broker_allows(rules, "/c/entry"sv, FileBrokerAccess::ChangeEntry));
}

// Hides Landlock, which is what a kernel without it looks like from here.
static void leave_only_the_broker_available()
{
    sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_landlock_create_ruleset, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    sock_fprog program {
        .len = sizeof(filter) / sizeof(filter[0]),
        .filter = filter,
    };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) < 0)
        _exit(100);
}

static void make_file(ByteString const& path)
{
    auto fd = open(path.characters(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    VERIFY(fd >= 0);
    VERIFY(write(fd, "k", 1) == 1);
    close(fd);
}

// Each check has its own exit code, so that a failure says which one it was.
static void check(int code, bool condition)
{
    if (!condition)
        _exit(code);
}

static int error_opening(ByteString const& path, int flags)
{
    auto fd = open(path.characters(), flags | O_CLOEXEC, 0600);
    if (fd < 0)
        return errno;
    close(fd);
    return 0;
}

static void confine(ReadonlySpan<PathRule> rules)
{
    leave_only_the_broker_available();
    check(50, !Sandbox::restrict_filesystem(rules).is_error());
    check(51, Sandbox::file_broker_is_active());

    Sandbox::SeccompPolicy policy;
    policy.allow_readonly_file_opens();
    policy.allow_filesystem_metadata_queries();
    policy.allow_filesystem_writes();
    policy.allow_file_descriptor_operations();
    policy.allow_ipc();
    policy.allow_socket_pairs();
    policy.allow_common_runtime();
    check(52, !policy.install().is_error());
}

TEST_CASE(the_broker_enforces_the_path_rules)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto read_only = ByteString::formatted("{}/read-only", base);
    auto writable = ByteString::formatted("{}/writable", base);
    auto hidden = ByteString::formatted("{}/hidden", base);
    auto read_only_file = ByteString::formatted("{}/file", read_only);
    auto hidden_file = ByteString::formatted("{}/file", hidden);

    VERIFY(mkdir(read_only.characters(), 0700) == 0);
    VERIFY(mkdir(writable.characters(), 0700) == 0);
    VERIFY(mkdir(hidden.characters(), 0700) == 0);
    make_file(read_only_file);
    make_file(hidden_file);
    auto subdirectory = ByteString::formatted("{}/subdirectory", read_only);
    VERIFY(mkdir(subdirectory.characters(), 0700) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, read_only, PathRule::Access::ReadOnly));
        MUST(Sandbox::add_path_if_exists(rules, writable, PathRule::Access::ReadWrite));
        MUST(Sandbox::add_path_if_exists(rules, "/proc/self"sv, PathRule::Access::ReadOnly));
        confine(rules);

        // Reading.
        auto fd = open(read_only_file.characters(), O_RDONLY | O_CLOEXEC);
        check(1, fd >= 0);
        char byte = 0;
        check(2, read(fd, &byte, 1) == 1 && byte == 'k');
        close(fd);

        // What the rules leave out.
        check(3, error_opening(hidden_file, O_RDONLY) == EACCES);
        check(4, error_opening("/etc/passwd"sv, O_RDONLY) == EACCES);
        check(5, error_opening(ByteString::formatted("{}/does-not-exist", hidden), O_RDONLY) == EACCES);

        // Writing.
        check(6, error_opening(read_only_file, O_WRONLY) == EACCES);
        check(7, error_opening(ByteString::formatted("{}/new", read_only), O_CREAT | O_WRONLY) == EACCES);
        auto created = ByteString::formatted("{}/created", writable);
        check(8, error_opening(created, O_CREAT | O_WRONLY) == 0);
        check(9, error_opening(created, O_CREAT | O_EXCL | O_WRONLY) == EEXIST);

        // Metadata.
        struct stat statbuf;
        check(10, stat(created.characters(), &statbuf) == 0 && S_ISREG(statbuf.st_mode));
        check(11, stat(hidden_file.characters(), &statbuf) < 0 && errno == EACCES);
        check(12, stat(base.characters(), &statbuf) == 0 && S_ISDIR(statbuf.st_mode));
        check(13, access(read_only_file.characters(), R_OK) == 0);
        check(14, access(read_only_file.characters(), W_OK) < 0 && errno == EACCES);

        // Making a link is refused rather than fatal, so a library that tries one can carry on.
        check(15, symlink(hidden_file.characters(), ByteString::formatted("{}/link", writable).characters()) < 0 && errno == EPERM);
        check(16, syscall(__NR_symlinkat, hidden_file.characters(), AT_FDCWD, ByteString::formatted("{}/link", writable).characters()) < 0 && errno == EPERM);

        // Changing the contents of a writable directory.
        auto directory = ByteString::formatted("{}/directory", writable);
        check(20, mkdir(directory.characters(), 0700) == 0);
        check(21, mkdir(ByteString::formatted("{}/nope", read_only).characters(), 0700) < 0 && errno == EACCES);
        check(28, mkdir(read_only.characters(), 0700) < 0 && errno == EEXIST);
        auto renamed = ByteString::formatted("{}/renamed", writable);
        check(22, rename(created.characters(), renamed.characters()) == 0);
        check(23, rename(renamed.characters(), ByteString::formatted("{}/stolen", hidden).characters()) < 0 && errno == EACCES);
        check(24, rename(read_only_file.characters(), ByteString::formatted("{}/stolen", writable).characters()) < 0 && errno == EACCES);
        check(25, unlink(renamed.characters()) == 0);
        check(26, unlink(read_only_file.characters()) < 0 && errno == EACCES);
        check(27, rmdir(directory.characters()) == 0);

        // Relative to a directory the process has open.
        auto directory_fd = open(read_only.characters(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        check(30, directory_fd >= 0);
        fd = openat(directory_fd, "file", O_RDONLY | O_CLOEXEC);
        check(31, fd >= 0);
        close(fd);
        check(32, openat(directory_fd, "../hidden/file", O_RDONLY | O_CLOEXEC) < 0 && errno == EACCES);

        // Going back up reports the directory that the path ends in.
        struct stat through_parent;
        check(36, stat(read_only.characters(), &statbuf) == 0);
        check(37, stat(ByteString::formatted("{}/subdirectory/..", read_only).characters(), &through_parent) == 0);
        check(38, statbuf.st_ino == through_parent.st_ino);

        // /proc/self has to mean this process, not the broker.
        fd = open("/proc/self/stat", O_RDONLY | O_CLOEXEC);
        check(33, fd >= 0);
        char status[32] {};
        check(34, read(fd, status, sizeof(status) - 1) > 0);
        close(fd);
        check(35, atoi(status) == getpid());
        char own_pid[32] {};
        check(39, readlink("/proc/self", own_pid, sizeof(own_pid) - 1) > 0 && atoi(own_pid) == getpid());
        memset(own_pid, 0, sizeof(own_pid));
        check(40, readlink("/proc/./self", own_pid, sizeof(own_pid) - 1) > 0 && atoi(own_pid) == getpid());

        // A trailing slash only ever names a directory.
        check(41, stat(ByteString::formatted("{}/", read_only_file).characters(), &statbuf) < 0 && errno == ENOTDIR);
        check(42, stat(ByteString::formatted("{}/", read_only).characters(), &statbuf) == 0);

        // A descriptor can be queried with no path at all.
        check(43, syscall(__NR_newfstatat, directory_fd, nullptr, &statbuf, AT_EMPTY_PATH) == 0 && S_ISDIR(statbuf.st_mode));

        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    EXPECT_EQ(error_opening(read_only_file, O_RDONLY), 0);
    EXPECT_EQ(error_opening(hidden_file, O_RDONLY), 0);

    (void)unlink(hidden_file.characters());
    (void)unlink(read_only_file.characters());
    (void)rmdir(subdirectory.characters());
    (void)rmdir(hidden.characters());
    (void)rmdir(writable.characters());
    (void)rmdir(read_only.characters());
    (void)rmdir(base.characters());
}

// Runs the body in a child with LADYBIRD_SANDBOX_DEBUG set, and returns what the child and its broker wrote to stderr.
template<typename Body>
static ByteString stderr_of_debugged_child(Body body)
{
    int fds[2];
    VERIFY(pipe(fds) == 0);
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        close(fds[0]);
        check(60, dup2(fds[1], STDERR_FILENO) == STDERR_FILENO);
        close(fds[1]);
        check(61, setenv("LADYBIRD_SANDBOX_DEBUG", "1", 1) == 0);
        body();
        _exit(0);
    }

    close(fds[1]);
    StringBuilder output;
    char buffer[4096];
    ssize_t received = 0;
    while ((received = read(fds[0], buffer, sizeof(buffer))) > 0)
        output.append(StringView { buffer, static_cast<size_t>(received) });
    close(fds[0]);

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
    return output.to_byte_string();
}

TEST_CASE(sandbox_debugging_logs_each_refused_path_once)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto read_only = ByteString::formatted("{}/read-only", base);
    auto hidden = ByteString::formatted("{}/hidden", base);
    auto hidden_file = ByteString::formatted("{}/file", hidden);
    VERIFY(mkdir(read_only.characters(), 0700) == 0);
    VERIFY(mkdir(hidden.characters(), 0700) == 0);
    make_file(hidden_file);

    auto output = stderr_of_debugged_child([&] {
        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, read_only, PathRule::Access::ReadOnly));
        MUST(Sandbox::add_path_if_exists(rules, "/proc/self"sv, PathRule::Access::ReadOnly));
        confine(rules);

        check(1, error_opening(hidden_file, O_RDONLY) == EACCES);
        check(2, error_opening(hidden_file, O_RDONLY) == EACCES);
        // A directory that exists is reported as such, which is no refusal to log.
        check(3, mkdir(read_only.characters(), 0700) < 0 && errno == EEXIST);
    });

    auto refusal = ByteString::formatted("read access to {}\n", hidden_file);
    EXPECT_EQ(output.count(refusal), 1u);
    EXPECT(!output.contains("entry change"sv));

    (void)unlink(hidden_file.characters());
    (void)rmdir(hidden.characters());
    (void)rmdir(read_only.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(sandbox_debugging_logs_what_the_file_system_filter_refuses)
{
    auto output = stderr_of_debugged_child([&] {
        confine({});

        // The policy allows links, but the file system filter refuses them.
        check(1, link("/", "/") < 0 && errno == EPERM);
    });

    EXPECT(output.contains("Sandbox refusal in "sv));
    EXPECT(output.contains(": errno 1 for syscall link"sv));
}

TEST_CASE(the_broker_does_not_follow_a_link_out_of_a_writable_directory)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto writable = ByteString::formatted("{}/writable", base);
    auto hidden = ByteString::formatted("{}/hidden", base);
    auto hidden_file = ByteString::formatted("{}/file", hidden);
    auto link_to_file = ByteString::formatted("{}/link-to-file", writable);
    auto link_to_directory = ByteString::formatted("{}/link-to-directory", writable);

    VERIFY(mkdir(writable.characters(), 0700) == 0);
    VERIFY(mkdir(hidden.characters(), 0700) == 0);
    make_file(hidden_file);
    // The sandbox can't make these itself. A helper that found one, or was handed a directory with one, is the concern.
    VERIFY(symlink(hidden_file.characters(), link_to_file.characters()) == 0);
    VERIFY(symlink(hidden.characters(), link_to_directory.characters()) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, writable, PathRule::Access::ReadWrite));
        confine(rules);

        check(1, error_opening(link_to_file, O_RDONLY) == EACCES);
        check(2, error_opening(link_to_file, O_WRONLY) == EACCES);
        check(3, error_opening(ByteString::formatted("{}/file", link_to_directory), O_RDONLY) == EACCES);
        check(4, error_opening(ByteString::formatted("{}/new", link_to_directory), O_CREAT | O_WRONLY) == EACCES);
        check(5, error_opening(link_to_file, O_RDONLY | O_NOFOLLOW) == ELOOP);

        // A link's own text is part of the directory the helper may read.
        char target[PATH_MAX];
        auto length = readlink(link_to_file.characters(), target, sizeof(target));
        check(6, length == static_cast<ssize_t>(hidden_file.length()));

        // Not following the link means that where it leads doesn't matter.
        check(8, syscall(__NR_faccessat2, AT_FDCWD, link_to_file.characters(), F_OK, AT_SYMLINK_NOFOLLOW) == 0);
        check(9, syscall(__NR_faccessat2, AT_FDCWD, link_to_file.characters(), F_OK, 0) < 0 && errno == EACCES);

        // Going up from an allowed directory is not a way out.
        check(7, error_opening(ByteString::formatted("{}/../hidden/file", writable), O_RDONLY) == EACCES);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    (void)unlink(link_to_directory.characters());
    (void)unlink(link_to_file.characters());
    (void)unlink(hidden_file.characters());
    (void)rmdir(hidden.characters());
    (void)rmdir(writable.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(a_socket_cannot_create_a_file_outside_the_path_rules)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto hidden = ByteString::formatted("{}/hidden", base);
    auto writable = ByteString::formatted("{}/writable", base);
    auto planted = ByteString::formatted("{}/planted", hidden);
    VERIFY(mkdir(hidden.characters(), 0700) == 0);
    VERIFY(mkdir(writable.characters(), 0700) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, writable, PathRule::Access::ReadWrite));
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem(rules).is_error());

        Sandbox::SeccompPolicy policy;
        policy.allow_file_descriptor_operations();
        policy.allow_ipc();
        policy.allow_socket_pairs();
        policy.allow_common_runtime();
        policy.allow_network();
        check(52, !policy.install().is_error());

        int pair[2];
        check(1, socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) == 0);

        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, planted.characters(), planted.length());
        check(2, bind(pair[0], reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 && errno == EACCES);

        // Internet sockets still bind, because the network policies are there for them.
        auto udp = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        check(3, udp >= 0);
        sockaddr_in loopback {};
        loopback.sin_family = AF_INET;
        loopback.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(4, bind(udp, reinterpret_cast<sockaddr*>(&loopback), sizeof(loopback)) == 0);
        socklen_t length = sizeof(loopback);
        check(5, getsockname(udp, reinterpret_cast<sockaddr*>(&loopback), &length) == 0 && loopback.sin_port != 0);

        // glibc binds one of these to ask the kernel about the network interfaces.
        auto netlink = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
        check(6, netlink >= 0);
        sockaddr_nl netlink_address {};
        netlink_address.nl_family = AF_NETLINK;
        check(7, bind(netlink, reinterpret_cast<sockaddr*>(&netlink_address), sizeof(netlink_address)) == 0);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    struct stat statbuf;
    EXPECT(lstat(planted.characters(), &statbuf) < 0);

    (void)unlink(planted.characters());
    (void)rmdir(hidden.characters());
    (void)rmdir(writable.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(a_helper_cannot_reach_a_unix_socket_outside_the_path_rules)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto stream_path = ByteString::formatted("{}/stream", base);
    auto datagram_path = ByteString::formatted("{}/datagram", base);

    auto address_of = [](ByteString const& path) {
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        memcpy(address.sun_path, path.characters(), path.length());
        return address;
    };

    auto stream_address = address_of(stream_path);
    auto datagram_address = address_of(datagram_path);
    auto listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    auto receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    VERIFY(listener >= 0 && receiver >= 0);
    VERIFY(bind(listener, reinterpret_cast<sockaddr*>(&stream_address), sizeof(stream_address)) == 0);
    VERIFY(listen(listener, 4) == 0);
    VERIFY(bind(receiver, reinterpret_cast<sockaddr*>(&datagram_address), sizeof(datagram_address)) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        Vector<PathRule> rules;
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem(rules).is_error());

        Sandbox::SeccompPolicy policy;
        policy.allow_file_descriptor_operations();
        policy.allow_ipc();
        policy.allow_socket_pairs();
        policy.allow_common_runtime();
        policy.allow_network();
        check(52, !policy.install().is_error());

        int unusable[2];
        check(1, socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, unusable) < 0);
        check(5, socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0) < 0);
        check(6, socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0) < 0);

        for (int type : { SOCK_STREAM, SOCK_SEQPACKET }) {
            int pair[2];
            check(2, socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, pair) == 0);

            auto cannot_leave_its_pair = [&](int code) {
                check(code, connect(pair[0], reinterpret_cast<sockaddr*>(&stream_address), sizeof(stream_address)) < 0);
                check(code + 1, connect(pair[0], reinterpret_cast<sockaddr*>(&datagram_address), sizeof(datagram_address)) < 0);
                (void)sendto(pair[0], "x", 1, MSG_NOSIGNAL | MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&datagram_address), sizeof(datagram_address));
            };

            cannot_leave_its_pair(10);
            check(3, shutdown(pair[0], SHUT_RDWR) == 0);
            cannot_leave_its_pair(20);
            check(4, close(pair[1]) == 0);
            cannot_leave_its_pair(30);
        }
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    char byte = 0;
    EXPECT(accept(listener, nullptr, nullptr) < 0 && errno == EAGAIN);
    EXPECT(recv(receiver, &byte, 1, 0) < 0 && errno == EAGAIN);

    close(listener);
    close(receiver);
    (void)unlink(stream_path.characters());
    (void)unlink(datagram_path.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(files_are_refused_as_soon_as_the_broker_runs)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto allowed = ByteString::formatted("{}/allowed", base);
    auto hidden = ByteString::formatted("{}/hidden", base);
    auto allowed_file = ByteString::formatted("{}/file", allowed);
    auto hidden_file = ByteString::formatted("{}/file", hidden);
    auto linked_file = ByteString::formatted("{}/linked", allowed);
    VERIFY(mkdir(allowed.characters(), 0700) == 0);
    VERIFY(mkdir(hidden.characters(), 0700) == 0);
    make_file(allowed_file);
    make_file(hidden_file);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, allowed, PathRule::Access::ReadOnly));
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem(rules).is_error());

        // No policy yet, as while a helper sets up its GPU driver.
        check(1, error_opening(hidden_file, O_RDONLY) == EACCES);
        check(2, error_opening(allowed_file, O_RDONLY) == 0);
        struct stat statbuf;
        check(3, stat(hidden_file.characters(), &statbuf) < 0 && errno == EACCES);
        check(4, truncate(hidden_file.characters(), 0) < 0 && errno == EACCES);
        check(5, chmod(hidden_file.characters(), 0777) < 0 && errno == EPERM);
        char value[8];
        check(6, lgetxattr(hidden_file.characters(), "user.test", value, sizeof(value)) < 0 && errno == EOPNOTSUPP);
        check(7, link(hidden_file.characters(), linked_file.characters()) < 0 && errno == EPERM);
        auto inotify = inotify_init1(IN_CLOEXEC);
        check(8, inotify >= 0 && inotify_add_watch(inotify, hidden_file.characters(), IN_ALL_EVENTS) < 0 && errno == EACCES);
        char* arguments[] = { const_cast<char*>("true"), nullptr };
        check(9, execve("/bin/true", arguments, nullptr) < 0 && errno == EACCES);
        check(10, ptrace(PTRACE_ATTACH, getppid(), nullptr, nullptr) < 0 && errno == EPERM);

        // open_tree(), openat2() and io_uring_setup(), by the numbers that every architecture shares.
        check(11, syscall(428, AT_FDCWD, hidden_file.characters(), 0) < 0 && errno == EPERM);
        check(12, syscall(437, AT_FDCWD, hidden_file.characters(), nullptr, 0) < 0 && errno == ENOSYS);
        check(13, syscall(425, 1, nullptr) < 0 && errno == ENOSYS);
        check(14, syscall(472) < 0 && errno == ENOSYS);
#if ARCH(X86_64)
        check(15, syscall(0x40000000 | __NR_openat, AT_FDCWD, hidden_file.characters(), O_RDONLY) < 0 && errno == ENOSYS);
#endif
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    struct stat statbuf;
    EXPECT(stat(hidden_file.characters(), &statbuf) == 0 && statbuf.st_size == 1 && (statbuf.st_mode & 0777) == 0600);
    EXPECT(lstat(linked_file.characters(), &statbuf) < 0);

    (void)unlink(allowed_file.characters());
    (void)unlink(hidden_file.characters());
    (void)rmdir(allowed.characters());
    (void)rmdir(hidden.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(a_thread_that_blocks_every_signal_still_reaches_the_broker)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto file = ByteString::formatted("{}/file", base);
    make_file(file);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        rlimit limit { 0, 0 };
        setrlimit(RLIMIT_CORE, &limit);

        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, base, PathRule::Access::ReadOnly));
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem(rules).is_error());

        Sandbox::SeccompPolicy policy;
        policy.allow_readonly_file_opens();
        policy.allow_file_descriptor_operations();
        policy.allow_threads();
        policy.allow_common_runtime();
        check(52, !policy.install().is_error());

        // As glibc does while a thread exits, which can still open a file.
        sigset_t all_signals;
        sigfillset(&all_signals);
        check(1, pthread_sigmask(SIG_BLOCK, &all_signals, nullptr) == 0);
        check(2, error_opening(file, O_RDONLY) == 0);

        sigset_t blocked;
        check(3, pthread_sigmask(SIG_SETMASK, nullptr, &blocked) == 0);
        check(4, sigismember(&blocked, SIGUSR1) == 1 && sigismember(&blocked, SIGSYS) == 0);

        sigset_t user_signal;
        sigemptyset(&user_signal);
        sigaddset(&user_signal, SIGUSR1);
        check(5, pthread_sigmask(SIG_UNBLOCK, &user_signal, nullptr) == 0);
        check(6, pthread_sigmask(SIG_SETMASK, nullptr, &blocked) == 0);
        check(7, sigismember(&blocked, SIGUSR1) == 0 && sigismember(&blocked, SIGUSR2) == 1);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    (void)unlink(file.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(a_policy_that_refuses_a_path_still_wins_over_the_broker)
{
    char base_template[] = "/tmp/lb-file-broker-XXXXXX";
    VERIFY(mkdtemp(base_template));
    ByteString base { base_template };
    auto file = ByteString::formatted("{}/file", base);
    make_file(file);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        Vector<PathRule> rules;
        MUST(Sandbox::add_path_if_exists(rules, base, PathRule::Access::ReadOnly));
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem(rules).is_error());
        check(1, error_opening(file, O_RDONLY) == 0);

        Sandbox::SeccompPolicy policy;
        policy.deny_readonly_filesystem_probes();
        policy.allow_file_descriptor_operations();
        policy.allow_common_runtime();
        check(52, !policy.install().is_error());
        check(2, error_opening(file, O_RDONLY) == EACCES);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);

    (void)unlink(file.characters());
    (void)rmdir(base.characters());
}

TEST_CASE(process_creation_without_landlock_cannot_signal_other_processes)
{
    auto parent = getpid();
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem().is_error());
        check(51, !Sandbox::signals_stay_within_the_process_tree());

        Sandbox::SeccompPolicy policy;
        policy.allow_common_runtime();
        policy.allow_process_creation();
        policy.allow_file_descriptor_operations();
        check(52, !policy.install().is_error());

        check(1, syscall(__NR_tgkill, getpid(), syscall(__NR_gettid), 0) == 0);
        check(2, syscall(__NR_tgkill, parent, parent, 0) < 0 && errno == EPERM);

        auto grandchild = fork();
        check(3, grandchild >= 0);
        if (grandchild == 0) {
            check(4, syscall(__NR_tgkill, parent, parent, 0) < 0 && errno == EPERM);
            _exit(0);
        }
        int status = 0;
        check(6, waitpid(grandchild, &status, 0) == grandchild);
        check(WIFEXITED(status) ? WEXITSTATUS(status) : 7, WIFEXITED(status) && WEXITSTATUS(status) == 0);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST_CASE(a_policy_that_sends_nothing_to_the_broker_still_cannot_pair_sockets)
{
    // ImageDecoder's sandbox: no paths at all, and no sockets of its own.
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        rlimit limit { 0, 0 };
        setrlimit(RLIMIT_CORE, &limit);
        leave_only_the_broker_available();
        check(50, !Sandbox::restrict_filesystem().is_error());

        Sandbox::SeccompPolicy policy;
        policy.deny_readonly_filesystem_probes();
        policy.allow_file_descriptor_operations();
        policy.allow_ipc();
        policy.allow_common_runtime();
        check(52, !policy.install().is_error());

        check(1, error_opening("/", O_RDONLY) == EACCES);
        int fds[2];
        (void)socketpair(AF_UNIX, SOCK_STREAM, 0, fds);
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFSIGNALED(status));
    if (WIFSIGNALED(status))
        EXPECT_EQ(WTERMSIG(status), SIGSYS);
}

TEST_CASE(the_broker_goes_away_with_the_helper)
{
    int pipe_fds[2];
    VERIFY(pipe(pipe_fds) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        close(pipe_fds[0]);
        leave_only_the_broker_available();
        check(1, !Sandbox::restrict_filesystem().is_error());
        // Our write end stays open in the broker only if it failed to close what it inherited.
        _exit(0);
    }

    close(pipe_fds[1]);
    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status) && WEXITSTATUS(status) == 0);

    char byte;
    EXPECT_EQ(read(pipe_fds[0], &byte, 1), 0);
    close(pipe_fds[0]);
}
