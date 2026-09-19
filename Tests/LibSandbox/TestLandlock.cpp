/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibSandbox/Sandbox.h>
#include <LibTest/TestCase.h>
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

enum class Outcome {
    Refused,
    Confined,
};

// Makes the kernel answer the Landlock version probe with the given errno (0 makes the probe return an ABI of 0), then
// asks for a Landlock sandbox in a child process.
static Outcome restrict_filesystem_when_version_probe_returns(u32 error)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        // CI allows a missing Landlock for the helpers it starts. This test is about the default.
        unsetenv("LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK");

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

        auto result = Sandbox::restrict_filesystem_with_landlock();
        _exit(result.is_error() ? 0 : 1);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    VERIFY(WIFEXITED(status));
    VERIFY(WEXITSTATUS(status) == 0 || WEXITSTATUS(status) == 1);
    return WEXITSTATUS(status) == 0 ? Outcome::Refused : Outcome::Confined;
}

TEST_CASE(a_kernel_without_landlock_refuses_the_sandbox)
{
    EXPECT_EQ(restrict_filesystem_when_version_probe_returns(ENOSYS), Outcome::Refused);
}

TEST_CASE(a_kernel_with_landlock_disabled_refuses_the_sandbox)
{
    EXPECT_EQ(restrict_filesystem_when_version_probe_returns(EOPNOTSUPP), Outcome::Refused);
}

TEST_CASE(a_landlock_abi_of_zero_refuses_the_sandbox)
{
    EXPECT_EQ(restrict_filesystem_when_version_probe_returns(0), Outcome::Refused);
}

TEST_CASE(a_container_that_blocks_landlock_refuses_the_sandbox)
{
    EXPECT_EQ(restrict_filesystem_when_version_probe_returns(EPERM), Outcome::Refused);
}

TEST_CASE(a_missing_landlock_can_be_allowed_explicitly)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        setenv("LADYBIRD_UNSAFE_ALLOW_MISSING_LANDLOCK", "1", 1);
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
            _exit(2);

        auto result = Sandbox::restrict_filesystem_with_landlock();
        _exit(result.is_error() ? 1 : 0);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    VERIFY(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
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

        auto result = Sandbox::restrict_filesystem_with_landlock();
        _exit(result.is_error() ? 0 : 1);
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    VERIFY(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
}
