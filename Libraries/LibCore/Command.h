/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibCore/File.h>
#include <LibCore/Forward.h>
#include <spawn.h>

namespace Core {

// FIXME: Unify this and the below 'command' functions with Command class below
struct CommandResult {
    int exit_code { 0 };
    ByteBuffer output;
    ByteBuffer error;
};

ErrorOr<CommandResult> command(ByteString const& program, Vector<ByteString> const& arguments, Optional<LexicalPath> chdir);
ErrorOr<CommandResult> command(ByteString const& command_string, Optional<LexicalPath> chdir);

class Command {
public:
    struct ProcessOutputs {
        ByteBuffer standard_output;
        ByteBuffer standard_error;
    };

    static ErrorOr<OwnPtr<Command>> create(StringView command, char const* const arguments[]);

    Command(pid_t pid, NonnullOwnPtr<Core::File> stdin_file, NonnullOwnPtr<Core::File> stdout_file, NonnullOwnPtr<Core::File> stderr_file);

    ErrorOr<void> write(StringView input);

    ErrorOr<void> write_lines(Span<ByteString> lines);

    ErrorOr<ProcessOutputs> read_all();

    enum class ProcessResult {
        Running,
        DoneWithZeroExitCode,
        Failed,
        FailedFromTimeout,
        Unknown,
    };

    ErrorOr<ProcessResult> status(int options = 0);

private:
    pid_t m_pid { -1 };
    NonnullOwnPtr<Core::File> m_stdin;
    NonnullOwnPtr<Core::File> m_stdout;
    NonnullOwnPtr<Core::File> m_stderr;
};

}
