/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibWebView/Process.h>

#include <fcntl.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/ScopeGuard.h>
#    include <AK/Windows.h>
#endif

namespace WebView {

Process::Process(ProcessType type, RefPtr<IPC::ConnectionBase> connection, Core::Process process)
    : m_process(move(process))
    , m_type(type)
    , m_connection(move(connection))
{
}

Process::~Process()
{
    if (m_connection)
        m_connection->shutdown();
}

ErrorOr<Process::ProcessAndIPCTransport> Process::spawn_and_connect_to_process(Core::ProcessSpawnOptions const& options, bool capture_output)
{
    // TODO: Mach IPC

    int socket_fds[2] {};
    TRY(Core::System::socketpair(AF_LOCAL, SOCK_STREAM, 0, socket_fds));

    ArmedScopeGuard guard_fd_0 { [&] { MUST(Core::System::close(socket_fds[0])); } };
    ArmedScopeGuard guard_fd_1 { [&] { MUST(Core::System::close(socket_fds[1])); } };

    // Note: Core::System::socketpair creates inheritable sockets both on Linux and Windows unless SOCK_CLOEXEC is specified.
    TRY(Core::System::set_close_on_exec(socket_fds[0], true));

    auto takeover_string = MUST(String::formatted("{}:{}", options.name, socket_fds[1]));
    TRY(Core::Environment::set("SOCKET_TAKEOVER"sv, takeover_string, Core::Environment::Overwrite::Yes));

    // Set up pipes for stdout/stderr capture if requested
    ProcessOutputCapture output_capture;
    Array<int, 2> stdout_pipe {};
    Array<int, 2> stderr_pipe {};

    Core::ProcessSpawnOptions spawn_options = options;

    if (capture_output) {
        stdout_pipe = TRY(Core::System::pipe2(O_CLOEXEC));
        stderr_pipe = TRY(Core::System::pipe2(O_CLOEXEC));

        // Clear close-on-exec for the write ends so they're inherited by the child
        TRY(Core::System::set_close_on_exec(stdout_pipe[1], false));
        TRY(Core::System::set_close_on_exec(stderr_pipe[1], false));

        // Add file actions to redirect stdout/stderr in the child
        spawn_options.file_actions.append(Core::FileAction::DupFd { .write_fd = stdout_pipe[1], .fd = STDOUT_FILENO });
        spawn_options.file_actions.append(Core::FileAction::DupFd { .write_fd = stderr_pipe[1], .fd = STDERR_FILENO });
        spawn_options.file_actions.append(Core::FileAction::CloseFile { .fd = stdout_pipe[1] });
        spawn_options.file_actions.append(Core::FileAction::CloseFile { .fd = stderr_pipe[1] });
    }

    auto process = TRY(Core::Process::spawn(spawn_options));

    if (capture_output) {
        // Close write ends in parent
        MUST(Core::System::close(stdout_pipe[1]));
        MUST(Core::System::close(stderr_pipe[1]));

        // Wrap read ends in File objects
        output_capture.stdout_file = TRY(Core::File::adopt_fd(stdout_pipe[0], Core::File::OpenMode::Read));
        output_capture.stderr_file = TRY(Core::File::adopt_fd(stderr_pipe[0], Core::File::OpenMode::Read));
    }

    auto ipc_socket = TRY(Core::LocalSocket::adopt_fd(socket_fds[0]));
    guard_fd_0.disarm();
    TRY(ipc_socket->set_blocking(true));

    return ProcessAndIPCTransport { move(process), make<IPC::Transport>(move(ipc_socket)), move(output_capture) };
}

ErrorOr<Optional<pid_t>> Process::get_process_pid(StringView process_name, StringView pid_path)
{
    if (Core::System::stat(pid_path).is_error())
        return OptionalNone {};

    Optional<pid_t> pid;
    {
        auto pid_file = Core::File::open(pid_path, Core::File::OpenMode::Read);
        if (pid_file.is_error()) {
            warnln("Could not open {} PID file '{}': {}", process_name, pid_path, pid_file.error());
            return pid_file.release_error();
        }

        auto contents = pid_file.value()->read_until_eof();
        if (contents.is_error()) {
            warnln("Could not read {} PID file '{}': {}", process_name, pid_path, contents.error());
            return contents.release_error();
        }

        pid = StringView { contents.value() }.to_number<pid_t>();
    }

    if (!pid.has_value()) {
        warnln("{} PID file '{}' exists, but with an invalid PID", process_name, pid_path);
        TRY(Core::System::unlink(pid_path));
        return OptionalNone {};
    }

    bool const process_not_found = [&pid]() {
#if defined(AK_OS_WINDOWS)
        HANDLE process_handle = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, *pid);
        if (process_handle == nullptr)
            return true;

        // FIXME: We should create an RAII wrapper around HANDLE objects.
        ScopeGuard handle_guard = [&process_handle] { CloseHandle(process_handle); };
        DWORD exit_code = 0;

        if (GetExitCodeProcess(process_handle, &exit_code) == 0)
            return true;

        return exit_code != STILL_ACTIVE;
#else
        return kill(*pid, 0) < 0;
#endif
    }();

    if (process_not_found) {
        warnln("{} PID file '{}' exists with PID {}, but process cannot be found", process_name, pid_path, *pid);
        TRY(Core::System::unlink(pid_path));
        return OptionalNone {};
    }

    return pid;
}

// This is heavily based on how SystemServer's Service creates its socket.
ErrorOr<int> Process::create_ipc_socket(ByteString const& socket_path)
{
    if (!Core::System::stat(socket_path).is_error())
        TRY(Core::System::unlink(socket_path));

#if defined(AK_OS_WINDOWS)
    auto socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));
    int option = 1;
    TRY(Core::System::ioctl(socket_fd, FIONBIO, &option));
    if (SetHandleInformation(to_handle(socket_fd), HANDLE_FLAG_INHERIT, 0) == 0)
        return Error::from_windows_error();
#else
#    ifdef SOCK_NONBLOCK
    auto socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
#    else
    auto socket_fd = TRY(Core::System::socket(AF_LOCAL, SOCK_STREAM, 0));

    int option = 1;
    TRY(Core::System::ioctl(socket_fd, FIONBIO, &option));
    TRY(Core::System::fcntl(socket_fd, F_SETFD, FD_CLOEXEC));
#    endif

#    if !defined(AK_OS_BSD_GENERIC) && !defined(AK_OS_GNU_HURD)
    TRY(Core::System::fchmod(socket_fd, 0600));
#    endif
#endif

    auto socket_address = Core::SocketAddress::local(socket_path);
    auto socket_address_un = socket_address.to_sockaddr_un().release_value();

    TRY(Core::System::bind(socket_fd, reinterpret_cast<sockaddr*>(&socket_address_un), sizeof(socket_address_un)));
    TRY(Core::System::listen(socket_fd, 16));

    return socket_fd;
}

ErrorOr<Process::ProcessPaths> Process::paths_for_process(StringView process_name)
{
    auto runtime_directory = TRY(Core::StandardPaths::runtime_directory());
    auto socket_path = ByteString::formatted("{}/{}.socket", runtime_directory, process_name);
    auto pid_path = ByteString::formatted("{}/{}.pid", runtime_directory, process_name);

    return ProcessPaths { move(socket_path), move(pid_path) };
}

}
