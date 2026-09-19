/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/Environment.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibTest/TestCase.h>
#include <fcntl.h>
#if defined(AK_OS_LINUX)
#    include <linux/filter.h>
#    include <linux/seccomp.h>
#    include <stddef.h>
#    include <sys/prctl.h>
#    include <sys/resource.h>
#    include <sys/syscall.h>
#    include <sys/wait.h>
#endif
#include <unistd.h>

static ByteString run_and_read_output(Core::ProcessSpawnOptions options)
{
    auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
    options.file_actions.append(Core::FileAction::DupFd { .write_fd = pipe[1], .fd = STDOUT_FILENO });
    auto process = MUST(Core::Process::spawn(options));
    MUST(Core::System::close(pipe[1]));

    StringBuilder output;
    char buffer[4096];
    while (true) {
        auto bytes = MUST(Core::System::read(pipe[0], { buffer, sizeof(buffer) }));
        if (bytes == 0)
            break;
        output.append(StringView { buffer, bytes });
    }
    MUST(Core::System::close(pipe[0]));
    EXPECT_EQ(MUST(process.wait_for_termination()), 0);
    return output.to_byte_string();
}

static ByteString run_env(Optional<Vector<ByteString>> environment)
{
    return run_and_read_output({
        .executable = "/usr/bin/env"sv,
        .environment = move(environment),
    });
}

TEST_CASE(spawned_process_gets_the_given_environment)
{
    MUST(Core::Environment::set("TEST_LIBCORE_PROCESS_SECRET"sv, "secret"sv, Core::Environment::Overwrite::Yes));

    EXPECT_EQ(run_env(Vector<ByteString> { "ONLY=1"sv }), "ONLY=1\n"sv);
    EXPECT_EQ(run_env(Vector<ByteString> {}), ""sv);
    EXPECT(run_env({}).contains("TEST_LIBCORE_PROCESS_SECRET=secret"sv));
}

#if defined(AK_OS_MACOS) || defined(AK_OS_LINUX)
TEST_CASE(spawned_process_only_inherits_the_standard_streams_and_the_given_descriptors)
{
    // Neither descriptor is close-on-exec, as with a descriptor that another thread has just received or duplicated.
    auto unrelated = MUST(Core::System::pipe2(0));
    auto given = MUST(Core::System::pipe2(0));
    auto given_child_fd = MUST(Core::System::fcntl(given[0], F_DUPFD_CLOEXEC, STDERR_FILENO + 1));

    auto script = ByteString::formatted(
        "for fd in {} {} {}; do if {{ true >&$fd; }} 2>/dev/null; then echo open $fd; else echo closed $fd; fi; done",
        STDERR_FILENO, unrelated[0], given_child_fd);
    auto output = run_and_read_output({
        .executable = "/bin/sh"sv,
        .arguments = { "-c"sv, script },
        .file_actions = { Core::FileAction::DupFd { .write_fd = given[0], .fd = given_child_fd } },
    });

    EXPECT_EQ(output, ByteString::formatted("open {}\nclosed {}\nopen {}\n", STDERR_FILENO, unrelated[0], given_child_fd));

    for (auto fd : { unrelated[0], unrelated[1], given[0], given[1], given_child_fd })
        MUST(Core::System::close(fd));
}
#endif

TEST_CASE(duplicated_descriptor_is_close_on_exec)
{
    auto pipe = MUST(Core::System::pipe2(0));
    auto duplicate = MUST(Core::System::dup(pipe[0]));
    EXPECT(MUST(Core::System::fcntl(duplicate, F_GETFD)) & FD_CLOEXEC);

    for (auto fd : { pipe[0], pipe[1], duplicate })
        MUST(Core::System::close(fd));
}

