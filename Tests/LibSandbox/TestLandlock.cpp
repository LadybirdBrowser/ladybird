/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSandbox/Sandbox.h>
#include <LibSandbox/Seccomp.h>
#include <LibTest/TestCase.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

// Makes the kernel answer the Landlock version probe with the given errno (0 makes the probe return an ABI of 0), then
// asks for a sandbox in a child process. Whatever else the kernel can do, a helper must still get one, and it must keep
// the helper from its files at once.
static bool can_restrict_filesystem_when_version_probe_returns(u32 error)
{
    char file_template[] = "/tmp/lb-landlock-XXXXXX";
    auto file_fd = mkstemp(file_template);
    VERIFY(file_fd >= 0);
    close(file_fd);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_landlock_create_ruleset, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (error & SECCOMP_RET_DATA)),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program {
            .len = sizeof(filter) / sizeof(filter[0]),
            .filter = filter,
        };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) < 0)
            _exit(2);

        if (Sandbox::restrict_filesystem().is_error())
            _exit(1);
        auto fd = open(file_template, O_RDONLY | O_CLOEXEC);
        _exit(fd < 0 && errno == EACCES ? 0 : 3);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    (void)unlink(file_template);
    VERIFY(WIFEXITED(status));
    VERIFY(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1 || WEXITSTATUS(status) == 3);
    return WEXITSTATUS(status) == 0;
}

TEST_CASE(a_kernel_without_landlock_still_gets_a_sandbox)
{
    EXPECT(can_restrict_filesystem_when_version_probe_returns(ENOSYS));
}

TEST_CASE(a_kernel_with_landlock_disabled_still_gets_a_sandbox)
{
    EXPECT(can_restrict_filesystem_when_version_probe_returns(EOPNOTSUPP));
}

TEST_CASE(a_landlock_abi_of_zero_still_gets_a_sandbox)
{
    EXPECT(can_restrict_filesystem_when_version_probe_returns(0));
}

TEST_CASE(a_container_that_blocks_landlock_still_gets_a_sandbox)
{
    EXPECT(can_restrict_filesystem_when_version_probe_returns(EPERM));
}

TEST_CASE(a_process_with_a_second_thread_refuses_the_sandbox)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        int pipe_fds[2];
        if (pipe(pipe_fds) < 0)
            _exit(2);

        // The thread waits on the pipe, so it is still there when we ask for the sandbox.
        pthread_t thread;
        auto wait_for_pipe = [](void* argument) -> void* {
            char byte;
            auto nread = read(*static_cast<int*>(argument), &byte, 1);
            (void)nread;
            return nullptr;
        };
        if (pthread_create(&thread, nullptr, wait_for_pipe, &pipe_fds[0]) != 0)
            _exit(2);

        auto result = Sandbox::restrict_filesystem();
        _exit(result.is_error() ? 0 : 1);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    VERIFY(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}

#ifdef LANDLOCK_SCOPE_ABSTRACT_UNIX_SOCKET
TEST_CASE(abstract_unix_sockets_outside_the_landlock_domain_are_unreachable)
{
    auto abi = syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 6) {
        warnln("Skipping abstract UNIX socket scoping test: Landlock ABI 6 is required");
        return;
    }

    auto receiver = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    VERIFY(receiver >= 0);
    // Autobind chooses a unique abstract address without needing a file on disk.
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    VERIFY(bind(receiver, reinterpret_cast<sockaddr*>(&address), sizeof(sa_family_t)) == 0);
    socklen_t address_length = sizeof(address);
    VERIFY(getsockname(receiver, reinterpret_cast<sockaddr*>(&address), &address_length) == 0);

    auto sender = socket(AF_UNIX, SOCK_DGRAM, 0);
    VERIFY(sender >= 0);
    VERIFY(sendto(sender, "k", 1, 0, reinterpret_cast<sockaddr*>(&address), address_length) == 1);
    char byte = 0;
    VERIFY(recv(receiver, &byte, 1, 0) == 1);
    VERIFY(byte == 'k');

    int handed_over[2];
    VERIFY(socketpair(AF_UNIX, SOCK_STREAM, 0, handed_over) == 0);

    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        MUST(Sandbox::install_no_new_privileges());
        MUST(Sandbox::restrict_filesystem());

        // No seccomp policy is installed: these refusals must come from Landlock.
        VERIFY(connect(sender, reinterpret_cast<sockaddr*>(&address), address_length) == -1);
        VERIFY(errno == EPERM);
        VERIFY(sendto(sender, "k", 1, 0, reinterpret_cast<sockaddr*>(&address), address_length) == -1);
        VERIFY(errno == EPERM);

        char payload = 'k';
        iovec io { .iov_base = &payload, .iov_len = 1 };
        msghdr message {};
        message.msg_name = &address;
        message.msg_namelen = address_length;
        message.msg_iov = &io;
        message.msg_iovlen = 1;
        VERIFY(sendmsg(sender, &message, 0) == -1);
        VERIFY(errno == EPERM);

        // Existing IPC channels remain usable after entering the Landlock domain.
        VERIFY(send(handed_over[0], &payload, 1, 0) == 1);
        VERIFY(recv(handed_over[1], &payload, 1, 0) == 1);
        VERIFY(payload == 'k');
        _exit(0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
    EXPECT_EQ(recv(receiver, &byte, 1, 0), -1);
    EXPECT_EQ(errno, EAGAIN);
    VERIFY(close(handed_over[0]) == 0);
    VERIFY(close(handed_over[1]) == 0);
    VERIFY(close(sender) == 0);
    VERIFY(close(receiver) == 0);
}
#endif

