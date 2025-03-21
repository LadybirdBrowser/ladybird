/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Environment.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/StandardPaths.h>
#include <LibWebView/Process.h>

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

ErrorOr<Process::ProcessAndIPCTransport> Process::spawn_and_connect_to_process(Core::ProcessSpawnOptions const& options)
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

    auto process = TRY(Core::Process::spawn(options));

    auto ipc_socket = TRY(Core::LocalSocket::adopt_fd(socket_fds[0]));
    guard_fd_0.disarm();
    TRY(ipc_socket->set_blocking(true));

    return ProcessAndIPCTransport { move(process), IPC::Transport(move(ipc_socket)) };
}

#ifdef AK_OS_WINDOWS
// FIXME: Implement WebView::Process::get_process_pid on Windows
ErrorOr<Optional<pid_t>> Process::get_process_pid(StringView, StringView)
{
    VERIFY(0 && "WebView::Process::get_process_pid is not implemented");
}
// FIXME: Implement WebView::Process::create_ipc_socket on Windows
ErrorOr<int> Process::create_ipc_socket(ByteString const&)
{
    VERIFY(0 && "WebView::Process::create_ipc_socket is not implemented");
}
#else
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
    if (kill(*pid, 0) < 0) {
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

    auto socket_address = Core::SocketAddress::local(socket_path);
    auto socket_address_un = socket_address.to_sockaddr_un().release_value();

    TRY(Core::System::bind(socket_fd, reinterpret_cast<sockaddr*>(&socket_address_un), sizeof(socket_address_un)));
    TRY(Core::System::listen(socket_fd, 16));

    return socket_fd;
}
#endif

ErrorOr<Process::ProcessPaths> Process::paths_for_process(StringView process_name)
{
    auto runtime_directory = TRY(Core::StandardPaths::runtime_directory());
    auto socket_path = ByteString::formatted("{}/{}.socket", runtime_directory, process_name);
    auto pid_path = ByteString::formatted("{}/{}.pid", runtime_directory, process_name);

    return ProcessPaths { move(socket_path), move(pid_path) };
}

}
