/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fixture.h"
#include "Application.h"

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

namespace TestWeb {

static ByteString s_fixtures_path;

// Key function for Fixture
Fixture::~Fixture() = default;

Optional<Fixture&> Fixture::lookup(StringView name)
{
    for (auto const& fixture : all()) {
        if (fixture->name() == name)
            return *fixture;
    }
    return {};
}

Vector<NonnullOwnPtr<Fixture>>& Fixture::all()
{
    static Vector<NonnullOwnPtr<Fixture>> fixtures;
    return fixtures;
}

class HttpEchoServerFixture final : public Fixture {
public:
    virtual ErrorOr<void> setup(WebView::WebContentOptions&) override;
    virtual void teardown_impl() override;
    virtual StringView name() const override { return "HttpEchoServer"sv; }
    virtual bool is_running() const override { return m_process.has_value(); }

private:
    ByteString m_script_path { "http-test-server.py" };
    Optional<Core::Process> m_process;
#if defined(AK_OS_WINDOWS)
    HANDLE m_stderr_read_handle { nullptr };
    HANDLE m_stderr_write_handle { nullptr };
    ByteString m_stderr_log_path {};
#endif
};

ErrorOr<void> HttpEchoServerFixture::setup(WebView::WebContentOptions& web_content_options)
{
    auto const script_path = LexicalPath::join(s_fixtures_path, m_script_path);
    auto const arguments = Vector { script_path.string(), "--directory", Application::the().test_root_path };

    // FIXME: Pick a more reasonable log path that is more observable
    auto const log_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), "http-test-server.log"sv).string();
    auto const stderr_log_path = ByteString::formatted("{}.stderr", log_path);

    auto process_options = Core::ProcessSpawnOptions {
        .executable = Application::the().python_executable_path,
        .search_for_executable_in_path = true,
        .arguments = arguments,
    };

    int stdout_read_fd = -1;

#if defined(AK_OS_WINDOWS)
    (void)Core::System::unlink(stderr_log_path);

    HANDLE stdout_read_handle { nullptr };
    HANDLE stdout_write_handle { nullptr };
    HANDLE stderr_read_handle { nullptr };
    HANDLE stderr_write_handle { nullptr };

    SECURITY_ATTRIBUTES security_attrs = {};
    security_attrs.nLength = sizeof(SECURITY_ATTRIBUTES);
    security_attrs.bInheritHandle = TRUE;
    security_attrs.lpSecurityDescriptor = nullptr;

    if (!CreatePipe(&stdout_read_handle, &stdout_write_handle, &security_attrs, 0))
        return Error::from_windows_error();

    if (!SetHandleInformation(stdout_read_handle, HANDLE_FLAG_INHERIT, 0))
        return Error::from_windows_error();

    if (!CreatePipe(&stderr_read_handle, &stderr_write_handle, &security_attrs, 0))
        return Error::from_windows_error();

    if (!SetHandleInformation(stderr_read_handle, HANDLE_FLAG_INHERIT, 0))
        return Error::from_windows_error();

    process_options.startup_info = Core::StartupInfo::UseStdHandles { .stderr_handle = stderr_write_handle, .stdout_handle = stdout_write_handle, .stdin_handle = nullptr };

    stdout_read_fd = to_fd(stdout_read_handle);
    m_stderr_read_handle = stderr_read_handle;
    m_stderr_write_handle = stderr_write_handle;
    m_stderr_log_path = stderr_log_path;
#else
    auto stdout_fds = TRY(Core::System::pipe2(0));

    process_options.file_actions = {
        Core::FileAction::OpenFile { stderr_log_path, Core::File::OpenMode::Write, STDERR_FILENO },
        Core::FileAction::DupFd { stdout_fds[1], STDOUT_FILENO }
    };

    stdout_read_fd = stdout_fds[0];
#endif

    m_process = TRY(Core::Process::spawn(process_options));

