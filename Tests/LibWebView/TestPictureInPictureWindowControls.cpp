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
#include <AK/Vector.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibCore/Timer.h>
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/Cursor.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
#include <LibWebCommon/Page/InputEvent.h>
#include <LibWebView/Application.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/ProcessManager.h>
#include <LibWebView/Utilities.h>
#include <signal.h>

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

    virtual OwnPtr<WebView::PictureInPictureWindow> create_picture_in_picture_window(WebView::WebContentPage& requesting_page, WebView::CanonicalTraversable& traversable, Gfx::IntSize video_size) override
    {
        auto window = WebView::Application::create_picture_in_picture_window(requesting_page, traversable, video_size);
        if (window) {
            picture_in_picture_window_handle = window->handle();
            picture_in_picture_window_size = window->size();
        }
        return window;
    }

    String picture_in_picture_window_handle;
    Gfx::IntSize picture_in_picture_window_size;
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

void wait_for(int milliseconds)
{
    bool elapsed = false;
    auto timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(milliseconds), [&] { elapsed = true; });
    timer->start();
    Core::EventLoop::current().spin_until([&] { return elapsed; });
}

void signal_renderers(int signal)
{
    WebView::Application::process_manager().for_each_process([&](WebView::Process& process) {
        if (process.type() == WebView::ProcessType::WebContent)
            MUST(Core::System::kill(process.pid(), signal));
    });
}

void send_mouse_event(WebView::ViewImplementation& view, Web::MouseEvent::Type type, Web::DevicePixelPoint position, Web::UIEvents::MouseButton button, Web::UIEvents::MouseButton buttons)
{
    view.enqueue_input_event(Web::MouseEvent {
        .type = type,
        .position = position,
        .screen_position = position,
        .button = button,
        .buttons = buttons,
        .click_count = type == Web::MouseEvent::Type::MouseDown ? 1 : 0,
        .browser_data = nullptr,
    });
}

void move_mouse_to(WebView::ViewImplementation& view, Web::DevicePixelPoint position, Web::UIEvents::MouseButton buttons = Web::UIEvents::MouseButton::None)
{
    send_mouse_event(view, Web::MouseEvent::Type::MouseMove, position, Web::UIEvents::MouseButton::None, buttons);
}

void press_mouse_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    send_mouse_event(view, Web::MouseEvent::Type::MouseDown, position, Web::UIEvents::MouseButton::Primary, Web::UIEvents::MouseButton::Primary);
}

void release_mouse_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    send_mouse_event(view, Web::MouseEvent::Type::MouseUp, position, Web::UIEvents::MouseButton::Primary, Web::UIEvents::MouseButton::None);
}

void click_at(WebView::ViewImplementation& view, Web::DevicePixelPoint position)
{
    press_mouse_at(view, position);
    release_mouse_at(view, position);
}

bool is_cursor(Gfx::Cursor const& cursor, Gfx::StandardCursor standard_cursor)
{
    return cursor.has<Gfx::StandardCursor>() && cursor.get<Gfx::StandardCursor>() == standard_cursor;
}

}

