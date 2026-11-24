/*
 * Copyright (c) 2024, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/NeverDestroyed.h>
#include <AK/Time.h>
#include <LibCore/Process.h>
#include <LibCore/Socket.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#if defined(AK_OS_MACOS)
#    include <LibCore/MachPort.h>
#    include <LibIPC/TransportBootstrapMach.h>
#endif
#include <LibWebView/Application.h>
#include <LibWebView/BrowserProcess.h>
#include <LibWebView/URL.h>
#include <LibWebView/Utilities.h>

#if defined(AK_OS_MACOS)
#    include <sys/proc.h>
#    include <sys/sysctl.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#else
#    include <errno.h>
#    include <limits.h>
#    include <signal.h>
#    include <sys/file.h>
#    include <unistd.h>
#endif

namespace WebView {

static HashMap<int, RefPtr<UIProcessConnectionFromClient>>& connections()
{
    static NeverDestroyed<HashMap<int, RefPtr<UIProcessConnectionFromClient>>> connections;
    return *connections;
}

// How long a browser that is already running gets to take what this process was asked to open.
static constexpr auto handover_timeout = AK::Duration::from_seconds(5);
// How long a browser that the user chose to replace gets to show that it answers again.
static constexpr auto replaced_browser_handover_timeout = AK::Duration::from_milliseconds(500);
// How long a browser may take to start, during which it does not answer yet.
static constexpr auto browser_startup_time = AK::Duration::from_seconds(30);

// NB: A browser writes its PID file as it starts.
static Optional<UnixDateTime> start_time_of_browser_with(StringView pid_path)
{
    auto pid_file_status = Core::File::stat(pid_path);
    if (pid_file_status.is_error())
        return {};
#if defined(AK_OS_MACOS)
    return UnixDateTime::from_unix_timespec(pid_file_status.value().st_mtimespec);
#elif !defined(AK_OS_WINDOWS)
    return UnixDateTime::from_unix_timespec(pid_file_status.value().st_mtim);
#else
    return UnixDateTime::from_seconds_since_epoch(pid_file_status.value().st_mtime);
#endif
}

// NB: A browser may be busy starting for a while after it wrote its PID file. A launch that comes right after it, as
//     when the user opens two links at once, waits for it longer.
static AK::Duration remaining_startup_time_of_browser(Optional<UnixDateTime> start_time)
{
    if (!start_time.has_value())
        return AK::Duration::zero();
    auto time_since_start = UnixDateTime::now() - *start_time;
    if (time_since_start < AK::Duration::zero() || time_since_start >= browser_startup_time)
        return AK::Duration::zero();
    return browser_startup_time - time_since_start;
}

class UIProcessClient final
    : public IPC::ConnectionToServer<UIProcessClientEndpoint, UIProcessServerEndpoint> {
    C_OBJECT(UIProcessClient);

public:
    // A browser that ends while we hand over to it leaves the profile to us.
    virtual void die() override { }

private:
    explicit UIProcessClient(NonnullOwnPtr<IPC::Transport>);
};

#if !defined(AK_OS_WINDOWS)
// How long another process that is starting on the profile gets to finish. That takes it no longer than a handover to
// a browser that is still starting and the end of an unresponsive browser do.
static constexpr auto startup_lock_timeout = AK::Duration::from_seconds(40);
static constexpr auto browser_exit_timeout = AK::Duration::from_seconds(5);

static bool process_is_gone(pid_t pid)
{
    if (kill(pid, 0) < 0 && errno == ESRCH)
        return true;

    // NB: A process that has ended keeps its PID until its parent takes note, and a parent that is stuck never does.
#    if defined(AK_OS_MACOS)
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    int name[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    if (sysctl(name, 4, &info, &size, nullptr, 0) == 0 && size == sizeof(info))
        return info.kp_proc.p_stat == SZOMB;
#    elif defined(AK_OS_LINUX)
    if (auto file = Core::File::open(ByteString::formatted("/proc/{}/stat", pid), Core::File::OpenMode::Read); !file.is_error()) {
        if (auto contents = file.value()->read_until_eof(); !contents.is_error()) {
            // The state follows the name of the process, which is in parentheses and may itself contain any character.
            auto stat = StringView { contents.value() };
            if (auto end_of_name = stat.find_last(')'); end_of_name.has_value() && *end_of_name + 2 < stat.length())
                return stat[*end_of_name + 2] == 'Z';
        }
    }
#    endif
    return false;
}

#    if defined(AK_OS_MACOS)
// Whether the process with this PID was there when the file was written. A browser writes its PID file after it
// starts, so a process that started later only got the PID of a browser that is gone. We only say yes when we know.
static bool process_may_have_written_file(pid_t pid, StringView path)
{
    auto file_status = Core::System::stat(path);
    if (file_status.is_error())
        return false;
    struct kinfo_proc info {};
    size_t size = sizeof(info);
    int name[] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, pid };
    if (sysctl(name, 4, &info, &size, nullptr, 0) != 0 || size != sizeof(info))
        return false;
    auto const& start_time = info.kp_proc.p_starttime;
    auto process_started_at = UnixDateTime::from_microseconds_since_epoch(static_cast<i64>(start_time.tv_sec) * 1'000'000 + start_time.tv_usec);
    return process_started_at <= UnixDateTime::from_unix_timespec(file_status.value().st_mtimespec);
}
#    endif

#    if !defined(AK_OS_MACOS)
// Whether the process with this PID can be a browser, as far as we can tell. A process that runs another program is
// not, and only got the PID of a browser that is gone.
static bool process_may_be_a_browser([[maybe_unused]] pid_t pid)
{
#        if defined(AK_OS_LINUX)
    auto executable_of = [](StringView process_directory) -> Optional<ByteString> {
        char path[PATH_MAX] = {};
        auto length = ::readlink(ByteString::formatted("{}/exe", process_directory).characters(), path, sizeof(path));
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(path))
            return {};
        return ByteString { path, static_cast<size_t>(length) };
    };
    auto our_executable = executable_of("/proc/self"sv);
    auto process_directory = ByteString::formatted("/proc/{}", pid);
    auto its_executable = executable_of(process_directory);
    if (our_executable.has_value() && its_executable.has_value())
        return *our_executable == *its_executable;
#        endif
    return true;
}
#    endif

static ErrorOr<bool> lock_until(int fd, MonotonicTime deadline)
{
    for (;;) {
        if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            return true;
        if (errno != EWOULDBLOCK && errno != EINTR)
            return Error::from_syscall("flock"sv, errno);
        if (MonotonicTime::now() >= deadline)
            return false;
        usleep(10'000);
    }
}

static bool wait_for_process_to_be_gone(pid_t pid, AK::Duration timeout)
{
    auto deadline = MonotonicTime::now() + timeout;
    while (!process_is_gone(pid)) {
        if (MonotonicTime::now() >= deadline)
            return false;
        usleep(10'000);
    }
    return true;
}

// Whether the browser is gone after this. One that is not still has the profile.
static bool end_browser(pid_t pid)
{
    warnln("Ending the unresponsive browser with PID {}", pid);
    (void)kill(pid, SIGKILL);
    // NB: The browser lets go of the profile as it ends, and its helpers end soon after it.
    return wait_for_process_to_be_gone(pid, browser_exit_timeout);
}

// Whether the browser with this PID wrote that it has a UI in another process, and that process is gone.
static bool ui_of_browser_is_gone(StringView ui_pid_path, pid_t browser_pid)
{
    auto file = Core::File::open(ui_pid_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return false;
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return false;
    auto pids = StringView { contents.value() }.split_view(' ');
    if (pids.size() != 2)
        return false;
    auto written_browser_pid = pids[0].to_number<pid_t>();
    auto ui_pid = pids[1].to_number<pid_t>();
    if (!written_browser_pid.has_value() || !ui_pid.has_value() || *written_browser_pid != browser_pid || *ui_pid <= 0)
        return false;
    return process_is_gone(*ui_pid);
}
#endif

// Reads the PID file and leaves it as it is, which is all that a process without the startup lock may do to it.
static Optional<pid_t> read_pid_file(StringView pid_path)
{
    auto file = Core::File::open(pid_path, Core::File::OpenMode::Read);
    if (file.is_error())
        return {};
    auto contents = file.value()->read_until_eof();
    if (contents.is_error())
        return {};
    return StringView { contents.value() }.to_number<pid_t>();
}

ErrorOr<BrowserProcess::ProcessDisposition> BrowserProcess::connect(Vector<ByteString> const& raw_urls, NewWindow new_window, StringView runtime_directory, [[maybe_unused]] StringView profile_identity, Optional<pid_t> ui_pid, Optional<UnixDateTime> replace_browser_from_before)
{
    static constexpr auto process_name = "Ladybird"sv;

    auto startup_lock_path = LexicalPath::join(runtime_directory, "startup.lock"sv).string();
    auto startup_lock = TRY(Core::File::open(startup_lock_path, Core::File::OpenMode::ReadWrite));
    bool has_startup_lock = true;
#if defined(AK_OS_WINDOWS)
    // FIXME: Stop waiting after a while, as on the other platforms.
    OVERLAPPED overlapped {};
    if (!LockFileEx(to_handle(startup_lock->fd()), LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped))
        return Error::from_windows_error();
#else
    // NB: Whoever has the lock is starting on this profile as well. We do not wait for it here, as one that is stuck
    //     would keep every later launch waiting, with nothing to show the user. Without the lock, we can still reach
    //     a browser that is already running. Becoming the browser of the profile takes the lock, or two processes
    //     could both do it, so that is what we wait for the lock for.
    has_startup_lock = TRY(lock_until(startup_lock->fd(), MonotonicTime::now()));
#endif
    auto wait_for_startup_lock = [&]() -> ErrorOr<void> {
#if !defined(AK_OS_WINDOWS)
        if (!TRY(lock_until(startup_lock->fd(), MonotonicTime::now() + startup_lock_timeout)))
            return Error::from_string_literal("Another process has been starting on this profile for too long");
        has_startup_lock = true;
#endif
        return {};
    };

    auto [socket_path, pid_path] = TRY(Process::paths_for_process(process_name, runtime_directory));
    auto ui_pid_path = ByteString::formatted("{}.ui", pid_path);

    // NB: Each round finds a browser that is gone, or ends one. Other launches wait for the lock meanwhile, so rounds
    //     only run out if something keeps bringing the PID file back.
    static constexpr size_t maximum_rounds = 8;
    for (size_t round = 0;; ++round) {
        // NB: Only the process with the lock changes the PID file. Whoever has the lock may be writing its own PID
        //     file right now, so without the lock, a file that is empty or left over is not ours to remove. All we do
        //     with a browser that we found gone, or ended, is to look again with the lock, for a browser that started
        //     meanwhile.
        auto pid = has_startup_lock ? TRY(Process::get_process_pid(process_name, pid_path)) : read_pid_file(pid_path);
        if (!pid.has_value()) {
            if (has_startup_lock)
                break;
            TRY(wait_for_startup_lock());
            continue;
        }
        if (round == maximum_rounds)
            return Error::from_string_literal("Unable to take over the profile from a browser that is gone");

        // NB: The user has already waited for the browser that they decided to replace, so it only gets a moment to
        //     show that it answers again. A browser that started after the user was asked is not that browser.
        auto start_time = start_time_of_browser_with(pid_path);
        auto user_wants_to_replace_browser = replace_browser_from_before.has_value() && start_time.has_value() && *start_time < *replace_browser_from_before;
        auto timeout = user_wants_to_replace_browser
            ? replaced_browser_handover_timeout
            : max(handover_timeout, remaining_startup_time_of_browser(start_time));
#if defined(AK_OS_MACOS)
        auto result = TRY(connect_as_client(*pid, raw_urls, new_window, profile_identity, timeout));
#else
        auto result_or_error = connect_as_client(socket_path, raw_urls, new_window, timeout);
        if (result_or_error.is_error()) {
            if (!has_startup_lock) {
                TRY(wait_for_startup_lock());
                continue;
            }
            // NB: A browser that ends removes its PID file and then its socket, so we may have read the one and missed
            //     the other. A browser that is still in the PID file is running without a socket that we can reach,
            //     and nothing may start on its profile then.
            if (TRY(Process::get_process_pid(process_name, pid_path)) != pid)
                continue;
            return result_or_error.release_error();
        }
        auto result = result_or_error.release_value();
        // NB: A socket that is missing, or that nothing listens on, does not tell whether the process in the PID file
        //     is gone. That process has to be gone before its files are taken to be left over, unless we know that it
        //     is some other process that got the PID of a browser that is gone.
#    if !defined(AK_OS_WINDOWS)
        if (result.handover == Handover::BrowserIsGone && !result.browser_pid.has_value() && process_may_be_a_browser(*pid))
            result.browser_pid = *pid;
#    endif
#endif
        if (result.handover == Handover::Done)
            return ProcessDisposition::ExitProcess;

#if defined(AK_OS_MACOS)
        // NB: A browser that is ending lets go of its Mach server a while before it removes its PID file. So a process
        //     that is in the PID file without a server may be that browser, and it gets a moment to end. One that is
        //     still there after that has the profile and cannot be reached, like an unresponsive browser. A process
        //     that started after the file was written is some other process that got the PID of a browser that is gone.
        if (result.handover == Handover::BrowserIsGone && !result.browser_pid.has_value() && process_may_have_written_file(*pid, pid_path)) {
            auto deadline = MonotonicTime::now() + browser_exit_timeout;
            while (read_pid_file(pid_path) == pid && !process_is_gone(*pid) && MonotonicTime::now() < deadline)
                usleep(10'000);
            // NB: The PID may have gone to another process while we waited.
            if (read_pid_file(pid_path) == pid && !process_is_gone(*pid) && process_may_have_written_file(*pid, pid_path))
                result = { Handover::BrowserIsUnresponsive, *pid };
        }
#endif

        if (result.handover == Handover::BrowserIsUnresponsive) {
#if defined(AK_OS_WINDOWS)
            // FIXME: End the unresponsive browser when asked to.
            return ProcessDisposition::BrowserIsUnresponsive;
#else
            if (!result.browser_pid.has_value())
                return ProcessDisposition::BrowserIsUnresponsive;
            if (!user_wants_to_replace_browser && !ui_of_browser_is_gone(ui_pid_path, *result.browser_pid))
                return ProcessDisposition::BrowserIsUnresponsive;
            // NB: The browser that we reached is ended on what the PID file told us before we reached it. If another
            //     browser took the profile in between, the file has changed, and we look again.
            if (user_wants_to_replace_browser && (read_pid_file(pid_path) != result.browser_pid || start_time_of_browser_with(pid_path) != start_time))
                continue;
            if (!end_browser(*result.browser_pid))
                return Error::from_string_literal("Unable to end the unresponsive browser that is already running");
#endif
        }

#if !defined(AK_OS_WINDOWS)
        // NB: A browser that turned us away without ending still has the profile, and nothing may start on it then.
        //     One that is ending takes a moment from closing its connections until it is gone.
        if (result.handover == Handover::BrowserIsGone && result.browser_pid.has_value()) {
            if (!wait_for_process_to_be_gone(*result.browser_pid, browser_exit_timeout))
                return Error::from_string_literal("The browser that is already running did not take what it was asked to open");
        }
#endif

        if (!has_startup_lock) {
            TRY(wait_for_startup_lock());
            continue;
        }

        // The PID file is left over from a browser that is gone, which may have the PID of some other process by now.
        if (auto removal = FileSystem::remove(pid_path, FileSystem::RecursionMode::Disallowed); removal.is_error() && FileSystem::exists(pid_path))
            return removal.release_error();
    }

#if defined(AK_OS_MACOS)
    TRY(connect_as_server());
#else
    TRY(connect_as_server(socket_path));
#endif

    m_startup_lock_path = startup_lock_path;
    m_pid_path = pid_path;
    m_pid_file = TRY(Core::File::open(pid_path, Core::File::OpenMode::Write));
    TRY(m_pid_file->write_until_depleted(ByteString::number(Core::System::getpid())));

    // NB: A UI file that an earlier browser left behind names that browser, so it is never taken for ours.
    (void)FileSystem::remove(ui_pid_path, FileSystem::RecursionMode::Disallowed);
    if (ui_pid.has_value()) {
        auto ui_pid_file = TRY(Core::File::open(ui_pid_path, Core::File::OpenMode::Write));
        TRY(ui_pid_file->write_until_depleted(ByteString::formatted("{} {}", Core::System::getpid(), *ui_pid)));
        m_ui_pid_path = move(ui_pid_path);
    }

    return ProcessDisposition::ContinueMainProcess;
}

static BrowserProcess::Handover hand_over(UIProcessClient& client, Vector<ByteString> const& raw_urls, NewWindow new_window, AK::Duration timeout)
{
    auto deadline = MonotonicTime::now() + timeout;
    bool done = false;
    switch (new_window) {
    case NewWindow::Yes:
        done = client.send_sync_until<Messages::UIProcessServer::CreateNewWindow>(deadline, raw_urls);
        break;
    case NewWindow::No:
        done = client.send_sync_until<Messages::UIProcessServer::CreateNewTab>(deadline, raw_urls);
        break;
    }
    if (done)
        return BrowserProcess::Handover::Done;
    // NB: A wait that ends before its deadline found the browser gone. The connection does not tell, as a socket
    //     stays open until its closing is handled, which is after the wait.
    if (client.is_open() && MonotonicTime::now() >= deadline)
        return BrowserProcess::Handover::BrowserIsUnresponsive;
    return BrowserProcess::Handover::BrowserIsGone;
}

#if defined(AK_OS_MACOS)
ErrorOr<BrowserProcess::HandoverResult> BrowserProcess::connect_as_client(pid_t pid, Vector<ByteString> const& raw_urls, NewWindow new_window, StringView profile_identity, AK::Duration handover_timeout)
{
    // NB: Only the browser of this profile with this PID has a server of this name. A PID file that an earlier browser
    //     left behind does not lead to one, whichever process has that PID now.
    auto process_name = ByteString::formatted("Ladybird-{}", profile_identity);
    auto server_port = Core::MachPort::look_up_from_bootstrap_server(mach_server_name_for_process(process_name, pid));
    if (server_port.is_error())
        return HandoverResult { Handover::BrowserIsGone };

    auto transport_ports = IPC::bootstrap_transport_from_server_port(server_port.value(), min(handover_timeout, AK::Duration::from_seconds(5)));
    if (transport_ports.is_error())
        return HandoverResult { process_is_gone(pid) ? Handover::BrowserIsGone : Handover::BrowserIsUnresponsive, pid };

    auto client = UIProcessClient::construct(TRY(IPC::Transport::create(move(transport_ports.value().receive_right), move(transport_ports.value().send_right))));
    return HandoverResult { hand_over(*client, raw_urls, new_window, handover_timeout), pid };
}
#else
ErrorOr<BrowserProcess::HandoverResult> BrowserProcess::connect_as_client(ByteString const& socket_path, Vector<ByteString> const& raw_urls, NewWindow new_window, AK::Duration handover_timeout)
{
    auto socket = Core::LocalSocket::connect(socket_path);
    if (socket.is_error()) {
        // Nothing listens on a socket that a browser left behind. A browser makes its socket before its PID file and
        // removes it after, so a PID file without a socket is left behind as well.
        if (socket.error().is_errno() && (socket.error().code() == ECONNREFUSED || socket.error().code() == ENOENT))
            return HandoverResult { Handover::BrowserIsGone };
        return socket.release_error();
    }

    Optional<pid_t> browser_pid;
#    if !defined(AK_OS_WINDOWS)
    // NB: A peer in another PID namespace has no PID here, and is reported as 0.
    if (auto peer_pid = socket.value()->peer_pid(); !peer_pid.is_error() && peer_pid.value() > 0)
        browser_pid = peer_pid.value();
#    endif

    auto transport = TRY(IPC::Transport::from_socket(socket.release_value()));
    auto client = UIProcessClient::construct(move(transport));
    auto handover = hand_over(*client, raw_urls, new_window, handover_timeout);
#    if defined(AK_OS_WINDOWS)
    // FIXME: Find out whether a browser that turned us away is gone, as on the other platforms. Until then, nothing
    //        starts on its profile.
    if (handover == Handover::BrowserIsGone)
        return Error::from_string_literal("The browser that is already running did not take what it was asked to open");
#    endif
    return HandoverResult { handover, browser_pid };
}
#endif

#if defined(AK_OS_MACOS)
ErrorOr<void> BrowserProcess::connect_as_server()
{
    Application::the().set_browser_process_transport_handler([this](auto transport) {
        accept_transport(move(transport));
    });
    return {};
}
#else
ErrorOr<void> BrowserProcess::connect_as_server(ByteString const& socket_path)
{
    auto socket_fd = TRY(Process::create_ipc_socket(socket_path));
    m_socket_path = socket_path;
    m_local_server = Core::LocalServer::construct();
    TRY(m_local_server->take_over_fd(socket_fd));

    m_local_server->on_accept = [this](auto client_socket) {
        auto transport = IPC::Transport::from_socket(move(client_socket));
        if (transport.is_error()) {
            dbgln("Failed to create IPC transport for UIProcess client: {}", transport.error());
            return;
        }

        accept_transport(transport.release_value());
    };

    return {};
}
#endif

void BrowserProcess::accept_transport(NonnullOwnPtr<IPC::Transport> transport)
{
    auto client = UIProcessConnectionFromClient::construct(move(transport), ++m_next_client_id);
    client->on_new_tab = [this](auto raw_urls) {
        if (this->on_new_tab)
            this->on_new_tab(raw_urls);
    };

    client->on_new_window = [this](auto raw_urls) {
        if (this->on_new_window)
            this->on_new_window(raw_urls);
    };
}

BrowserProcess::~BrowserProcess()
{
#if defined(AK_OS_MACOS)
    Application::the().set_browser_process_transport_handler({});
#endif

#if !defined(AK_OS_WINDOWS)
    // NB: A launch only changes the files of the profile while it has the startup lock, so with the lock we know whose
    //     they are for as long as we remove ours. A launch that has the lock right now is busy with this profile, and
    //     takes what we leave behind to be left over once we are gone. We do not wait for it.
    OwnPtr<Core::File> startup_lock;
    if (m_pid_file) {
        auto lock_file = Core::File::open(m_startup_lock_path, Core::File::OpenMode::ReadWrite);
        if (lock_file.is_error() || flock(lock_file.value()->fd(), LOCK_EX | LOCK_NB) != 0)
            return;
        startup_lock = lock_file.release_value();
    }
#endif

    // NB: A launch that found this browser ending may have taken the PID file to be left over, and put the files of
    //     its own browser in their place. Those files are not ours to remove.
    auto pid_file_is_ours = m_pid_file && read_pid_file(m_pid_path) == Core::System::getpid();

    // NB: The UI file goes first. No other browser starts on the profile for as long as the PID file names us.
    if (pid_file_is_ours && !m_ui_pid_path.is_empty())
        (void)FileSystem::remove(m_ui_pid_path, FileSystem::RecursionMode::Disallowed);

    if (m_pid_file) {
#if defined(AK_OS_WINDOWS)
        MUST(m_pid_file->truncate(0));
        // NOTE: On Windows, be conservative and close the pid file's handle before
        // removing the file; removal of an open file requires cooperative sharing modes.
        m_pid_file->close();
#endif
        // NB: The file names us until it is removed, and is not emptied first. A launch does not start on the profile
        //     while a file names a browser that is still there, but it does when it finds an empty file.
        // NB: That launch may also remove the file right now, so the file can be gone by the time we remove it.
        if (pid_file_is_ours)
            (void)FileSystem::remove(m_pid_path, FileSystem::RecursionMode::Disallowed);
    }

    if (!m_socket_path.is_empty()) {
#if defined(AK_OS_WINDOWS)
        MUST(FileSystem::remove(m_socket_path, FileSystem::RecursionMode::Disallowed));
#else
        if (pid_file_is_ours)
            (void)FileSystem::remove(m_socket_path, FileSystem::RecursionMode::Disallowed);
#endif
    }
}

UIProcessClient::UIProcessClient(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<UIProcessClientEndpoint, UIProcessServerEndpoint>(*this, move(transport))
{
}

UIProcessConnectionFromClient::UIProcessConnectionFromClient(NonnullOwnPtr<IPC::Transport> transport, int client_id)
    : IPC::ConnectionFromClient<UIProcessClientEndpoint, UIProcessServerEndpoint>(*this, move(transport), client_id)
{
    connections().set(client_id, *this);
}

void UIProcessConnectionFromClient::die()
{
    connections().remove(client_id());
}

void UIProcessConnectionFromClient::create_new_tab(Vector<ByteString> urls)
{
    if (on_new_tab)
        on_new_tab(sanitize_urls(urls));
}

void UIProcessConnectionFromClient::create_new_window(Vector<ByteString> urls)
{
    if (on_new_window)
        on_new_window(sanitize_urls(urls));
}

}