#ifdef LANDLOCK_SCOPE_SIGNAL
TEST_CASE(signals_stay_within_the_landlock_process_tree)
{
    auto abi = syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 6) {
        warnln("Skipping signal scoping test: Landlock ABI 6 is required");
        return;
    }

    auto parent = getpid();
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        MUST(Sandbox::install_no_new_privileges());
        MUST(Sandbox::restrict_filesystem());
        VERIFY(Sandbox::signals_stay_within_the_process_tree());
        Sandbox::SeccompPolicy policy;
        policy.allow_common_runtime();
        policy.allow_process_creation();
        policy.allow_file_descriptor_operations();
        MUST(policy.install());

        VERIFY(syscall(__NR_tgkill, parent, parent, 0) == -1);
        VERIFY(errno == EPERM);
        auto grandchild = fork();
        VERIFY(grandchild >= 0);
        if (grandchild == 0) {
            VERIFY(syscall(__NR_tgkill, getpid(), syscall(__NR_gettid), 0) == 0);
            VERIFY(syscall(__NR_tgkill, parent, parent, 0) == -1);
            VERIFY(errno == EPERM);
            _exit(0);
        }
        int status = 0;
        VERIFY(waitpid(grandchild, &status, 0) == grandchild);
        VERIFY(WIFEXITED(status) && WEXITSTATUS(status) == 0);
        _exit(0);
    }
    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif

TEST_CASE(gpu_driver_paths_leave_out_shader_caches_that_cannot_be_used)
{
    char directory_template[] = "/tmp/lb-shader-cache-XXXXXX";
    VERIFY(mkdtemp(directory_template));
    auto directory = ByteString { directory_template };
    auto regular_file = ByteString::formatted("{}/file", directory);
    auto mesa_cache = ByteString::formatted("{}/mesa", directory);
    auto nvidia_cache = ByteString::formatted("{}/nvidia", directory);

    auto fd = open(regular_file.characters(), O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    VERIFY(fd >= 0);
    VERIFY(close(fd) == 0);
    VERIFY(setenv("__GL_SHADER_DISK_CACHE_PATH", nvidia_cache.characters(), 1) == 0);

    auto rules_for_mesa_cache = [](char const* mesa_cache_path) {
        VERIFY(setenv("MESA_SHADER_CACHE_DIR", mesa_cache_path, 1) == 0);
        Vector<Sandbox::PathRule> rules;
        EXPECT(!Sandbox::add_gpu_driver_paths(rules).is_error());
        return rules;
    };
    auto has_rule_for = [](Vector<Sandbox::PathRule> const& rules, StringView path) {
        return any_of(rules, [&](auto const& rule) { return rule.path == path; });
    };

    auto rules = rules_for_mesa_cache(regular_file.characters());
    EXPECT(!has_rule_for(rules, regular_file));

    rules = rules_for_mesa_cache("../cache");
    EXPECT(!has_rule_for(rules, "../cache"sv));

    rules = rules_for_mesa_cache(mesa_cache.characters());
    EXPECT(any_of(rules, [&](auto const& rule) {
        return rule.path == mesa_cache && rule.access == Sandbox::PathRule::Access::ReadWrite;
    }));

    VERIFY(unsetenv("MESA_SHADER_CACHE_DIR") == 0);
    VERIFY(unsetenv("__GL_SHADER_DISK_CACHE_PATH") == 0);
    (void)unlink(regular_file.characters());
    (void)rmdir(mesa_cache.characters());
    (void)rmdir(nvidia_cache.characters());
    (void)rmdir(directory.characters());
}
