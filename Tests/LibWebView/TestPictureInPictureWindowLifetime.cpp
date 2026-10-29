/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <signal.h>

namespace {

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions& web_content_options) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        web_content_options.is_test_mode = WebView::IsTestMode::Yes;
    }

    virtual OwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size) override
    {
        auto window = WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);
        if (window) {
            picture_in_picture_window = window.ptr();
            picture_in_picture_window_handle = window->handle();
        }
        return window;
    }

    bool has_spare_web_content_process() const
    {
        return WebView::Application::has_spare_web_content_process();
    }

    WebView::PictureInPictureWindow* picture_in_picture_window { nullptr };
    String picture_in_picture_window_handle;
};

// Tests/LibWeb/Assets/solid-blue-160x120.webm
constexpr auto video_url = "data:video/webm;base64,"
                           "GkXfo59ChoEBQveBAULygQRC84EIQoKEd2VibUKHgQJChYECGFOAZwEAAAAAAAJHEU2bdLpNu4tTq4QVSalmU6yBoU27i1OrhBZUrmtTrIHYTb"
                           "uMU6uEElTDZ1OsggElTbuMU6uEHFO7a1OsggIx7AEAAAAAAABZAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
                           "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAVSalmsirXsYMPQkBNgI1MYXZmNjIuMTIuMTAwV0GNTGF2ZjYyLj"
                           "EyLjEwMESJiEBUAAAAAAAAFlSua8iuAQAAAAAAAD/XgQFzxYi7y8SD17A9dpyBACK1nIN1bmSIgQCGhVZfVlA5g4EBI+ODhAJiWgDgkLCBoLqB"
                           "eJqBAlWwhFW5gQESVMNnQIBzc6BjwIBnyJpFo4dFTkNPREVSRIeNTGF2ZjYyLjEyLjEwMHNz2mPAi2PFiLvLxIPXsD12Z8ilRaOHRU5DT0RFUk"
                           "SHmExhdmM2Mi4yOC4xMDAgbGlidnB4LXZwOWfIoUWjiERVUkFUSU9ORIeTMDA6MDA6MDAuMDgwMDAwMDAwAB9DtnVAgOeBAKPkgQAAgIJJg0IA"
                           "CfAHdgY4JBwYQgAA8EfY/WIM2+nMNg69o5wAAGsolj+HwagtlxsozzSBUQjvp5qKfWffW0n9FjLeyZR10gJc8B6pWRwBd9w2DbjItsNWdXtxBq"
                           "bMrifsah2MQKOVgQAoAIYAQJKcKElAAANwAABZ79oAHFO7a5G7j7OBALeK94EB8YIBq/CBAw=="sv;

bool spin_until_or_timeout(Function<bool()> condition, int timeout_ms)
{
    bool timed_out = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(timeout_ms), [&] { timed_out = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return condition() || timed_out; });
    return condition();
}

void signal_renderers(int signal)
{
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::WebContent)
            MUST(Core::System::kill(process.pid(), signal));
    });
}

void click_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    for (auto type : { Web::MouseEvent::Type::MouseDown, Web::MouseEvent::Type::MouseUp }) {
        view.enqueue_input_event(Web::MouseEvent {
            .type = type,
            .position = position,
            .screen_position = position,
            .button = Web::UIEvents::MouseButton::Primary,
            .buttons = type == Web::MouseEvent::Type::MouseDown ? Web::UIEvents::MouseButton::Primary : Web::UIEvents::MouseButton::None,
            .click_count = type == Web::MouseEvent::Type::MouseDown ? 1 : 0,
            .browser_data = nullptr,
        });
    }
}

bool process_is_running(pid_t pid)
{
    return ::kill(pid, 0) == 0;
}

NonnullOwnPtr<WebView::HeadlessWebView> open_tab_with_picture_in_picture_window(TestApplication& app, Core::AnonymousBuffer theme)
{
    // A view that takes the spare process finds its initial document loaded already.
    auto takes_spare_process = app.has_spare_web_content_process();
    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });
    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };
    if (!takes_spare_process)
        Core::EventLoop::current().spin_until([&] { return loads_finished >= 1; });

    Utf16String title;
    view->on_title_change = [&](auto const& new_title) { title = new_title; };
    view->load_html(MUST(String::formatted(
        "<!DOCTYPE html>"
        "<button onclick=\"video.requestPictureInPicture().then(() => {{ document.title = 'entered'; }})\" style=\"position:fixed;left:0;top:0;width:800px;height:600px\"></button>"
        "<video id=video src=\"{}\"></video>"
        "<script>"
        "video.addEventListener('loadeddata', () => {{ document.title = 'ready'; }}, {{ once: true }});"
        "</script>",
        video_url)));
    Core::EventLoop::current().spin_until([&] { return title == "ready"sv; });

    app.picture_in_picture_window = nullptr;
    click_at(*view, { 400, 300 });
    Core::EventLoop::current().spin_until([&] { return title == "entered"sv && app.picture_in_picture_window; });
    view->on_title_change = {};
    view->on_load_finish = {};
    return view;
}

// A browser window closes its tabs without waiting for their processes, and destroys their views at once.
void close_tab_as_a_browser_window_does(NonnullOwnPtr<WebView::HeadlessWebView> view)
{
    auto close = view->prepare_for_close_without_prompting();
    close();
}

}

// A Picture-in-Picture window's page lives in the process of the tab that opened it, so it must not outlive that tab.

TEST_CASE(picture_in_picture_window_lifetime)
{
    Test::HeadlessTestEnvironment environment { "TestPictureInPictureWindowLifetime"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto window_and_process_are_gone = [&](StringView window_handle, pid_t pid) {
        return spin_until_or_timeout([&] {
            return !WebView::ViewImplementation::find_view_by_handle(window_handle).has_value() && !process_is_running(pid);
        },
            3000);
    };

    {
        auto view = open_tab_with_picture_in_picture_window(*app, theme);
        auto window_handle = app->picture_in_picture_window_handle;
        auto pid = view->page().client().pid();

        close_tab_as_a_browser_window_does(move(view));
        VERIFY(window_and_process_are_gone(window_handle, pid));
        outln("the window and the process of a closed tab go away, even when the tab's process never unloads it");
    }

    {
        auto view = open_tab_with_picture_in_picture_window(*app, theme);
        auto window_handle = app->picture_in_picture_window_handle;
        auto pid = view->page().client().pid();

        // The user closes the window while the tab's renderer cannot answer, so the window is retired with its page open.
        signal_renderers(SIGSTOP);
        app->picture_in_picture_window->on_close(WebView::PictureInPictureWindow::VideoKeepsPlaying::Yes);
        close_tab_as_a_browser_window_does(move(view));
        signal_renderers(SIGCONT);
        VERIFY(window_and_process_are_gone(window_handle, pid));
        outln("a window the user closed before its tab goes away with the tab's process too");
    }
}
