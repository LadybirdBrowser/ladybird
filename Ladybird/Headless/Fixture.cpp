/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <Ladybird/Headless/Application.h>
#include <Ladybird/Headless/Fixture.h>
#include <LibCore/Process.h>
#include <LibCore/StandardPaths.h>

namespace Ladybird {

static ByteString s_fixtures_path;

// Key function for Fixture
Fixture::~Fixture() = default;

Optional<Fixture&> Fixture::lookup(StringView name)
{
    for (auto& fixture : all()) {
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
    virtual ErrorOr<void> setup() override;
    virtual void teardown_impl() override;
    virtual StringView name() const override { return "HttpEchoServer"sv; }
    virtual bool is_running() const override { return m_process.has_value(); }

private:
    ByteString m_script_path { "http-test-server.py" };
    Optional<Core::Process> m_process;
};

ErrorOr<void> HttpEchoServerFixture::setup()
{
    auto script_path = LexicalPath::join(s_fixtures_path, m_script_path);

    // FIXME: Pick a more reasonable log path that is more observable
    auto log_path = LexicalPath::join(Core::StandardPaths::tempfile_directory(), "http-test-server.log"sv).string();

    auto arguments = Vector { script_path.string(), "start", "--directory", Ladybird::Application::the().test_root_path };
    auto process_options = Core::ProcessSpawnOptions {
        .executable = Ladybird::Application::the().python_executable_path,
        .search_for_executable_in_path = true,
        .arguments = arguments,
        .file_actions = {
            Core::FileAction::OpenFile { ByteString::formatted("{}.stdout", log_path), Core::File::OpenMode::Write, STDOUT_FILENO },
            Core::FileAction::OpenFile { ByteString::formatted("{}.stderr", log_path), Core::File::OpenMode::Write, STDERR_FILENO },
        }
    };

    m_process = TRY(Core::Process::spawn(process_options));

    return {};
}

void HttpEchoServerFixture::teardown_impl()
{
    VERIFY(m_process.has_value());

    auto script_path = LexicalPath::join(s_fixtures_path, m_script_path);

    auto ret = Core::System::kill(m_process->pid(), SIGINT);
    if (ret.is_error() && ret.error().code() != ESRCH) {
        warnln("Failed to kill http-test-server.py: {}", ret.error());
        m_process = {};
        return;
    }

    MUST(m_process->wait_for_termination());

    m_process = {};
}

void Fixture::initialize_fixtures()
{
    s_fixtures_path = LexicalPath::join(Ladybird::Application::the().test_root_path, "Fixtures"sv).string();

    auto& registry = all();
    registry.append(make<HttpEchoServerFixture>());
}

}