#if defined(AK_OS_WINDOWS)
    CloseHandle(stdout_write_handle);
#else
    TRY(Core::System::close(stdout_fds[1]));
#endif

    auto const stdout_file = MUST(Core::File::adopt_fd(stdout_read_fd, Core::File::OpenMode::Read));

    auto buffer = MUST(ByteBuffer::create_uninitialized(5));
    TRY(stdout_file->read_some(buffer));

    auto const raw_output = ByteString { buffer, AK::ShouldChomp::NoChomp };

    if (auto const maybe_port = raw_output.to_number<u16>(); maybe_port.has_value())
        web_content_options.echo_server_port = maybe_port.value();
    else
        warnln("Failed to read echo server port from buffer: '{}'", raw_output);

#if defined(AK_OS_WINDOWS)
    // FIXME: Currently our File/IPC/Event Loop infrastructure on Windows assumes we don't use pipes, only regular file handles
    // and WinSock2-based socket fds. So currently if we let Core::File try to close the read FD we hit fail an assertion as
    // System::close() thinks we're a socket. Until we have improved our infrastructure to allow for properly handling of Windows
    // pipes, we will just manually close the read FD here
    CloseHandle(to_handle(stdout_file->leak_fd()));
#endif
    return {};
}

void HttpEchoServerFixture::teardown_impl()
{
    VERIFY(m_process.has_value());

    auto script_path = LexicalPath::join(s_fixtures_path, m_script_path);

#if defined(AK_OS_WINDOWS)
    // FIXME: We currently can't create a notifier for the stderr read anonymous pipe handle. So we'll instead wait until we're
    // about to shut down to read all of the stderr data output by the python script and write it out to the log file all at once
    CloseHandle(m_stderr_write_handle);
    auto const stderr_read_file = MUST(Core::File::adopt_fd(to_fd(m_stderr_read_handle), Core::File::OpenMode::Read));

    if (auto maybe_stderr_stream_size = stderr_read_file->size(); !maybe_stderr_stream_size.is_error()) {
        auto const stderr_stream_size = maybe_stderr_stream_size.value();
        if (stderr_stream_size > 0) {
            auto stderr_byte_buffer = MUST(ByteBuffer::create_uninitialized(stderr_stream_size));
            MUST(stderr_read_file->read_some(stderr_byte_buffer));

            auto stderr_log_path_file = MUST(Core::File::open(m_stderr_log_path, Core::File::OpenMode::Write));
            MUST(stderr_log_path_file->write_some(stderr_byte_buffer.bytes()));
            CloseHandle(to_handle(stderr_log_path_file->leak_fd()));
        }
    }

    CloseHandle(to_handle(stderr_read_file->leak_fd()));

    if (!FreeConsole()) {
        warnln("Failed to kill HTTP echo server, error: Unable to free current console");
        return;
    }

    if (!AttachConsole(m_process->pid())) {
        warnln("Failed to kill HTTP echo server, error: Unable to attach to child process console");
        return;
    }

    if (!GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0)) {
        warnln("Failed to kill HTTP echo server, error: Unable to interrupt child process");
        return;
    }
#else
    if (auto kill_or_error = Core::System::kill(m_process->pid(), SIGINT); kill_or_error.is_error()) {
        if (kill_or_error.error().code() != ESRCH) {
            warnln("Failed to kill HTTP echo server, error: {}", kill_or_error.error());
        } else if (auto termination_or_error = m_process->wait_for_termination(); termination_or_error.is_error()) {
            warnln("Failed to terminate HTTP echo server, error: {}", termination_or_error.error());
        }
    }
#endif

    m_process = {};
}

void Fixture::initialize_fixtures()
{
    s_fixtures_path = LexicalPath::join(Application::the().test_root_path, "Fixtures"sv).string();

    auto& registry = all();
    registry.append(make<HttpEchoServerFixture>());
}

}
