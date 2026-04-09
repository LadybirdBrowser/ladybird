/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fixture.h"
#include "Application.h"

#include <AK/ByteBuffer.h>
#include <AK/JsonParser.h>
#include <AK/LexicalPath.h>
#include <LibCore/File.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>

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

class TestServerFixture final : public Fixture {
public:
    virtual ErrorOr<void> setup(WebView::WebContentOptions&) override;
    virtual void teardown_impl() override;
    virtual StringView name() const override { return "TestServer"sv; }
    virtual bool is_running() const override { return m_process.has_value(); }

private:
    ByteString m_script_path { "test-server.py" };
    Optional<Core::Process> m_process;
    static constexpr size_t DEFAULT_NUM_ORIGINS = 3;
};

#if defined(AK_OS_WINDOWS)

ErrorOr<void> TestServerFixture::setup(WebView::WebContentOptions&)
{
    VERIFY(0 && "TestServerFixture::setup is not implemented on Windows");
}

void TestServerFixture::teardown_impl()
{
    VERIFY(0 && "TestServerFixture::teardown_impl is not implemented on Windows");
}

#else

ErrorOr<void> TestServerFixture::setup(WebView::WebContentOptions& web_content_options)
{
    auto const script_path = LexicalPath::join(s_fixtures_path, m_script_path);
    auto const arguments = Vector<ByteString> {
        script_path.string(),
        "--directory",
        Application::the().test_root_path,
        "--num-origins",
        ByteString::number(DEFAULT_NUM_ORIGINS)
    };

    auto const log_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), "test-server.log"sv).string();

    auto stdout_fds = TRY(Core::System::pipe2(0));

    auto const process_options = Core::ProcessSpawnOptions {
        .executable = Application::the().python_executable_path,
        .search_for_executable_in_path = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::OpenFile { ByteString::formatted("{}.stderr", log_path), Core::File::OpenMode::Write, STDERR_FILENO },
            Core::FileAction::DupFd { stdout_fds[1], STDOUT_FILENO } }
    };

    m_process = TRY(Core::Process::spawn(process_options));

    TRY(Core::System::close(stdout_fds[1]));

    auto const stdout_file = MUST(Core::File::adopt_fd(stdout_fds[0], Core::File::OpenMode::Read));

    auto buffer = MUST(ByteBuffer::create_uninitialized(1024));
    auto bytes_read = TRY(stdout_file->read_some(buffer));

    auto const raw_output = StringView { bytes_read }.trim_whitespace();

    auto json_or_error = JsonParser::parse(raw_output);
    if (json_or_error.is_error()) {
        warnln("Failed to parse test server output: '{}' - {}", raw_output, json_or_error.error());
        return Error::from_string_literal("Failed to parse test server JSON output");
    }

    auto json = json_or_error.release_value();
    if (!json.is_object() || !json.as_object().has_array("origins"sv)) {
        warnln("Invalid test server JSON format: '{}'", raw_output);
        return Error::from_string_literal("Invalid test server JSON format");
    }

    auto const& origins = json.as_object().get_array("origins"sv).value();
    for (auto const& origin : origins.values()) {
        if (!origin.is_object() || !origin.as_object().has_u64("port"sv))
            continue;
        auto port = static_cast<u16>(origin.as_object().get_u64("port"sv).value());
        web_content_options.multi_origin_server_ports.append(port);
    }

    if (web_content_options.multi_origin_server_ports.is_empty()) {
        warnln("No ports found in test server output");
        return Error::from_string_literal("No ports found in test server output");
    }

    // Origin 0 serves as the echo server for backward compatibility
    web_content_options.echo_server_port = web_content_options.multi_origin_server_ports[0];

    return {};
}

void TestServerFixture::teardown_impl()
{
    VERIFY(m_process.has_value());

    if (auto kill_or_error = Core::System::kill(m_process->pid(), SIGINT); kill_or_error.is_error()) {
        if (kill_or_error.error().code() != ESRCH) {
            warnln("Failed to kill test server, error: {}", kill_or_error.error());
        } else if (auto termination_or_error = m_process->wait_for_termination(); termination_or_error.is_error()) {
            warnln("Failed to terminate test server, error: {}", termination_or_error.error());
        }
    }

    m_process = {};
}

#endif

void Fixture::initialize_fixtures()
{
    s_fixtures_path = LexicalPath::join(Application::the().test_root_path, "Fixtures"sv).string();

    auto& registry = all();
    registry.append(make<TestServerFixture>());
}

}
