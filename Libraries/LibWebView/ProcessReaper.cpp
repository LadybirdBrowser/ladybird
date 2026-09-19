/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibCore/Process.h>
#include <LibCore/System.h>
#include <LibWebView/ProcessReaper.h>
#include <LibWebView/ProcessReaperProtocol.h>
#include <fcntl.h>
#include <unistd.h>

namespace WebView {

// The reaper reads the helper records from this descriptor.
static constexpr int reaper_pipe_fd = 3;

ErrorOr<NonnullOwnPtr<ProcessReaper>> ProcessReaper::start(ByteString const& executable_path)
{
    // Both ends are close-on-exec, so that no helper inherits the write end: the reaper acts when the last copy of the
    // write end is closed, which must happen when this process is gone.
    auto pipe_fds = TRY(Core::System::pipe2(O_CLOEXEC));
    ScopeGuard close_read_end = [&] { (void)Core::System::close(pipe_fds[0]); };
    ArmedScopeGuard close_write_end = [&] { (void)Core::System::close(pipe_fds[1]); };

    // If the reaper is gone, writing to the pipe must fail instead of killing us with SIGPIPE.
    if (::fcntl(pipe_fds[1], F_SETNOSIGPIPE, 1) < 0)
        return Error::from_syscall("fcntl(F_SETNOSIGPIPE)"sv, errno);

    // If the read end already is the reaper's descriptor number, the child-side dup2() would do nothing and leave it
    // close-on-exec. Move it to a descriptor that cannot collide.
    if (pipe_fds[0] <= reaper_pipe_fd) {
        auto moved_fd = TRY(Core::System::fcntl(pipe_fds[0], F_DUPFD_CLOEXEC, reaper_pipe_fd + 1));
        (void)Core::System::close(pipe_fds[0]);
        pipe_fds[0] = moved_fd;
    }

    Vector<ByteString> arguments;
    auto process = TRY(Core::Process::spawn({
        .name = "ProcessReaper"sv,
        .executable = executable_path,
        .arguments = arguments,
        .environment = Vector<ByteString> {},
        .file_actions = {
            Core::FileAction::OpenFile { .path = "/dev/null", .mode = Core::File::OpenMode::Read, .fd = STDIN_FILENO },
            Core::FileAction::DupFd { .write_fd = pipe_fds[0], .fd = reaper_pipe_fd },
        },
    }));

    auto process_reaper = TRY(adopt_nonnull_own_or_enomem(new (nothrow) ProcessReaper(pipe_fds[1], process.pid())));
    close_write_end.disarm();
    return process_reaper;
}

ProcessReaper::ProcessReaper(int pipe_fd, pid_t pid)
    : m_pipe_fd(pipe_fd)
    , m_pid(pid)
{
}

ProcessReaper::~ProcessReaper()
{
    // Closing the pipe tells the reaper that we are gone, as our exit or death would.
    if (m_pipe_fd >= 0)
        (void)Core::System::close(m_pipe_fd);
}

ErrorOr<void> ProcessReaper::watch(pid_t pid)
{
    // The helper is our child, and we have not reaped it yet, so its PID still identifies it here. If it is already
    // gone, there is nothing left to kill.
    ProcessReaperRecord record { .pid = pid, .padding = 0, .start_time_in_microseconds = 0 };
    if (!process_start_time_in_microseconds(pid, record.start_time_in_microseconds))
        return {};

    // Writes of this size to a pipe are atomic, so the reaper always reads whole records.
    auto bytes = TRY(Core::System::write(m_pipe_fd, { &record, sizeof(record) }));
    if (bytes != sizeof(record))
        return Error::from_errno(EIO);
    return {};
}

}
