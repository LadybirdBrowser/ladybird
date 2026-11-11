/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>
#include <signal.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    bool has_spare_web_content_process() const
    {
        return WebView::Application::has_spare_web_content_process();
    }
};

Core::AnonymousBuffer load_theme()
{
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    return MUST(Gfx::load_system_theme(theme_path.string()));
}

pid_t wait_for_spare_web_content_process(TestApplication const& app)
{
    Core::EventLoop::current().spin_until([&] { return app.has_spare_web_content_process(); });

    Optional<pid_t> spare_pid;
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::WebContent && process.title() == "(spare)"_utf16)
            spare_pid = process.pid();
    });
    VERIFY(spare_pid.has_value());
    return *spare_pid;
}

bool connection_to_web_content_process_is_open(pid_t pid)
{
    bool is_open = false;
    WebView::WebContentClient::for_each_client([&](WebView::WebContentClient& client) {
        if (client.pid() != pid)
            return IterationDecision::Continue;
        is_open = client.is_open();
        return IterationDecision::Break;
    });
    return is_open;
}

}

TEST_CASE(spare_is_dropped_when_its_process_exits)
{
    Test::HeadlessTestEnvironment environment { "TestSpareWebContentProcess"sv };
    HashTable<pid_t> exited_pids;
    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());
    auto theme = load_theme();

    auto& process_manager = WebView::Application::process_manager();
    process_manager.on_process_exited = [&, handle_exit = move(process_manager.on_process_exited)](WebView::Process&& process, Optional<int> exit_status) {
        auto pid = process.pid();
        handle_exit(move(process), exit_status);
        exited_pids.set(pid);
    };

    auto first_tab = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto spare_pid = wait_for_spare_web_content_process(*app);

    MUST(Core::System::kill(spare_pid, SIGKILL));
    Core::EventLoop::current().spin_until([&] { return exited_pids.contains(spare_pid); });
    EXPECT(!app->has_spare_web_content_process());

    auto second_tab = WebView::HeadlessWebView::create(theme, { 800, 600 });
    EXPECT(second_tab->page().is_open());
    EXPECT_NE(second_tab->web_content_pid(), spare_pid);
}

TEST_CASE(spare_is_not_taken_once_its_connection_closed)
{
    Test::HeadlessTestEnvironment environment { "TestSpareWebContentProcess"sv };
    Optional<WebView::Process> held_spare_exit;
    Optional<int> held_spare_exit_status;
    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());
    auto theme = load_theme();

    auto first_tab = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto spare_pid = wait_for_spare_web_content_process(*app);

    // The application handles the spare's exit only once the test lets it.
    auto& process_manager = WebView::Application::process_manager();
    auto handle_exit = move(process_manager.on_process_exited);
    process_manager.on_process_exited = [&](WebView::Process&& process, Optional<int> exit_status) {
        if (process.pid() != spare_pid) {
            handle_exit(move(process), exit_status);
            return;
        }
        held_spare_exit = move(process);
        held_spare_exit_status = exit_status;
    };

    MUST(Core::System::kill(spare_pid, SIGKILL));
    Core::EventLoop::current().spin_until([&] {
        return held_spare_exit.has_value() && !connection_to_web_content_process_is_open(spare_pid);
    });

    auto second_tab = WebView::HeadlessWebView::create(theme, { 800, 600 });
    EXPECT_NE(second_tab->web_content_pid(), spare_pid);

    handle_exit(held_spare_exit.release_value(), held_spare_exit_status);
    process_manager.on_process_exited = move(handle_exit);
    EXPECT(second_tab->page().is_open());
}
