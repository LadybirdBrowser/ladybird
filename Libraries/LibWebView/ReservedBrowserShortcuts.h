/*
 * Copyright (c) 2026, Jeet Shah <jeetsh4h@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <LibWeb/Page/EventResult.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/UIEvents/KeyCode.h>

namespace WebView {

class ReservedBrowserShortcuts {
public:
    static bool is_reserved(Web::KeyEvent const& key_event)
    {
        if (key_event.type != Web::KeyEvent::Type::KeyDown)
            return false;

        for (auto const& shortcut : s_reserved_shortcuts) {
            if (matches_shortcut(key_event, shortcut.modifiers, shortcut.key))
                return true;
        }

        return false;
    }

private:
    using KeyCode = Web::UIEvents::KeyCode;
    using KeyModifier = Web::UIEvents::KeyModifier;

    struct KeyEventCombination {
        KeyModifier modifiers;
        KeyCode key;
    };

    static constexpr Array<KeyEventCombination, 16> s_reserved_shortcuts {
        // Open New Tab
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_T },

        // Close Current Tab
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_W },

        // New Window
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_N },

        // Cycle Tabs
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_Tab },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl | KeyModifier::Mod_Shift, KeyCode::Key_Tab },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_PageDown },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_PageUp },

        // Jump to Tab
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_1 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_2 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_3 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_4 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_5 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_6 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_7 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_8 },
        KeyEventCombination { KeyModifier::Mod_PlatformCtrl, KeyCode::Key_9 },
    };

    static bool matches_shortcut(Web::KeyEvent const& key_event, Web::UIEvents::KeyModifier modifiers, Web::UIEvents::KeyCode key)
    {
        return key_event.modifiers == modifiers && key_event.key == key;
    }
};

inline bool is_reserved_browser_shortcut(Web::KeyEvent const& key_event)
{
    return ReservedBrowserShortcuts::is_reserved(key_event);
}

inline bool should_redispatch_input_event(Web::InputEvent const& event, Web::EventResult event_result)
{
    if (event_result == Web::EventResult::Handled || event_result == Web::EventResult::Cancelled) {
        bool reserved_shortcut = false;
        event.visit(
            [&](Web::KeyEvent const& key_event) {
                reserved_shortcut = is_reserved_browser_shortcut(key_event);
            },
            [](auto const&) {});

        return reserved_shortcut;
    }

    return true;
}

}
