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
#include <LibFileSystem/FileSystem.h>
#include <LibGfx/SystemTheme.h>
#include <LibMain/Main.h>
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

}

// WebContent pushes fresh IME state (caret, cursoranchor positions, surrounding text) to the UI on every editing step,
// and platform IMEs compose against the UI-side copy of that state. The UI view must be notified of every push (not
// just the first) so it can invalidate whatever the platform IME has cached: The Qt port answers inputMethodQuery()
// from input_method_state(). And without an updateMicroFocus() nudge per push, the macOS IME composes against the stale
// 1st-keystroke state and drops everything after the first committed character. See #9712: on_input_method_state_change
// must fire for each state push, after the new state is already readable through input_method_state().

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    auto test_config_directory = ByteString::formatted("{}/Ladybird-TestInputMethodState-{}", Core::StandardPaths::tempfile_directory(), generate_random_uuid());
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

    // Wait out the initial about:blank load; navigating before it completes would drop the navigation.
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 1; });

    view->load_html("<!DOCTYPE html><input>"sv);
    Core::EventLoop::current().spin_until([&]() { return loads_finished >= 2; });

    // Runs synchronously inside WebContent's handler, so it is ordered before the commits below.
    view->run_javascript("document.querySelector('input').focus()"_string);

    size_t state_changes = 0;
    WebView::ViewImplementation::InputMethodState observed_state;
    view->on_input_method_state_change = [&]() {
        ++state_changes;
        // The new state must already be stored when the notification fires: The notified view re-queries the state from
        // inside the callback. Qt's updateMicroFocus() makes the platform IME call inputMethodQuery(), which answers
        // from input_method_state().
        observed_state = view->input_method_state();
    };

    // Each committed character round-trips through WebContent, which pushes a fresh input-method state.
    view->commit_text_from_input_method("あ"_utf16);
    Core::EventLoop::current().spin_until([&]() { return state_changes >= 1; });
    VERIFY(observed_state.is_enabled);
    VERIFY(observed_state.cursor_position == 1);
    VERIFY(observed_state.text_before_cursor == "あ"_utf16);
    VERIFY(observed_state.text_after_cursor.is_empty());

    // Composing past the first character depends on the UI being notified for the SECOND push as well (#9712).
    view->commit_text_from_input_method("い"_utf16);
    Core::EventLoop::current().spin_until([&]() { return state_changes >= 2; });
    VERIFY(observed_state.cursor_position == 2);
    VERIFY(observed_state.text_before_cursor == "あい"_utf16);

    // The disabled transition must be pushed and notified too, so the platform IME is turned off for the view.
    view->run_javascript("document.querySelector('input').blur()"_string);
    view->commit_text_from_input_method("x"_utf16);
    Core::EventLoop::current().spin_until([&]() { return state_changes >= 3; });
    VERIFY(!observed_state.is_enabled);
    VERIFY(observed_state.text_before_cursor.is_empty());

    outln("PASS: every input-method state change notified the view");
    return 0;
}
