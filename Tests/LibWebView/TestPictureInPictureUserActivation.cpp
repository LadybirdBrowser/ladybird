/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibCore/EventLoop.h>
#include <LibCore/System.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <signal.h>

namespace {

// A headless window is never shown, so this records whether the browser hid it.
class RecordingPictureInPictureWindow final : public WebView::PictureInPictureWindow {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit RecordingPictureInPictureWindow(NonnullOwnPtr<WebView::PictureInPictureWindow> window)
        : m_window(move(window))
    {
        m_window->on_resize = [this](auto size) {
            if (on_resize)
                on_resize(size);
        };
        m_window->on_page_close = [this] {
            if (on_page_close)
                on_page_close();
        };
    }

    virtual Gfx::IntSize size() const override { return m_window->size(); }
    virtual String handle() const override { return m_window->handle(); }
    virtual void set_video_size(Gfx::IntSize video_size) override { m_window->set_video_size(video_size); }

    virtual void hide() override
    {
        is_hidden = true;
        m_window->hide();
    }

    bool is_hidden { false };

private:
    NonnullOwnPtr<WebView::PictureInPictureWindow> m_window;
};

class TestApplication : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(TestApplication)

public:
    explicit TestApplication(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    // NB: Outside test mode, automation, and the internals object, a renderer cannot synthesize input, so the UI process
    //     honors only user activation from input it delivered. Pop-ups are allowed, so that only the check for a
    //     Picture-in-Picture window can refuse one.
    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::Yes;
        browser_options.allow_popups = WebView::AllowPopups::Yes;
    }

    virtual OwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size) override
    {
        auto window = WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);
        if (!window)
            return {};
        auto recording_window = make<RecordingPictureInPictureWindow>(window.release_nonnull());
        opened_windows.append(recording_window.ptr());
        return recording_window;
    }

    Vector<RecordingPictureInPictureWindow*> opened_windows;
};

void signal_renderers(int signal)
{
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::WebContent)
            MUST(Core::System::kill(process.pid(), signal));
    });
}

void click(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseDown,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .buttons = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
    view.enqueue_input_event(Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseUp,
        .position = position,
        .screen_position = position,
        .button = Web::UIEvents::MouseButton::Primary,
        .click_count = 1,
        .browser_data = nullptr,
    });
}

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

}

// A document needs the activation of the user's input to open a Picture-in-Picture window, unless it has one open, which
// it may replace with one for another of its videos. The window is gone as soon as the user closes it, whatever its
// renderer does, and the document needs that activation again, even before its page has heard that the window closed.

TEST_CASE(picture_in_picture_user_activation)
{
    Test::HeadlessTestEnvironment environment { "TestPictureInPictureUserActivation"sv };

    auto app = TRY_OR_FAIL(environment.create_application<TestApplication>());
    VERIFY(!app->blocks_pop_ups());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(move(theme), { 800, 600 });

    size_t loads_finished = 0;
    view->on_load_finish = [&](auto const&) { ++loads_finished; };

    Utf16String last_title;
    view->on_title_change = [&](auto const& title) { last_title = title; };

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    // A button filling the viewport asks for a window for the first of two videos, and the page reports how each request
    // went through its title.
    view->load_html(MUST(String::formatted(
        "<!DOCTYPE html>"
        "<button onclick=\"enter(first)\" style=\"position:fixed;left:0;top:0;width:800px;height:600px\">Enter</button>"
        "<video id=first src=\"{0}\"></video>"
        "<video id=second src=\"{0}\"></video>"
        "<script>"
        "function enter(video) {{"
        "    video.requestPictureInPicture().then(() => {{ document.title = `${{video.id}} entered`; }}, error => {{ document.title = `${{video.id}} ${{error.name}}`; }});"
        "}}"
        "Promise.all([first, second].map(video => new Promise(resolve => {{"
        "    if (video.readyState >= HTMLMediaElement.HAVE_METADATA) resolve(); else video.addEventListener('loadedmetadata', resolve, {{ once: true }});"
        "}}))).then(() => {{ document.title = 'ready'; }});"
        "</script>",
        video_url)));
    Core::EventLoop::current().spin_until([&]() { return last_title == "ready"sv; });

    click(*view, { 400, 300 });
    Core::EventLoop::current().spin_until([&]() { return last_title.starts_with("first "sv); });
    VERIFY(last_title == "first entered"sv);
    VERIFY(app->opened_windows.size() == 1);
    outln("a click opens a window");

    // The window took the click's activation, so the document opens the next window as the one with a window open.
    view->run_javascript("enter(second)"_string);
    Core::EventLoop::current().spin_until([&]() { return last_title.starts_with("second "sv); });
    VERIFY(last_title == "second entered"sv);
    VERIFY(app->opened_windows.size() == 2);
    outln("a document replaces its window without a click");

    // The window is gone as soon as the user closes it, even while its renderer cannot answer.
    auto& closed_window = *app->opened_windows.last();
    signal_renderers(SIGSTOP);
    closed_window.on_close(WebView::PictureInPictureWindow::VideoKeepsPlaying::Yes);
    VERIFY(closed_window.is_hidden);
    outln("the window hides as the user closes it, while its renderer is stopped");

    // The page hears that the user closed the window only after it asks for another, so it asks as a document that has a
    // window open.
    view->run_javascript("enter(first)"_string);
    signal_renderers(SIGCONT);
    Core::EventLoop::current().spin_until([&]() { return last_title.starts_with("first "sv); });
    VERIFY(last_title == "first InvalidStateError"sv);
    VERIFY(app->opened_windows.size() == 2);
    outln("a document needs a click again once the user closes its window");
}
