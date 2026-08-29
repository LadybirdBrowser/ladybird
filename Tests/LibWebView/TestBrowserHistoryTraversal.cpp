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
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>
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

    bool has_ready_spare_web_content_process() const
    {
        return WebView::Application::has_ready_spare_web_content_process();
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }
};

void press_history_traversal_key(WebView::ViewImplementation& view, Web::UIEvents::KeyCode key)
{
    view.enqueue_input_event(Web::KeyEvent {
        .type = Web::KeyEvent::Type::KeyDown,
        .key = key,
        .modifiers = WebView::ViewImplementation::history_traversal_key_modifier(),
        .code_point = 0,
        .browser_data = nullptr,
    });
}

}

// Browser-UI traversals resolve their target at their queue position. Presses made before that position compose into
// one traversal, and a press made while it is loading supersedes its target.
//
// The browser's back and forward keys are matched in the UI process once WebContent reports them unhandled, so a
// page's keydown handler must be able to consume them, and an unconsumed press must traverse the session history.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestBrowserHistoryTraversal-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });

    size_t loads_started = 0;
    view->on_load_start = [&] { ++loads_started; };

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    size_t browser_history_traversals_completed = 0;
    view->on_browser_history_traversal_complete = [&] { ++browser_history_traversals_completed; };

    size_t expected_loads = 1;
    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= expected_loads; });

    // A spare can create its initial traversable before a view adopts it. Preserve that entry across assignment.
    Core::EventLoop::current().spin_until([&]() { return app->has_ready_spare_web_content_process(); });
    auto spare_view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    VERIFY(spare_view->traversable().session_history().current_step() == 0);
    VERIFY(spare_view->traversable().session_history().current_entry());
    VERIFY(spare_view->traversable().session_history().current_entry()->url == URL::about_blank());

    size_t spare_view_loads_finished = 0;
    spare_view->on_load_finish = [&](auto const&) { ++spare_view_loads_finished; };
    auto spare_view_url = URL::Parser::basic_parse("data:text/html,spare-process-navigation"sv).release_value();
    spare_view->load(spare_view_url);
    Core::EventLoop::current().spin_until([&]() { return spare_view_loads_finished == 1; });
    VERIFY(spare_view->url() == spare_view_url);

    auto url_a = URL::Parser::basic_parse("data:text/html,<title>A</title>first"sv).release_value();
    auto url_b = URL::Parser::basic_parse("data:text/html,<title>B</title>second"sv).release_value();
    auto url_c = URL::Parser::basic_parse("data:text/html,<title>C</title>third"sv).release_value();
    auto url_d = URL::Parser::basic_parse("data:text/html,<title>D</title><script>window.preventNextKeydown=true;addEventListener('keydown',e=>{if(window.preventNextKeydown){window.preventNextKeydown=false;e.preventDefault()}})</script>fourth"sv).release_value();

    auto view_is_at = [&](URL::URL const& url) { return view->url().serialize() == url.serialize(); };
    // Waiting for the load count as well as the URL keeps the next step from racing the destination document.
    auto wait_until_at = [&](URL::URL const& url, size_t new_loads = 1) {
        expected_loads += new_loads;
        Core::EventLoop::current().spin_until([&]() { return loads_finished >= expected_loads && view_is_at(url); });
    };

    view->load(url_a);
    wait_until_at(url_a);
    view->load(url_b);
    wait_until_at(url_b);
    view->load(url_c);
    wait_until_at(url_c);
    view->load(url_d);
    wait_until_at(url_d);

    auto back_menu_items = view->session_history_traversal_menu_items(-1);
    VERIFY(back_menu_items.size() == 3);
    VERIFY(back_menu_items[0].step == 2);
    VERIFY(back_menu_items[1].step == 1);
    VERIFY(back_menu_items[2].step == 0);

    view->traverse_the_history_by_delta(-1);
    wait_until_at(url_c);
    view->traverse_the_history_by_delta(-1);
    wait_until_at(url_b);
    view->traverse_the_history_by_delta(-1);
    wait_until_at(url_a);

    // The Back at the start of history has no entry to select. The two Forwards behind it still compose, selecting C
    // in one traversal rather than loading the intermediate entry B.
    view->traverse_the_history_by_delta(-1);
    view->traverse_the_history_by_delta(1);
    view->traverse_the_history_by_delta(1);
    wait_until_at(url_c);

    view->traverse_the_history_by_delta(1);
    wait_until_at(url_d);

    // Page D consumes the first keydown, so only the second press may traverse. A consumed press that wrongly
    // traversed would shift every traversal below by one entry and leave the final wait stuck short of C.
    press_history_traversal_key(*view, Web::UIEvents::KeyCode::Key_Left);
    press_history_traversal_key(*view, Web::UIEvents::KeyCode::Key_Left);
    wait_until_at(url_c);

    press_history_traversal_key(*view, Web::UIEvents::KeyCode::Key_Left);
    wait_until_at(url_b);

    press_history_traversal_key(*view, Web::UIEvents::KeyCode::Key_Right);
    wait_until_at(url_c);

    // An absolute target selected from the history menu shares the pending slot with button presses. The Forward
    // therefore retargets the queued traversal from A to B instead of creating a second operation.
    view->traverse_the_history_to_step(back_menu_items.last().step);
    view->traverse_the_history_by_delta(1);
    wait_until_at(url_b);

    view->traverse_the_history_by_delta(1);
    wait_until_at(url_c);

    // A javascript: URL that evaluates without producing a document still terminates the UI-initiated load. Keep its
    // start and cancellation notifications paired so the cancellation is correlated with this load.
    auto javascript_url = URL::Parser::basic_parse("javascript:void(0)"sv).release_value();
    auto loads_started_before_javascript_url = loads_started;
    auto loads_finished_before_javascript_url = loads_finished;
    view->load(javascript_url);
    Core::EventLoop::current().spin_until([&] { return loads_finished > loads_finished_before_javascript_url; });
    VERIFY(loads_started == loads_started_before_javascript_url + 1);
    VERIFY(view_is_at(url_c));

    view->traverse_the_history_to_step(back_menu_items.last().step);
    wait_until_at(url_a);

    auto completed_traversals_before_history_boundary = browser_history_traversals_completed;
    bool history_boundary_traversal_ready = false;
    view->traverse_the_history_by_delta(-1, WebView::CheckForCancelation::Yes, [&] {
        history_boundary_traversal_ready = true;
    });
    Core::EventLoop::current().spin_until([&] { return history_boundary_traversal_ready; });
    VERIFY(browser_history_traversals_completed == completed_traversals_before_history_boundary + 1);

    // Closing a view may synchronously cause the UI to generate input and visibility events while removing the view
    // from its container. Events received after the browsing context closes must be discarded.
    bool did_close = false;
    view->on_close = [&] {
        Web::MouseEvent event {};
        event.type = Web::MouseEvent::Type::MouseLeave;
        view->enqueue_input_event(move(event));
        view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
        did_close = true;
    };
    view->request_close();
    Core::EventLoop::current().spin_until([&] { return did_close; });
    Core::EventLoop::current().spin_until([&] { return app->has_ready_spare_web_content_process(); });

    outln("PASS: browser history traversal");
    return 0;
}
