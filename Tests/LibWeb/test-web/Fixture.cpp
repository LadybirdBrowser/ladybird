/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fixture.h"
#include "Application.h"

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <fcntl.h>

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
    int m_stdin_fd { -1 };
    OwnPtr<Core::File> m_stdout_file;
};

ErrorOr<void> HttpEchoServerFixture::setup(WebView::WebContentOptions& web_content_options)
{
    auto const script_path = LexicalPath::join(s_fixtures_path, m_script_path);
    // Path where http-test-server.py writes the AIA test root CA. Must match the certificate trusted by
    // TestWeb::Application::create_platform_options.
    auto const ca_cert_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("ladybird-aia-test-ca-{}.pem", Core::System::getpid())).string();
    auto const arguments = Vector { script_path.string(), "--directory", Application::the().test_root_path, "--ca-cert-output", ca_cert_path, "--exit-when-stdin-closes" };

    // FIXME: Pick a more reasonable log path that is more observable
    auto const log_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), "http-test-server.log"sv).string();

    auto stdin_fds = TRY(Core::System::pipe2(O_CLOEXEC, Core::System::PipeReadEnd::ReadExternally));
    auto stdout_fds = TRY(Core::System::pipe2(O_CLOEXEC));
    auto stderr_file = TRY(Core::File::open(ByteString::formatted("{}.stderr", log_path), Core::File::OpenMode::Write));

    auto const process_options = Core::ProcessSpawnOptions {
        .executable = Application::the().python_executable_path,
        .search_for_executable_in_path = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::DupFd { stdin_fds[0], STDIN_FILENO },
            Core::FileAction::DupFd { stderr_file->fd(), STDERR_FILENO },
            Core::FileAction::DupFd { stdout_fds[1], STDOUT_FILENO } }
    };

    m_process = TRY(Core::Process::spawn(process_options));

    TRY(Core::System::close(stdin_fds[0]));
    m_stdin_fd = stdin_fds[1];
    TRY(Core::System::close(stdout_fds[1]));

    m_stdout_file = MUST(Core::File::adopt_fd(stdout_fds[0], Core::File::OpenMode::Read));

    StringBuilder first_line;
    while (true) {
        char byte = 0;
        auto read = TRY(m_stdout_file->read_some({ &byte, 1 }));
        if (read.is_empty() || byte == '\n')
            break;
        first_line.append(byte);
    }
    auto const raw_output = first_line.to_byte_string();

    // The first line holds the HTTP port, followed by the HTTPS port if the server could set one up.
    auto const ports = raw_output.split_view(' ');
    if (auto const maybe_port = ports.is_empty() ? Optional<u16> {} : ports[0].to_number<u16>(); maybe_port.has_value())
        web_content_options.echo_server_port = maybe_port.value();
    else
        warnln("Failed to read echo server port from buffer: '{}'", raw_output);

    if (ports.size() > 1) {
        if (auto const maybe_port = ports[1].to_number<u16>(); maybe_port.has_value())
            web_content_options.https_echo_server_port = maybe_port.value();
        else
            warnln("Failed to read HTTPS echo server port from buffer: '{}'", raw_output);
    }

    return {};
}

void HttpEchoServerFixture::teardown_impl()
{
    VERIFY(m_process.has_value());

    // Close the server's stdin to signal it to exit.
    if (auto close_or_error = Core::System::close(exchange(m_stdin_fd, -1)); close_or_error.is_error())
        warnln("Failed to close HTTP echo server's stdin, error: {}", close_or_error.error());
    else if (auto termination_or_error = m_process->wait_for_termination(); termination_or_error.is_error())
        warnln("Failed to wait for HTTP echo server to exit, error: {}", termination_or_error.error());

    m_process = {};
    m_stdout_file = nullptr;
    auto const ca_cert_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), ByteString::formatted("ladybird-aia-test-ca-{}.pem", Core::System::getpid())).string();
    (void)FileSystem::remove(ca_cert_path, FileSystem::RecursionMode::Disallowed);
}

void Fixture::initialize_fixtures()
{
    s_fixtures_path = LexicalPath::join(Application::the().test_root_path, "Fixtures"sv).string();

    auto& registry = all();
    registry.append(make<HttpEchoServerFixture>());
}

}
