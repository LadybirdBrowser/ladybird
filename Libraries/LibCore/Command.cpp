/*
 * Copyright (c) 2020, Itamar S. <itamar8910@gmail.com>
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 * Copyright (c) 2023, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Command.h"
#include <AK/Format.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

namespace Core {

ErrorOr<OwnPtr<Command>> Command::create(StringView command, char const* const arguments[])
{
    auto stdin_fds = TRY(Core::System::pipe2(O_CLOEXEC));
    auto stdout_fds = TRY(Core::System::pipe2(O_CLOEXEC));
    auto stderr_fds = TRY(Core::System::pipe2(O_CLOEXEC));

    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);
    posix_spawn_file_actions_adddup2(&file_actions, stdin_fds[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&file_actions, stdout_fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&file_actions, stderr_fds[1], STDERR_FILENO);

    auto pid = TRY(Core::System::posix_spawnp(command, &file_actions, nullptr, const_cast<char**>(arguments), Core::Environment::raw_environ()));

    posix_spawn_file_actions_destroy(&file_actions);
    ArmedScopeGuard runner_kill { [&pid] { kill(pid, SIGKILL); } };

    TRY(Core::System::close(stdin_fds[0]));
    TRY(Core::System::close(stdout_fds[1]));
    TRY(Core::System::close(stderr_fds[1]));

    auto stdin_file = TRY(Core::File::adopt_fd(stdin_fds[1], Core::File::OpenMode::Write));
    auto stdout_file = TRY(Core::File::adopt_fd(stdout_fds[0], Core::File::OpenMode::Read));
    auto stderr_file = TRY(Core::File::adopt_fd(stderr_fds[0], Core::File::OpenMode::Read));

    runner_kill.disarm();

    return make<Command>(pid, move(stdin_file), move(stdout_file), move(stderr_file));
}

Command::Command(pid_t pid, NonnullOwnPtr<Core::File> stdin_file, NonnullOwnPtr<Core::File> stdout_file, NonnullOwnPtr<Core::File> stderr_file)
    : m_pid(pid)
    , m_stdin(move(stdin_file))
    , m_stdout(move(stdout_file))
    , m_stderr(move(stderr_file))
{
}

ErrorOr<void> Command::write(StringView input)
{
    TRY(m_stdin->write_until_depleted(input.bytes()));
    m_stdin->close();
    return {};
}

ErrorOr<void> Command::write_lines(Span<ByteString> lines)
{
    // It's possible the process dies before we can write everything to the
    // stdin. So make sure that we don't crash but just stop writing.

    struct sigaction action_handler {};
    action_handler.sa_handler = SIG_IGN;

    struct sigaction old_action_handler;
    TRY(Core::System::sigaction(SIGPIPE, &action_handler, &old_action_handler));

    auto close_stdin = ScopeGuard([this, &old_action_handler] {
        // Ensure that the input stream ends here, whether we were able to write all lines or not
        m_stdin->close();

        // It's not really a problem if this signal failed
        if (sigaction(SIGPIPE, &old_action_handler, nullptr) < 0)
            perror("sigaction");
    });

    for (ByteString const& line : lines)
        TRY(m_stdin->write_until_depleted(ByteString::formatted("{}\n", line)));

    return {};
}

ErrorOr<Command::ProcessOutputs> Command::read_all()
{
    return ProcessOutputs { TRY(m_stdout->read_until_eof()), TRY(m_stderr->read_until_eof()) };
}

ErrorOr<Command::ProcessResult> Command::status(int options)
{
    if (m_pid == -1)
        return ProcessResult::Unknown;

    m_stdin->close();

    auto wait_result = TRY(Core::System::waitpid(m_pid, options));
    if (wait_result.pid == 0) {
        // Attempt to kill it, since it has not finished yet somehow
        return ProcessResult::Running;
    }
    m_pid = -1;

    if (WIFSIGNALED(wait_result.status) && WTERMSIG(wait_result.status) == SIGALRM)
        return ProcessResult::FailedFromTimeout;

    if (WIFEXITED(wait_result.status) && WEXITSTATUS(wait_result.status) == 0)
        return ProcessResult::DoneWithZeroExitCode;

    return ProcessResult::Failed;
}

// Only supported in serenity mode because we use `posix_spawn_file_actions_addchdir`
#ifdef AK_OS_SERENITY

ErrorOr<CommandResult> command(ByteString const& command_string, Optional<LexicalPath> chdir)
{
    auto parts = command_string.split(' ');
    if (parts.is_empty())
        return Error::from_string_literal("empty command");
    auto program = parts[0];
    parts.remove(0);
    return command(program, parts, chdir);
}

ErrorOr<CommandResult> command(ByteString const& program, Vector<ByteString> const& arguments, Optional<LexicalPath> chdir)
{
    int stdout_pipe[2] = {};
    int stderr_pipe[2] = {};
    if (pipe2(stdout_pipe, O_CLOEXEC)) {
        return Error::from_errno(errno);
    }
    if (pipe2(stderr_pipe, O_CLOEXEC)) {
        perror("pipe2");
        return Error::from_errno(errno);
    }

    auto close_pipes = ScopeGuard([stderr_pipe, stdout_pipe] {
        // The write-ends of these pipes are closed manually
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
    });

    Vector<char const*> parts = { program.characters() };
    for (auto const& part : arguments) {
        parts.append(part.characters());
    }
    parts.append(nullptr);

    char const** argv = parts.data();

    posix_spawn_file_actions_t action;
    posix_spawn_file_actions_init(&action);
    if (chdir.has_value()) {
        posix_spawn_file_actions_addchdir(&action, chdir.value().string().characters());
    }
    posix_spawn_file_actions_adddup2(&action, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&action, stderr_pipe[1], STDERR_FILENO);

    pid_t pid;
    if ((errno = posix_spawnp(&pid, program.characters(), &action, nullptr, const_cast<char**>(argv), environ))) {
        perror("posix_spawn");
        VERIFY_NOT_REACHED();
    }

    // close the write-ends so reading wouldn't block
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    auto read_all_from_pipe = [](int pipe[2]) -> ErrorOr<ByteBuffer> {
        auto result_file_or_error = Core::File::adopt_fd(pipe[0], Core::File::OpenMode::Read, Core::File::ShouldCloseFileDescriptor::Yes);
        auto result_file = TRY(result_file_or_error);
        return result_file->read_until_eof();
    };
    auto output = TRY(read_all_from_pipe(stdout_pipe));
    auto error = TRY(read_all_from_pipe(stderr_pipe));

    int wstatus { 0 };
    waitpid(pid, &wstatus, 0);
    posix_spawn_file_actions_destroy(&action);
    int exit_code = WEXITSTATUS(wstatus);

    if (exit_code != 0) {
#    ifdef DBG_FAILED_COMMANDS
        dbgln("command failed. stderr: {}", );
#    endif
    }

    return CommandResult { WEXITSTATUS(wstatus), output, error };
}

#endif

}