#if defined(AK_OS_LINUX)
TEST_CASE(spawned_process_found_in_path_gets_the_given_environment)
{
    auto output = run_and_read_output({
        .executable = "env"sv,
        .search_for_executable_in_path = true,
        .environment = Vector<ByteString> { "ONLY=1"sv },
    });
    EXPECT_EQ(output, "ONLY=1\n"sv);
}

// A file action that targets a descriptor number that is free here must not take the pipe that reports a failed exec().
TEST_CASE(failed_exec_is_reported_when_a_file_action_targets_a_free_descriptor)
{
    // The next pipe gets these same numbers.
    auto probe = MUST(Core::System::pipe2(O_CLOEXEC));
    MUST(Core::System::close(probe[0]));
    MUST(Core::System::close(probe[1]));

    auto result = Core::Process::spawn({
        .executable = "/nonexistent/program"sv,
        .file_actions = { Core::FileAction::OpenFile { "/dev/null", Core::File::OpenMode::Read, probe[1] } },
    });
    EXPECT(result.is_error());

    auto result_after_close = Core::Process::spawn({
        .executable = "/nonexistent/program"sv,
        .file_actions = { Core::FileAction::CloseFile { probe[1] } },
    });
    EXPECT(result_after_close.is_error());
}

// A file action that duplicates a descriptor that is not open here must not hand the child the pipe instead. The child
// would keep it open, and the spawn would wait for the child to exit.
TEST_CASE(file_action_that_duplicates_a_free_descriptor_does_not_get_the_error_pipe)
{
    // The next pipe gets these same numbers.
    auto probe = MUST(Core::System::pipe2(O_CLOEXEC));
    MUST(Core::System::close(probe[0]));
    MUST(Core::System::close(probe[1]));

    auto process = Core::Process::spawn({
        .executable = "/bin/true"sv,
        .file_actions = { Core::FileAction::DupFd { .write_fd = probe[1], .fd = probe[1] + 100 } },
    });
    // dup2() of a descriptor that is not open fails, so the spawn does too.
    EXPECT(process.is_error());
}

// Moving the pipe out of the way must not need a descriptor above the ones that the file actions name.
TEST_CASE(file_action_at_the_descriptor_limit_does_not_stop_a_spawn)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        rlimit limit { 64, 64 };
        if (setrlimit(RLIMIT_NOFILE, &limit) < 0)
            _exit(2);
        auto process = Core::Process::spawn({
            .executable = "/bin/true"sv,
            .file_actions = { Core::FileAction::DupFd { .write_fd = STDERR_FILENO, .fd = 63 } },
        });
        if (process.is_error())
            _exit(3);
        _exit(MUST(process.value().wait_for_termination()));
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}

// Linux before 5.9 has no close_range(), and some container seccomp profiles refuse it.
TEST_CASE(spawned_process_gets_only_the_given_descriptors_without_close_range)
{
    auto child = fork();
    VERIFY(child >= 0);
    if (child == 0) {
        sock_filter filter[] = {
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_close_range, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program { .len = sizeof(filter) / sizeof(filter[0]), .filter = filter };
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) < 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) < 0)
            _exit(2);

        auto forgotten_fd = fcntl(STDERR_FILENO, F_DUPFD, 100);
        auto given_fd = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 101);
        auto script = ByteString::formatted("[ -e /proc/self/fd/{} ] && exit 1; [ -e /proc/self/fd/{} ] || exit 1; exit 0", forgotten_fd, given_fd);
        Vector<ByteString> arguments { "-c"sv, script };
        auto process = Core::Process::spawn({
            .executable = "/bin/sh"sv,
            .arguments = arguments,
            .file_actions = { Core::FileAction::DupFd { .write_fd = given_fd, .fd = given_fd } },
        });
        if (process.is_error())
            _exit(3);
        _exit(MUST(process.value().wait_for_termination()));
    }

    int status = 0;
    VERIFY(waitpid(child, &status, 0) == child);
    EXPECT(WIFEXITED(status));
    if (WIFEXITED(status))
        EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif
