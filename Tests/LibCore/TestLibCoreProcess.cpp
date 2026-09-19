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

static ByteString run_env(Optional<Vector<ByteString>> environment)
{
    auto pipe = MUST(Core::System::pipe2(O_CLOEXEC));
    Vector<ByteString> arguments;
    auto process = MUST(Core::Process::spawn({
        .executable = "/usr/bin/env"sv,
        .arguments = arguments,
        .environment = move(environment),
        .file_actions = { Core::FileAction::DupFd { .write_fd = pipe[1], .fd = STDOUT_FILENO } },
    }));
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

TEST_CASE(spawned_process_gets_the_given_environment)
{
    MUST(Core::Environment::set("TEST_LIBCORE_PROCESS_SECRET"sv, "secret"sv, Core::Environment::Overwrite::Yes));

    EXPECT_EQ(run_env(Vector<ByteString> { "ONLY=1"sv }), "ONLY=1\n"sv);
    EXPECT_EQ(run_env(Vector<ByteString> {}), ""sv);
    EXPECT(run_env({}).contains("TEST_LIBCORE_PROCESS_SECRET=secret"sv));
}
