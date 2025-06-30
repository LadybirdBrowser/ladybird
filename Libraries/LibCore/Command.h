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

class Command {
public:
    struct ProcessOutputs {
        ByteBuffer standard_output;
        ByteBuffer standard_error;
    };

    struct ProcessResult {
        int exit_code { 0 };
        ByteBuffer output;
        ByteBuffer error;
    };

    // Static convenience methods for synchronous execution (replaces standalone functions)
    static ErrorOr<ProcessResult> run(ByteString const& program, Vector<ByteString> const& arguments, Optional<LexicalPath> chdir = {});
    static ErrorOr<ProcessResult> run(ByteString const& command_string, Optional<LexicalPath> chdir = {});

    // Factory method for interactive command execution
    static ErrorOr<OwnPtr<Command>> create(StringView command, char const* const arguments[]);

    Command(pid_t pid, NonnullOwnPtr<Core::File> stdin_file, NonnullOwnPtr<Core::File> stdout_file, NonnullOwnPtr<Core::File> stderr_file);

    ErrorOr<void> write(StringView input);

    ErrorOr<void> write_lines(Span<ByteString> lines);

    ErrorOr<ProcessOutputs> read_all();

    // Run to completion and return results (for converting from interactive to synchronous usage)
    ErrorOr<ProcessResult> run_to_completion();

    enum class Status {
        Running,
        DoneWithZeroExitCode,
        Failed,
        FailedFromTimeout,
        Unknown,
    };

    ErrorOr<Status> status(int options = 0);

private:
    pid_t m_pid { -1 };
    NonnullOwnPtr<Core::File> m_stdin;
    NonnullOwnPtr<Core::File> m_stdout;
    NonnullOwnPtr<Core::File> m_stderr;
};

// Legacy compatibility functions (deprecated, use Command::run instead)
ErrorOr<Command::ProcessResult> command(ByteString const& program, Vector<ByteString> const& arguments, Optional<LexicalPath> chdir = {});
ErrorOr<Command::ProcessResult> command(ByteString const& command_string, Optional<LexicalPath> chdir = {});

}
