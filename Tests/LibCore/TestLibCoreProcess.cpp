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

#if defined(AK_OS_MACOS)
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