// The browser's controls over a Picture-in-Picture window's page take the presses on them, which the page never sees,
// and show their own cursor and tooltip in place of the page's while hovered. They work while the page's renderer is
// stopped.

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestPictureInPictureWindowControls-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
    TRY(Core::Directory::create(test_config_directory, Core::Directory::CreateDirectories::Yes));
    auto cleanup_test_config_directory = ScopeGuard([&] {
        MUST(FileSystem::remove(test_config_directory, FileSystem::RecursionMode::Allowed));
    });
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, test_config_directory, Core::Environment::Overwrite::Yes));

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
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    Utf16String last_title;
    view->on_title_change = [&](auto const& title) { last_title = title; };
    Gfx::Cursor last_cursor { Gfx::StandardCursor::Arrow };
    view->on_cursor_change = [&](auto const& cursor) { last_cursor = cursor; };
    Optional<ByteString> tooltip;
    view->on_enter_tooltip_area = [&](auto const& text) { tooltip = text; };
    view->on_leave_tooltip_area = [&]() { tooltip.clear(); };
    Vector<Compositing::PictureInPictureControl> activated_controls;
    view->on_picture_in_picture_control_activated = [&](auto control) { activated_controls.append(control); };

    view->enable_picture_in_picture_controls();

    // The page logs where it is pressed, released and dragged, and its top-left corner has a cursor and tooltip of its
    // own.
    view->load_html(R"~~~(<!DOCTYPE html>
<title>page saw</title>
<body style="margin:0">
<div title="Page tooltip" style="position:fixed;top:0;left:0;width:100px;height:100px;cursor:crosshair"></div>
<script>
function log(name, event) { document.title += " " + name + " " + event.clientX; }
addEventListener("pointerdown", event => log("down", event));
addEventListener("pointerup", event => log("up", event));
addEventListener("pointermove", event => { if (event.buttons) log("drag", event); });
</script>
</body>)~~~"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });
    VERIFY(last_title == "page saw"sv);

    // The Close button is centred at (24, 24) and Back to tab at (776, 24). (11, 11) is in the corner of Close's bounds,
    // over the page's corner but outside the round button.
    Web::DevicePixelPoint close_button { 24, 24 };
    Web::DevicePixelPoint back_to_tab_button { 776, 24 };
    Web::DevicePixelPoint corner_beside_close_button { 11, 11 };
    Web::DevicePixelPoint middle_of_page { 400, 300 };

    move_mouse_to(*view, close_button);
    VERIFY(is_cursor(last_cursor, Gfx::StandardCursor::Hand));
    VERIFY(tooltip == "Close"sv);
    // The page reports its cursor and tooltip apart from finishing the move, so the wait is for those.
    Core::EventLoop::current().spin_until([&]() {
        return is_cursor(view->page_cursor_for_testing(), Gfx::StandardCursor::Crosshair) && view->page_tooltip_for_testing() == "Page tooltip"sv;
    });
    VERIFY(is_cursor(last_cursor, Gfx::StandardCursor::Hand));
    VERIFY(tooltip == "Close"sv);
    outln("a hovered control shows its own cursor and tooltip over the page's");

    move_mouse_to(*view, corner_beside_close_button);
    VERIFY(is_cursor(last_cursor, Gfx::StandardCursor::Crosshair));
    VERIFY(tooltip == "Page tooltip"sv);
    outln("the page's cursor and tooltip return outside the round control");

    move_mouse_to(*view, close_button);
    click_at(*view, close_button);
    VERIFY(activated_controls.size() == 1 && activated_controls[0] == Compositing::PictureInPictureControl::Close);
    outln("a click on a control activates it");

    move_mouse_to(*view, back_to_tab_button);
    press_mouse_at(*view, back_to_tab_button);
    move_mouse_to(*view, middle_of_page, Web::UIEvents::MouseButton::Primary);
    release_mouse_at(*view, middle_of_page);
    VERIFY(activated_controls.size() == 1);
    outln("a press on a control released elsewhere does nothing");

    click_at(*view, corner_beside_close_button);

    // Once the pointer leaves, the controls hide and a press where one was is the page's.
    send_mouse_event(*view, Web::MouseEvent::Type::MouseLeave, {}, Web::UIEvents::MouseButton::None, Web::UIEvents::MouseButton::None);
    click_at(*view, close_button);
    VERIFY(activated_controls.size() == 1);

    click_at(*view, middle_of_page);
    Core::EventLoop::current().spin_until([&]() { return last_title.ends_with(" up 400"sv); });
    VERIFY(last_title == "page saw down 11 up 11 down 24 up 24 down 400 up 400"sv);
    outln("the page sees only the presses off the shown controls");

    signal_renderers(SIGSTOP);
    move_mouse_to(*view, close_button);
    click_at(*view, close_button);
    signal_renderers(SIGCONT);
    VERIFY(activated_controls.size() == 2 && activated_controls[1] == Compositing::PictureInPictureControl::Close);
    outln("a control works while the page's renderer is stopped");

    // The window cancels a press on the page that it takes over to move itself.
    move_mouse_to(*view, middle_of_page);
    VERIFY(view->picture_in_picture_controls_are_shown_for_testing());
    press_mouse_at(*view, middle_of_page);
    send_mouse_event(*view, Web::MouseEvent::Type::MouseCancel, {}, Web::UIEvents::MouseButton::None, Web::UIEvents::MouseButton::None);
    VERIFY(!view->picture_in_picture_controls_are_shown_for_testing());
    outln("a cancelled press hides the controls at once");

    // A real window's page shows a player, whose controls fade together with the window's. The video is moved off its
    // first frame, as its controls otherwise stay shown over it.
    view->load_html(MUST(String::formatted(
        "<!DOCTYPE html>"
        "<button onclick=\"video.requestPictureInPicture().then(() => {{ document.title = 'entered'; }})\" style=\"position:fixed;left:0;top:0;width:800px;height:600px\"></button>"
        "<video id=video src=\"{}\"></video>"
        "<script>"
        "video.addEventListener('seeked', () => {{ document.title = 'ready'; }}, {{ once: true }});"
        "video.currentTime = 0.04;"
        "</script>",
        video_url)));
    Core::EventLoop::current().spin_until([&]() { return last_title == "ready"sv; });
    click_at(*view, middle_of_page);
    Core::EventLoop::current().spin_until([&]() { return last_title == "entered"sv; });

    auto& window_view = WebView::ViewImplementation::find_view_by_handle(app->picture_in_picture_window_handle).value();
    window_view.enable_picture_in_picture_controls();
    auto window_size = app->picture_in_picture_window_size;
    // The player's timeline is in the middle of the row along its bottom, inside 12 px of padding.
    Web::DevicePixelPoint player_control_bar { window_size.width() / 2, window_size.height() - 23 };
    Web::DevicePixelPoint window_close_button { 24, 24 };
    // The player's play button is in the middle of the window, so the page is pressed and rested on beside it.
    Web::DevicePixelPoint beside_play_button { window_size.width() / 4, window_size.height() / 2 };

    // The player's control bar shows on the first move and is hovered from the next.
    move_mouse_to(window_view, player_control_bar);
    Core::EventLoop::current().spin_until([&]() { return window_view.picture_in_picture_player_controls_are_shown_for_testing(); });
    move_mouse_to(window_view, player_control_bar);
    wait_for(1500);
    VERIFY(window_view.picture_in_picture_player_controls_are_shown_for_testing());
    VERIFY(window_view.picture_in_picture_controls_are_shown_for_testing());
    outln("the window's controls stay shown while the player's control bar is hovered");

    move_mouse_to(window_view, window_close_button);
    wait_for(1500);
    VERIFY(window_view.picture_in_picture_player_controls_are_shown_for_testing());
    outln("the player's controls stay shown while a window control is hovered");

    move_mouse_to(window_view, beside_play_button);
    Core::EventLoop::current().spin_until([&]() { return !window_view.picture_in_picture_controls_are_shown_for_testing(); });
    VERIFY(!window_view.picture_in_picture_player_controls_are_shown_for_testing());
    outln("both fade once the pointer rests away from them");

    // A press on the page can move onto a control before the window takes it over.
    Gfx::Cursor window_cursor { Gfx::StandardCursor::Arrow };
    window_view.on_cursor_change = [&](auto const& cursor) { window_cursor = cursor; };
    Optional<ByteString> window_tooltip;
    window_view.on_enter_tooltip_area = [&](auto const& text) { window_tooltip = text; };
    window_view.on_leave_tooltip_area = [&]() { window_tooltip.clear(); };
    move_mouse_to(window_view, beside_play_button);
    press_mouse_at(window_view, beside_play_button);
    move_mouse_to(window_view, window_close_button, Web::UIEvents::MouseButton::Primary);
    VERIFY(is_cursor(window_cursor, Gfx::StandardCursor::Hand));
    VERIFY(window_tooltip == "Close"sv);
    send_mouse_event(window_view, Web::MouseEvent::Type::MouseCancel, {}, Web::UIEvents::MouseButton::None, Web::UIEvents::MouseButton::None);
    VERIFY(!is_cursor(window_cursor, Gfx::StandardCursor::Hand));
    VERIFY(window_tooltip != "Close"sv);
    Core::EventLoop::current().spin_until([&]() {
        return !window_view.picture_in_picture_controls_are_shown_for_testing() && !window_view.picture_in_picture_player_controls_are_shown_for_testing();
    });
    outln("a cancelled press leaves no control hovered, so both fade");

    // The page sees none of a press on a control, even once the press moves off it.
    move_mouse_to(window_view, window_close_button);
    press_mouse_at(window_view, window_close_button);
    move_mouse_to(window_view, beside_play_button, Web::UIEvents::MouseButton::Primary);
    wait_for(1500);
    VERIFY(window_view.picture_in_picture_player_controls_are_shown_for_testing());
    release_mouse_at(window_view, beside_play_button);
    Core::EventLoop::current().spin_until([&]() {
        return !window_view.picture_in_picture_controls_are_shown_for_testing() && !window_view.picture_in_picture_player_controls_are_shown_for_testing();
    });
    outln("the player's controls stay shown for the rest of a press on a window control");

    // The controls are where the displayed frame shows them, at the pixel ratio it was painted at, until a frame at a new
    // ratio replaces it. (16, 16) is inside Close at 1x but outside it at 2x, and (48, 48) is the reverse.
    Vector<Compositing::PictureInPictureControl> window_activated_controls;
    window_view.on_picture_in_picture_control_activated = [&](auto control) { window_activated_controls.append(control); };
    Web::DevicePixelPoint close_button_at_1x { 16, 16 };
    Web::DevicePixelPoint close_button_at_2x { 48, 48 };

    signal_renderers(SIGSTOP);
    window_view.set_device_pixel_ratio_for_testing(window_view.page(), 2.0);
    move_mouse_to(window_view, close_button_at_1x);
    click_at(window_view, close_button_at_1x);
    VERIFY(window_activated_controls.size() == 1);
    signal_renderers(SIGCONT);
    outln("a control is where the displayed frame shows it while a frame at a new pixel ratio is pending");

    Core::EventLoop::current().spin_until([&]() { return window_view.displayed_frame_device_pixel_ratio_for_testing() == 2.0; });
    move_mouse_to(window_view, close_button_at_2x);
    click_at(window_view, close_button_at_2x);
    VERIFY(window_activated_controls.size() == 2);
    move_mouse_to(window_view, close_button_at_1x);
    click_at(window_view, close_button_at_1x);
    VERIFY(window_activated_controls.size() == 2);
    outln("a control follows the frame at the new pixel ratio once it is displayed");

    return 0;
}
