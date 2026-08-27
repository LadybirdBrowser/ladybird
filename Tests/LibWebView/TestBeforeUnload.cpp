/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWeb/Page/InputEvent.h>
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
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

void click(WebView::ViewImplementation& view)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = { 10, 10 },
        .screen_position = { 10, 10 },
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = { 10, 10 },
        .screen_position = { 10, 10 },
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::None,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestBeforeUnload-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    VERIFY(setenv("XDG_CONFIG_HOME", test_config_directory.characters(), 1) == 0);

#if defined(LADYBIRD_BINARY_PATH)
    auto app = TRY(TestApplication::create(arguments, LADYBIRD_BINARY_PATH));
#else
    auto app = TRY(TestApplication::create(arguments, OptionalNone {}));
#endif

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    Core::EventLoop::current().spin_until([&] { return loads_finished == 1; });

    auto source_url = URL::Parser::basic_parse("data:text/html,<title>source</title><button style='width:100px;height:100px' onclick=\"document.title='activated'\">Activate</button><script>onbeforeunload=e=>e.preventDefault()</script>"sv).release_value();
    auto destination_url = URL::Parser::basic_parse("data:text/html,<title>destination</title>Destination"sv).release_value();

    auto view_is_at = [&](URL::URL const& url) { return view->url().serialize() == url.serialize(); };
    auto load_and_wait = [&](URL::URL const& url) {
        auto expected_loads_finished = loads_finished + 1;
        view->load(url);
        Core::EventLoop::current().spin_until([&] { return loads_finished >= expected_loads_finished && view_is_at(url); });
    };
    auto activate_page = [&] {
        bool activated = false;
        view->on_title_change = [&](auto const& title) { activated = title == "activated"sv; };
        click(*view);
        Core::EventLoop::current().spin_until([&] { return activated; });
        view->on_title_change = {};
    };

    // Frontends without a beforeunload handler must act as if the user chose Leave.
    load_and_wait(source_url);
    activate_page();
    load_and_wait(destination_url);

    size_t prompt_count = 0;
    bool accept_prompt = false;
    Optional<String> prompt_source;
    view->on_request_before_unload = [&](auto const& source, auto on_complete) {
        ++prompt_count;
        prompt_source = source;
        on_complete(accept_prompt);
    };

    // A cancellation request without sticky activation must not show a prompt.
    load_and_wait(source_url);
    load_and_wait(destination_url);
    VERIFY(prompt_count == 0);

    // Stay cancels the navigation. The opaque data: origin uses the scheme fallback.
    load_and_wait(source_url);
    activate_page();
    auto loads_finished_before_cancellation = loads_finished;
    view->load(destination_url);
    Core::EventLoop::current().spin_until([&] { return loads_finished > loads_finished_before_cancellation; });
    VERIFY(prompt_count == 1);
    VERIFY(prompt_source == "data://"sv);
    VERIFY(view_is_at(source_url));

    // A subsequent navigation may ask once again, and Leave allows it to proceed.
    accept_prompt = true;
    load_and_wait(destination_url);
    VERIFY(prompt_count == 2);

    outln("PASS: beforeunload prompt handling");
    return 0;
}
