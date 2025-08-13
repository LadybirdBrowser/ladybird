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

}
