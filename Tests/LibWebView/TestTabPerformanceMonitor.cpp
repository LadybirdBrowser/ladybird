/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibRequests/RequestClient.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/TabPerformanceMonitor.h>
#include <LibWebView/Utilities.h>
#include <stdlib.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto directory = ByteString::formatted("{}/Ladybird-TestTabPerformanceMonitor-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(directory, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup = [&] { MUST(FileSystem::remove(directory, FileSystem::RecursionMode::Allowed)); };
    VERIFY(setenv("XDG_CONFIG_HOME", directory.characters(), 1) == 0);
#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    auto& monitor = WebView::TabPerformanceMonitor::the();
    VERIFY(!monitor.enabled());

    u64 samples = 0;
    u64 pushes = 0;
    Optional<double> cpu;
    view->on_performance_stats = [&](WebView::TabPerformanceStats const& stats) {
        ++samples;
        cpu = stats.cpu_percent;
    };
    auto enable = [&] {
        WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::ShowTabPerformanceMonitor, true);
        auto& requests = WebView::Application::request_server_client();
        auto callback = move(requests.on_network_usage);
        requests.on_network_usage = [&, callback = move(callback)](Vector<Requests::NetworkUsage> usage, u64 interval) {
            VERIFY(interval > 0);
            ++pushes;
            callback(move(usage), interval);
        };
    };
    auto wait_for_samples = [&](u64 minimum_samples) -> ErrorOr<void> {
        bool timed_out = false;
        auto watchdog = Core::Timer::create_single_shot(20'000, [&] { timed_out = true; });
        watchdog->start();
        Core::EventLoop::current().spin_until([&] { return timed_out || (samples >= minimum_samples && pushes >= 2 && cpu.has_value()); });
        if (timed_out)
            return Error::from_string_literal("Timed out waiting for performance samples and network pushes");
        return {};
    };
    enable();
    // Wait for events, not a fixed sleep or an assumed sampling cadence. This detects a configured
    // timer that was never started, in both the browser and RequestServer.
    TRY(wait_for_samples(3));
    WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::ShowTabPerformanceMonitor, false);
    VERIFY(!monitor.enabled());
    VERIFY(!WebView::Application::request_server_client().on_network_usage);
    auto previous_samples = samples;
    pushes = 0;
    enable();
    VERIFY(!cpu.has_value());
    TRY(wait_for_samples(previous_samples + 3));
    WebView::Application::settings().set_config_variable(WebView::ConfigVariableID::ShowTabPerformanceMonitor, false);
    outln("Tab performance sampling, asynchronous pushes and re-enabling passed.");
    return 0;
}
