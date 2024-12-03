/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/LexicalPath.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <UI/Headless/Application.h>
#include <UI/Headless/Fixture.h>

namespace Ladybird {

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
};

ErrorOr<void> HttpEchoServerFixture::setup(WebView::WebContentOptions& web_content_options)
{
    auto const script_path = LexicalPath::join(s_fixtures_path, m_script_path);
    auto const arguments = Vector { script_path.string(), "--directory", Ladybird::Application::the().test_root_path };

    // FIXME: Pick a more reasonable log path that is more observable
    auto const log_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), "http-test-server.log"sv).string();

    auto stdout_fds = TRY(Core::System::pipe2(0));

    auto const process_options = Core::ProcessSpawnOptions {
        .executable = Ladybird::Application::the().python_executable_path,
        .search_for_executable_in_path = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::OpenFile { ByteString::formatted("{}.stderr", log_path), Core::File::OpenMode::Write, STDERR_FILENO },
            Core::FileAction::DupFd { stdout_fds[1], STDOUT_FILENO } }
    };

    m_process = TRY(Core::Process::spawn(process_options));

    TRY(Core::System::close(stdout_fds[1]));

    auto const stdout_file = MUST(Core::File::adopt_fd(stdout_fds[0], Core::File::OpenMode::Read));

    auto buffer = MUST(ByteBuffer::create_uninitialized(5));
    TRY(stdout_file->read_some(buffer));

    auto const raw_output = ByteString { buffer, AK::ShouldChomp::NoChomp };

    if (auto const maybe_port = raw_output.to_number<u16>(); maybe_port.has_value())
        web_content_options.echo_server_port = maybe_port.value();
    else
        warnln("Failed to read echo server port from buffer: '{}'", raw_output);

    return {};
}

void HttpEchoServerFixture::teardown_impl()
{
    VERIFY(m_process.has_value());

    auto script_path = LexicalPath::join(s_fixtures_path, m_script_path);

    if (auto kill_or_error = Core::System::kill(m_process->pid(), SIGINT); kill_or_error.is_error()) {
        if (kill_or_error.error().code() != ESRCH) {
            warnln("Failed to kill HTTP echo server, error: {}", kill_or_error.error());
            m_process = {};
            return;
        }
    }

    if (auto termination_or_error = m_process->wait_for_termination(); termination_or_error.is_error()) {
        warnln("Failed to terminate HTTP echo server, error: {}", termination_or_error.error());
    }

    m_process = {};
}

void Fixture::initialize_fixtures()
{
    s_fixtures_path = LexicalPath::join(Ladybird::Application::the().test_root_path, "Fixtures"sv).string();

    auto& registry = all();
    registry.append(make<HttpEchoServerFixture>());
}

}
