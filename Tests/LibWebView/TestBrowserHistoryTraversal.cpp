/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <LibCompositing/InputEvent.h>
#include <LibCompositing/KeyCode.h>
#include <LibCore/Directory.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibHTTP/Cookie/ParsedCookie.h>
#include <LibIPC/Transport.h>
#include <LibMain/Main.h>
#include <LibURL/Parser.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/CookieJar.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <stdlib.h>

namespace {

class TestApplication : public WebView::Application {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    bool has_spare_web_content_process() const
    {
        return WebView::Application::has_spare_web_content_process();
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.allow_popups = WebView::AllowPopups::Yes;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual bool should_coordinate_browser_process() const override { return false; }

    virtual Optional<WebView::ViewImplementation&> open_blank_new_tab(Web::HTML::ActivateTab) const override
    {
        ++m_new_tab_requests;
        return {};
    }

    u64 new_tab_requests() const { return m_new_tab_requests; }

private:
    mutable u64 m_new_tab_requests { 0 };
};

void press_history_traversal_key(WebView::ViewImplementation& view, Compositing::KeyCode key)
{
    view.enqueue_input_event(Compositing::KeyEvent {
        .type = Compositing::KeyEvent::Type::KeyDown,
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
    Core::EventLoop::current().spin_until([&]() { return app->has_spare_web_content_process(); });
    // A page ID that was never handed out is refused by every client, spare ones included.
    WebView::WebContentClient::for_each_client([](auto& client) {
        VERIFY(!client.may_act_for_page(0));
        return IterationDecision::Continue;
    });
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
    // The page a spare process was started with is one its client may act for, and a page ID the client
    // was never given stays refused.
    VERIFY(spare_view->client().may_act_for_page(spare_view->page_id()));
    VERIFY(!spare_view->client().may_act_for_page(0));

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

    // The Back at the start of history has no entry to select. The Forwards behind it, queued in the same turn,
    // traverse one entry each, through B to C.
    view->traverse_the_history_by_delta(-1);
    view->traverse_the_history_by_delta(1);
    view->traverse_the_history_by_delta(1);
    wait_until_at(url_c);

    view->traverse_the_history_by_delta(1);
    wait_until_at(url_d);

    // Page D consumes the first keydown, so only the second press may traverse. A consumed press that wrongly
    // traversed would shift every traversal below by one entry and leave the final wait stuck short of C.
    press_history_traversal_key(*view, Compositing::KeyCode::Key_Left);
    press_history_traversal_key(*view, Compositing::KeyCode::Key_Left);
    wait_until_at(url_c);

    press_history_traversal_key(*view, Compositing::KeyCode::Key_Left);
    wait_until_at(url_b);

    press_history_traversal_key(*view, Compositing::KeyCode::Key_Right);
    wait_until_at(url_c);

    // An absolute target selected from the history menu queues like a button press. The Forward queued behind it
    // selects its entry once A is current, and traverses to B.
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

    // Reopening a closed tab restores its history into a view whose process is still starting up.
    auto closed_tab_history = view->session_history_snapshot();
    VERIFY(closed_tab_history.has_value());
    auto closed_tab_url = view->url();

    // Closing a view may synchronously cause the UI to generate input, geometry, and visibility events while removing
    // the view from its container. Events received after the browsing context closes must be discarded.
    bool did_close = false;
    view->on_close = [&] {
        Compositing::MouseEvent event {};
        event.type = Compositing::MouseEvent::Type::MouseLeave;
        view->enqueue_input_event(move(event));
        view->set_window_position({ 0, 0 });
        view->set_window_size({ 800, 600 });
        view->set_system_visibility_state(Web::HTML::VisibilityState::Hidden);
        did_close = true;
    };
    view->request_close();
    Core::EventLoop::current().spin_until([&] { return did_close; });
    Core::EventLoop::current().spin_until([&] { return app->has_spare_web_content_process(); });

    // Take the spare, so the restored view launches a process of its own the way a private window's tab does. A
    // spare is relaunched from a deferred task, so none is available until the event loop next runs.
    auto restored_theme = TRY(Gfx::load_system_theme(theme_path.string()));
    auto spare_consumer = WebView::HeadlessWebView::create(restored_theme, { 800, 600 });
    VERIFY(!app->has_spare_web_content_process());

    auto restored_view = WebView::HeadlessWebView::create(restored_theme, { 800, 600 });
    VERIFY(&restored_view->client() != &spare_consumer->client());

    // The restored URL is shown before the traversal runs, so wait for the document behind it to load.
    size_t restored_view_loads_finished = 0;
    bool restored_view_traversal_completed = false;
    restored_view->on_load_finish = [&](auto const&) { ++restored_view_loads_finished; };
    restored_view->on_browser_history_traversal_complete = [&] { restored_view_traversal_completed = true; };
    MUST(restored_view->restore_session_history_from_snapshot(closed_tab_history.release_value()));
    Core::EventLoop::current().spin_until([&] {
        return restored_view_loads_finished >= 1
            && restored_view->url() == closed_tab_url
            && restored_view_traversal_completed;
    });
    VERIFY(restored_view->traversable().session_history().current_entry()->url == closed_tab_url);

    // Exercise the record of pages the client was given across popup detachment and close
    // acknowledgement, without pumping the event loop between those transitions.
    OwnPtr<WebView::HeadlessWebView> popup;
    bool popup_loaded = false;
    Compositing::PageId popup_page_id = 0;
    restored_view->on_new_web_view = [&](auto, auto, WebView::WebContentClient& page_process, Optional<Compositing::PageId> page_id) {
        VERIFY(page_id.has_value());
        popup_page_id = *page_id;
        popup = WebView::HeadlessWebView::create_child(*restored_view, page_process, *page_id);
        popup->on_load_finish = [&](auto const&) { popup_loaded = true; };
        return popup->handle();
    };
    restored_view->run_javascript("window.open('about:blank')"_string);
    Core::EventLoop::current().spin_until([&] { return popup_loaded; });
    auto& client = restored_view->client();
    VERIFY(&popup->client() == &client);
    VERIFY(client.may_act_for_page(popup_page_id));
    VERIFY(!client.may_act_for_page(0));
    auto cookie_url = URL::Parser::basic_parse("https://example.com/"sv).release_value();
    HTTP::Cookie::ParsedCookie cookie { .name = "page-lifecycle"_string, .value = "preserved"_string };
    auto& cookie_jar = *client.session().cookie_jar;
    cookie_jar.set_cookie(cookie_url, cookie, HTTP::Cookie::Source::Http);
    VERIFY(cookie_jar.get_named_cookie(cookie_url, cookie.name).has_value());
    auto& stub = static_cast<WebContentClientStub&>(client);
    auto new_tab_requests = app->new_tab_requests();
    stub.did_get_source(popup_page_id, URL::about_blank(), URL::about_blank(), {});
    VERIFY(app->new_tab_requests() == ++new_tab_requests);
    VERIFY(stub.did_request_cookie(popup_page_id, cookie_url, HTTP::Cookie::Source::Http).cookie().cookie == "page-lifecycle=preserved"sv);
    stub.did_request_delete_all_cookies(popup_page_id, 0, cookie_url);
    VERIFY(!cookie_jar.get_named_cookie(cookie_url, cookie.name).has_value());
    cookie_jar.set_cookie(cookie_url, cookie, HTTP::Cookie::Source::Http);
    client.page(popup_page_id)->set_detached_close_pending(true);
    popup.clear();
    VERIFY(!client.page(popup_page_id));
    // The page is gone, but messages sent while the client had it can still arrive, so the client may
    // still name it.
    VERIFY(client.may_act_for_page(popup_page_id));
    VERIFY(!client.may_act_for_page(0));
    stub.did_request_delete_all_cookies(popup_page_id, 0, cookie_url);
    VERIFY(cookie_jar.get_named_cookie(cookie_url, cookie.name).has_value());
    VERIFY(stub.did_request_cookie(popup_page_id, cookie_url, HTTP::Cookie::Source::Http).cookie().cookie.is_empty());
    stub.did_get_source(popup_page_id, URL::about_blank(), URL::about_blank(), {});
    VERIFY(app->new_tab_requests() == new_tab_requests);
    static_cast<WebContentClientStub&>(client).did_close_browsing_context(popup_page_id);
    VERIFY(client.may_act_for_page(popup_page_id));
    VERIFY(!client.may_act_for_page(0));
    stub.did_request_delete_all_cookies(popup_page_id, 0, cookie_url);
    VERIFY(cookie_jar.get_named_cookie(cookie_url, cookie.name).has_value());
    VERIFY(stub.did_request_cookie(popup_page_id, cookie_url, HTTP::Cookie::Source::Http).cookie().cookie.is_empty());
    stub.did_get_source(popup_page_id, URL::about_blank(), URL::about_blank(), {});
    VERIFY(app->new_tab_requests() == new_tab_requests);

    // Rejecting a popup must not authorize an ID that was never assigned to a page.
    Compositing::PageId rejected_page_id = 0;
    restored_view->on_new_web_view = [&](auto, auto, auto&, Optional<Compositing::PageId> page_id) {
        VERIFY(page_id.has_value());
        rejected_page_id = *page_id;
        return String {};
    };
    auto rejected_popup = stub.did_request_new_web_view(restored_view->page_id(), Web::HTML::ActivateTab::No, {}, {}, {}, {}, {});
    VERIFY(!rejected_popup.new_page_id().has_value());
    VERIFY(rejected_page_id != 0);
    VERIFY(!client.may_act_for_page(rejected_page_id));
    restored_view->on_new_web_view = nullptr;

    // An initial page can read cookies before the UI adopts it into a view.
    {
        auto transport = TRY(IPC::Transport::create_paired());
        auto initial_page_id = app->allocate_page_id();
        auto initial_client = adopt_ref(*new WebView::WebContentClient(move(transport.local), client.is_private(), initial_page_id, app->allocate_ui_process_cross_process_id()));
        VERIFY(!initial_client->page(initial_page_id));
        auto& initial_stub = static_cast<WebContentClientStub&>(*initial_client);
        VERIFY(initial_stub.did_request_cookie(initial_page_id, cookie_url, HTTP::Cookie::Source::Http).cookie().cookie == "page-lifecycle=preserved"sv);
    }

    // A navigation from the browser's UI that starts while another's document is being activated does not take that
    // document from it.
    {
        auto view = WebView::HeadlessWebView::create(restored_theme, { 800, 600 });
        auto write_page = [&](StringView name, StringView contents) -> ErrorOr<URL::URL> {
            auto path = ByteString::formatted("{}/{}", test_config_directory, name);
            auto file = TRY(Core::File::open(path, Core::File::OpenMode::Write));
            TRY(file->write_until_depleted(contents.bytes()));
            return URL::create_with_file_scheme(path).release_value();
        };
        auto slow_unload_url = TRY(write_page("slow-unload.html"sv, "<script>addEventListener('unload', () => { const end = performance.now() + 1000; while (performance.now() < end) {} });</script>"sv));
        auto activated_url = TRY(write_page("activated.html"sv, "activated"sv));
        auto newer_url = TRY(write_page("newer.html"sv, "newer"sv));
        Vector<URL::URL> loads_finished;
        view->on_load_finish = [&](URL::URL const& url) { loads_finished.append(url); };
        view->load(slow_unload_url);
        Core::EventLoop::current().spin_until([&] { return loads_finished.contains_slow(slow_unload_url); });

        // The displayed document is still unloading, after its successor's history job was found ready.
        view->load(activated_url);
        bool unloading = false;
        auto timer = Core::Timer::create_single_shot(300, [&] { unloading = true; });
        timer->start();
        Core::EventLoop::current().spin_until([&] { return unloading; });
        view->load(newer_url);
        Core::EventLoop::current().spin_until([&] { return loads_finished.contains_slow(newer_url); });
        VERIFY(view->url() == newer_url);
    }

    auto const& active_entry = restored_view->traversable().active_session_history_entry();
    VERIFY(active_entry);
    auto invalid_mode = static_cast<Web::HTML::ScrollRestorationMode>(to_underlying(Web::HTML::ScrollRestorationMode::Manual) + 1);
    stub.did_update_session_history_entry_scroll_restoration_mode(restored_view->page_id(), restored_view->traversable().id(), active_entry->identity(), invalid_mode);
    VERIFY(!client.is_open());

    outln("PASS: browser history traversal");
    return 0;
}
