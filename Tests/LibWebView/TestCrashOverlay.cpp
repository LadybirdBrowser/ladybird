/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
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
        browser_options.headless_mode.clear();
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_directory = ByteString::formatted("{}/Ladybird-TestCrashOverlay-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_directory, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup = [&] { MUST(FileSystem::remove(test_directory, FileSystem::RecursionMode::Allowed)); };
    VERIFY(setenv("XDG_CONFIG_HOME", test_directory.characters(), 1) == 0);
#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif
    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(theme, { 1000, 700 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 1; });

    auto evaluate = [&](StringView source) {
        Optional<JsonValue> result;
        view->on_request_alert = [&](Utf16String const& value) {
            result = MUST(JsonValue::from_string(value.to_utf8()));
            view->alert_closed();
        };
        view->run_javascript(MUST(String::formatted("alert(JSON.stringify(({}) ?? null))", source)));
        Core::EventLoop::current().spin_until([&] { return result.has_value(); });
        view->on_request_alert = nullptr;
        return result.release_value();
    };
    auto first_url = URL::Parser::basic_parse("data:text/html,<title>First</title>first-page"sv).release_value();
    auto failed_url = URL::Parser::basic_parse("data:text/html,<title>Failed page</title>original-page-content"sv).release_value();
    view->load(first_url);
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 2; });
    view->load(failed_url);
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 3; });
    auto history_step = view->traversable().session_history().current_step();

    auto crash_and_wait_for_screen = [&] {
        view->debug_request("crash-current-page"sv);
        Core::EventLoop::current().spin_until([&] { return view->crash_overlay_active(); });
        VERIFY(view->url() == failed_url);
        VERIFY(view->title() == "Failed page"_utf16);
        VERIFY(view->traversable().session_history().current_step() == history_step);
        VERIFY(view->traversable().session_history().current_entry()->url == failed_url);
        VERIFY(view->crash_overlay_failed_url() == failed_url.serialize());
    };

    crash_and_wait_for_screen();
    view->reload();
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 4; });
    VERIFY(evaluate("document.body.textContent === 'original-page-content'"sv).as_bool());
    VERIFY(evaluate("typeof ladybird === 'undefined'"sv).as_bool());
    VERIFY(view->traversable().session_history().current_step() == history_step);

    // Back from the crash screen must traverse the original history, without an
    // extra crash-document entry or losing the original forward destination.
    Core::EventLoop::current().spin_until([&] { return !view->crash_overlay_active(); });
    crash_and_wait_for_screen();
    view->traverse_the_history_by_delta(-1);
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 5; });
    VERIFY(view->url() == first_url);
    VERIFY(evaluate("document.body.textContent === 'first-page'"sv).as_bool());
    view->traverse_the_history_by_delta(1);
    Core::EventLoop::current().spin_until([&] { return loads_finished >= 6; });
    VERIFY(view->url() == failed_url);
    VERIFY(evaluate("document.body.textContent === 'original-page-content'"sv).as_bool());
    outln("PASS: native crash overlay state, reload and history");
    return 0;
}
