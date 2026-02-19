/*
 * Copyright (c) 2026, Jeet Shah <jeetsh4h@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWebView/ReservedBrowserShortcuts.h>

using Web::UIEvents::KeyCode;
using Web::UIEvents::KeyModifier;

static Web::KeyEvent make_key_event(Web::KeyEvent::Type type, KeyCode key, KeyModifier modifiers)
{
    return {
        .type = type,
        .key = key,
        .modifiers = modifiers,
        .code_point = 0,
        .repeat = false,
        .browser_data = nullptr,
    };
}

static Web::InputEvent make_mouse_event()
{
    return Web::MouseEvent {
        .type = Web::MouseEvent::Type::MouseMove,
        .position = {},
        .screen_position = {},
        .button = Web::UIEvents::MouseButton::None,
        .buttons = Web::UIEvents::MouseButton::None,
        .modifiers = Web::UIEvents::KeyModifier::Mod_None,
        .wheel_delta_x = 0,
        .wheel_delta_y = 0,
        .browser_data = nullptr,
    };
}

TEST_CASE(reserved_shortcuts_are_detected)
{
    auto reserved_shortcuts = Array {
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_T, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_W, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_N, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_Tab, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_Tab, KeyModifier::Mod_PlatformCtrl | KeyModifier::Mod_Shift),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_PageDown, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_PageUp, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_1, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_2, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_3, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_4, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_5, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_6, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_7, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_8, KeyModifier::Mod_PlatformCtrl),
        make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_9, KeyModifier::Mod_PlatformCtrl),
    };

    for (auto const& shortcut : reserved_shortcuts)
        EXPECT(WebView::ReservedBrowserShortcuts::is_reserved(shortcut));
}

TEST_CASE(non_reserved_shortcuts_are_rejected)
{
    EXPECT(!WebView::ReservedBrowserShortcuts::is_reserved(make_key_event(Web::KeyEvent::Type::KeyUp, KeyCode::Key_T, KeyModifier::Mod_PlatformCtrl)));
    EXPECT(!WebView::ReservedBrowserShortcuts::is_reserved(make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_T, KeyModifier::Mod_None)));
    EXPECT(!WebView::ReservedBrowserShortcuts::is_reserved(make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_K, KeyModifier::Mod_PlatformCtrl)));
    EXPECT(!WebView::ReservedBrowserShortcuts::is_reserved(make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_0, KeyModifier::Mod_PlatformCtrl)));
    EXPECT(!WebView::ReservedBrowserShortcuts::is_reserved(make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_PageDown, KeyModifier::Mod_PlatformCtrl | KeyModifier::Mod_Shift)));
}

TEST_CASE(non_captured_events_are_redispatched)
{
    auto non_reserved_key = Web::InputEvent { make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_K, KeyModifier::Mod_None) };
    auto mouse_event = make_mouse_event();

    EXPECT(WebView::should_redispatch_input_event(non_reserved_key, Web::EventResult::Accepted));
    EXPECT(WebView::should_redispatch_input_event(non_reserved_key, Web::EventResult::Dropped));
    EXPECT(WebView::should_redispatch_input_event(mouse_event, Web::EventResult::Accepted));
    EXPECT(WebView::should_redispatch_input_event(mouse_event, Web::EventResult::Dropped));
}

TEST_CASE(cancelled_reserved_shortcut_is_redispatched)
{
    auto reserved_key = Web::InputEvent { make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_T, KeyModifier::Mod_PlatformCtrl) };
    EXPECT(WebView::should_redispatch_input_event(reserved_key, Web::EventResult::Cancelled));
}

TEST_CASE(handled_or_cancelled_events_only_redispatch_reserved_shortcuts)
{
    auto reserved_key = Web::InputEvent { make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_T, KeyModifier::Mod_PlatformCtrl) };
    auto non_reserved_key = Web::InputEvent { make_key_event(Web::KeyEvent::Type::KeyDown, KeyCode::Key_K, KeyModifier::Mod_PlatformCtrl) };
    auto mouse_event = make_mouse_event();

    EXPECT(WebView::should_redispatch_input_event(reserved_key, Web::EventResult::Handled));
    EXPECT(WebView::should_redispatch_input_event(reserved_key, Web::EventResult::Cancelled));

    EXPECT(!WebView::should_redispatch_input_event(non_reserved_key, Web::EventResult::Handled));
    EXPECT(!WebView::should_redispatch_input_event(non_reserved_key, Web::EventResult::Cancelled));
    EXPECT(!WebView::should_redispatch_input_event(mouse_event, Web::EventResult::Handled));
    EXPECT(!WebView::should_redispatch_input_event(mouse_event, Web::EventResult::Cancelled));
}
