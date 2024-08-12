/*
 * Copyright (c) 2021-2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/KeyboardEventPrototype.h>
#include <LibWeb/UIEvents/KeyboardEvent.h>

namespace Web::UIEvents {

JS_DEFINE_ALLOCATOR(KeyboardEvent);

// https://www.w3.org/TR/uievents/#determine-keydown-keyup-keyCode
static unsigned long determine_key_code(KeyCode platform_key, u32 code_point, unsigned modifiers)
{
    // If input key when pressed without modifiers would insert a numerical character (0-9), return the ASCII code of that numerical character.
    if (is_ascii_digit(code_point))
        return code_point;

    // Handle keys pressed with Ctrl
    if (modifiers & Mod_Ctrl) {
        if (code_point >= 1 && code_point <= 26) {
            return code_point + 64; // Convert to corresponding uppercase letter
        }
    }

    switch (platform_key) {
    case KeyCode::Key_ExclamationPoint:
        return static_cast<unsigned long>('1');
    case KeyCode::Key_AtSign:
        return static_cast<unsigned long>('2');
    case KeyCode::Key_Hashtag:
        return static_cast<unsigned long>('3');
    case KeyCode::Key_Dollar:
        return static_cast<unsigned long>('4');
    case KeyCode::Key_Percent:
        return static_cast<unsigned long>('5');
    case KeyCode::Key_Circumflex:
        return static_cast<unsigned long>('6');
    case KeyCode::Key_Ampersand:
        return static_cast<unsigned long>('7');
    case KeyCode::Key_Asterisk:
        return static_cast<unsigned long>('8');
    case KeyCode::Key_LeftParen:
        return static_cast<unsigned long>('9');
    case KeyCode::Key_RightParen:
        return static_cast<unsigned long>('0');
    default:
        break;
    }

    // If input key when pressed without modifiers would insert a lower case character in the a-z alphabetical range, return the ASCII code of the upper case equivalent.
    if (is_ascii_lower_alpha(code_point))
        return to_ascii_uppercase(code_point);

    // If the key’s function, as determined in an implementation-specific way, corresponds to one of the keys in the §8.3.3 Fixed virtual key codes table, return the corresponding key code.
    // https://www.w3.org/TR/uievents/#fixed-virtual-key-codes
    switch (platform_key) {
    case KeyCode::Key_Backspace:
        return 8;
    case KeyCode::Key_Tab:
        return 9;
    case KeyCode::Key_Return:
        return 13;
    case KeyCode::Key_Shift:
        return 16;
    case KeyCode::Key_Control:
        return 17;
    case KeyCode::Key_Alt:
        return 18;
    case KeyCode::Key_CapsLock:
        return 20;
    case KeyCode::Key_Escape:
        return 27;
    case KeyCode::Key_Space:
        return 32;
    case KeyCode::Key_PageUp:
        return 33;
    case KeyCode::Key_PageDown:
        return 34;
    case KeyCode::Key_End:
        return 35;
    case KeyCode::Key_Home:
        return 36;
    case KeyCode::Key_Left:
        return 37;
    case KeyCode::Key_Up:
        return 38;
    case KeyCode::Key_Right:
        return 39;
    case KeyCode::Key_Down:
        return 40;
    default:
        break;
    }

    // https://www.w3.org/TR/uievents/#optionally-fixed-virtual-key-codes
    switch (platform_key) {
    case KeyCode::Key_Semicolon:
    case KeyCode::Key_Colon:
        return 186;
    case KeyCode::Key_Equal:
    case KeyCode::Key_Plus:
        return 187;
    case KeyCode::Key_Comma:
    case KeyCode::Key_LessThan:
        return 188;
    case KeyCode::Key_Minus:
    case KeyCode::Key_Underscore:
        return 189;
    case KeyCode::Key_Period:
    case KeyCode::Key_GreaterThan:
        return 190;
    case KeyCode::Key_Slash:
    case KeyCode::Key_QuestionMark:
        return 191;
    case KeyCode::Key_Backtick:
    case KeyCode::Key_Tilde:
        return 192;
    case KeyCode::Key_LeftBracket:
    case KeyCode::Key_LeftBrace:
        return 219;
    case KeyCode::Key_Backslash:
    case KeyCode::Key_Pipe:
        return 220;
    case KeyCode::Key_RightBracket:
    case KeyCode::Key_RightBrace:
        return 221;
    case KeyCode::Key_Apostrophe:
    case KeyCode::Key_DoubleQuote:
        return 222;
    default:
        break;
    }

    // Return the virtual key code from the operating system.
    return platform_key;
}

// 3. Named key Attribute Values, https://www.w3.org/TR/uievents-key/#named-key-attribute-values
static ErrorOr<Optional<String>> get_event_named_key(KeyCode platform_key, unsigned modifiers)
{
    // Handle keys pressed with Ctrl
    if (modifiers & Mod_Ctrl) {
        if (platform_key >= KeyCode::Key_A && platform_key <= KeyCode::Key_Z) {
            return String::from_code_point(platform_key + 0x40); // Convert to corresponding uppercase letter
        }
    }

    switch (platform_key) {
    // 3.1. Special Keys, https://www.w3.org/TR/uievents-key/#keys-special
    case KeyCode::Key_Invalid:
        return "Unidentified"_string;

    // 3.2. Modifier Keys, https://www.w3.org/TR/uievents-key/#keys-modifier
    case KeyCode::Key_Alt:
        return "AltLeft"_string;
    case KeyCode::Key_RightAlt:
        return "AltRight"_string;
    case KeyCode::Key_CapsLock:
        return "CapsLock"_string;
    case KeyCode::Key_Control:
        return "Control"_string;
    case KeyCode::Key_Super:
        return "Meta"_string;
    case KeyCode::Key_NumLock:
        return "NumLock"_string;
    case KeyCode::Key_ScrollLock:
        return "ScrollLock"_string;
    case KeyCode::Key_LeftShift:
    case KeyCode::Key_RightShift:
        return "Shift"_string;

    // 3.3. Whitespace Keys, https://www.w3.org/TR/uievents-key/#keys-whitespace
    case KeyCode::Key_Return:
        return "Enter"_string;
    case KeyCode::Key_Tab:
        return "Tab"_string;
    case KeyCode::Key_Space:
        return " "_string;

    // 3.4. Navigation Keys, https://www.w3.org/TR/uievents-key/#keys-navigation
    case KeyCode::Key_Down:
        return "ArrowDown"_string;
    case KeyCode::Key_Left:
        return "ArrowLeft"_string;
    case KeyCode::Key_Right:
        return "ArrowRight"_string;
    case KeyCode::Key_Up:
        return "ArrowUp"_string;
    case KeyCode::Key_End:
        return "End"_string;
    case KeyCode::Key_Home:
        return "Home"_string;
    case KeyCode::Key_PageDown:
        return "PageDown"_string;
    case KeyCode::Key_PageUp:
        return "PageUp"_string;

    // 3.5. Editing Keys, https://www.w3.org/TR/uievents-key/#keys-editing
    case KeyCode::Key_Backspace:
        return "Backspace"_string;
    case KeyCode::Key_Delete:
        return "Delete"_string;
    case KeyCode::Key_Insert:
        return "Insert"_string;

    // 3.6. UI Keys, https://www.w3.org/TR/uievents-key/#keys-ui
    case KeyCode::Key_Menu:
        return "ContextMenu"_string;
    case KeyCode::Key_Escape:
        return "Escape"_string;
    // FIXME: Help
    // FIXME: Pause

    // 3.7. Device Keys, https://www.w3.org/TR/uievents-key/#keys-device
    case KeyCode::Key_PrintScreen:
        return "PrintScreen"_string;

    // 3.9. General-Purpose Function Keys, https://www.w3.org/TR/uievents-key/#keys-function
    case KeyCode::Key_F1:
        return "F1"_string;
    case KeyCode::Key_F2:
        return "F2"_string;
    case KeyCode::Key_F3:
        return "F3"_string;
    case KeyCode::Key_F4:
        return "F4"_string;
    case KeyCode::Key_F5:
        return "F5"_string;
    case KeyCode::Key_F6:
        return "F6"_string;
    case KeyCode::Key_F7:
        return "F7"_string;
    case KeyCode::Key_F8:
        return "F8"_string;
    case KeyCode::Key_F9:
        return "F9"_string;
    case KeyCode::Key_F10:
        return "F10"_string;
    case KeyCode::Key_F11:
        return "F11"_string;
    case KeyCode::Key_F12:
        return "F12"_string;

    default:
        break;
    }

    return OptionalNone {};
}

// 2.1. Unicode Values, https://www.w3.org/TR/uievents-key/#keys-unicode
static ErrorOr<Optional<String>> get_event_key_string(u32 code_point)
{
    auto is_non_control_character = [&]() {
        // A non-control character is any valid Unicode character except those that are part of the "Other, Control"
        // ("Cc") General Category.
        return !Unicode::code_point_has_control_general_category(code_point);
    };

    // A key string is a string containing a 0 or 1 non-control characters ("base" characters) followed by 0 or more
    // combining characters. The string MUST be in Normalized Form C (NFC) as described in [UAX15].
    // FIXME: Our key events are currently set up to provide one code point at a time. We will need to handle multi-
    //        code point events and NFC normalize that string.
    if (is_non_control_character())
        return String::from_code_point(code_point);

    return OptionalNone {};
}

// 2.2. Selecting key Attribute Values, https://www.w3.org/TR/uievents-key/#selecting-key-attribute-values
static ErrorOr<String> get_event_key(KeyCode platform_key, u32 code_point, unsigned modifiers)
{
    // 1. Let key be a DOMString initially set to "Unidentified".
    // NOTE: We return "Unidentified" at the end to avoid needlessly allocating it here.
    Optional<String> key;

    // 2. If there exists an appropriate named key attribute value for this key event, then
    if (auto named_key = TRY(get_event_named_key(platform_key, modifiers)); named_key.has_value()) {
        // 1. Set key to that named key attribute value.
        key = named_key.release_value();
    }

    // 3. Else, if the key event generates a valid key string, then
    else if (auto key_string = TRY(get_event_key_string(code_point)); key_string.has_value()) {
        // 1. Set key to that key string value.
        key = key_string.release_value();
    }

    // FIXME: 4. Else, if the key event has any modifier keys other than glyph modifier keys, then
    // FIXME:     1. Set key to the key string that would have been generated by this event if it had been typed with all
    //               modifer keys removed except for glyph modifier keys.

    // 5. Return key as the key attribute value for this key event.
    if (key.has_value())
        return key.release_value();
    return "Unidentified"_string;
}

// 3. Keyboard Event code Value Tables, https://www.w3.org/TR/uievents-code/#code-value-tables
static ErrorOr<String> get_event_code(KeyCode platform_key, unsigned modifiers)
{
    // 3.4. Numpad Section, https://www.w3.org/TR/uievents-code/#key-numpad-section
    if ((modifiers & Mod_Keypad) != 0) {
        switch (platform_key) {
        case KeyCode::Key_0:
            return "Numpad0"_string;
        case KeyCode::Key_1:
            return "Numpad1"_string;
        case KeyCode::Key_2:
            return "Numpad2"_string;
        case KeyCode::Key_3:
            return "Numpad3"_string;
        case KeyCode::Key_4:
            return "Numpad4"_string;
        case KeyCode::Key_5:
            return "Numpad5"_string;
        case KeyCode::Key_6:
            return "Numpad6"_string;
        case KeyCode::Key_7:
            return "Numpad7"_string;
        case KeyCode::Key_8:
            return "Numpad8"_string;
        case KeyCode::Key_9:
            return "Numpad9"_string;
        case KeyCode::Key_Plus:
            return "NumpadAdd"_string;
        case KeyCode::Key_Period:
        case KeyCode::Key_Delete:
            return "NumpadDecimal"_string;
        case KeyCode::Key_Slash:
            return "NumpadDivide"_string;
        case KeyCode::Key_Return:
            return "NumpadEnter"_string;
        case KeyCode::Key_Asterisk:
            return "NumpadAsterisk"_string;
        case KeyCode::Key_Minus:
            return "NumpadSubtract"_string;
        default:
            break;
        }
    }

    switch (platform_key) {
    // 3.1.1. Writing System Keys, https://www.w3.org/TR/uievents-code/#key-alphanumeric-writing-system
    case KeyCode::Key_Backtick:
    case KeyCode::Key_Tilde:
        return "Backquote"_string;
    case KeyCode::Key_Backslash:
    case KeyCode::Key_Pipe:
        return "Backslash"_string;
    case KeyCode::Key_LeftBrace:
    case KeyCode::Key_LeftBracket:
        return "BracketLeft"_string;
    case KeyCode::Key_RightBrace:
    case KeyCode::Key_RightBracket:
        return "BracketRight"_string;
    case KeyCode::Key_Comma:
    case KeyCode::Key_LessThan:
        return "Comma"_string;
    case KeyCode::Key_0:
    case KeyCode::Key_RightParen:
        return "Digit0"_string;
    case KeyCode::Key_1:
    case KeyCode::Key_ExclamationPoint:
        return "Digit1"_string;
    case KeyCode::Key_2:
    case KeyCode::Key_AtSign:
        return "Digit2"_string;
    case KeyCode::Key_3:
    case KeyCode::Key_Hashtag:
        return "Digit3"_string;
    case KeyCode::Key_4:
    case KeyCode::Key_Dollar:
        return "Digit4"_string;
    case KeyCode::Key_5:
    case KeyCode::Key_Percent:
        return "Digit5"_string;
    case KeyCode::Key_6:
    case KeyCode::Key_Circumflex:
        return "Digit6"_string;
    case KeyCode::Key_7:
    case KeyCode::Key_Ampersand:
        return "Digit7"_string;
    case KeyCode::Key_8:
    case KeyCode::Key_Asterisk:
        return "Digit8"_string;
    case KeyCode::Key_9:
    case KeyCode::Key_LeftParen:
        return "Digit9"_string;
    case KeyCode::Key_Equal:
    case KeyCode::Key_Plus:
        return "Equal"_string;
    // FIXME: IntlBackslash
    // FIXME: IntlRo
    // FIXME: IntlYen
    case KeyCode::Key_A:
        return "KeyA"_string;
    case KeyCode::Key_B:
        return "KeyB"_string;
    case KeyCode::Key_C:
        return "KeyC"_string;
    case KeyCode::Key_D:
        return "KeyD"_string;
    case KeyCode::Key_E:
        return "KeyE"_string;
    case KeyCode::Key_F:
        return "KeyF"_string;
    case KeyCode::Key_G:
        return "KeyG"_string;
    case KeyCode::Key_H:
        return "KeyH"_string;
    case KeyCode::Key_I:
        return "KeyI"_string;
    case KeyCode::Key_J:
        return "KeyJ"_string;
    case KeyCode::Key_K:
        return "KeyK"_string;
    case KeyCode::Key_L:
        return "KeyL"_string;
    case KeyCode::Key_M:
        return "KeyM"_string;
    case KeyCode::Key_N:
        return "KeyN"_string;
    case KeyCode::Key_O:
        return "KeyO"_string;
    case KeyCode::Key_P:
        return "KeyP"_string;
    case KeyCode::Key_Q:
        return "KeyQ"_string;
    case KeyCode::Key_R:
        return "KeyR"_string;
    case KeyCode::Key_S:
        return "KeyS"_string;
    case KeyCode::Key_T:
        return "KeyT"_string;
    case KeyCode::Key_U:
        return "KeyU"_string;
    case KeyCode::Key_V:
        return "KeyV"_string;
    case KeyCode::Key_W:
        return "KeyW"_string;
    case KeyCode::Key_X:
        return "KeyX"_string;
    case KeyCode::Key_Y:
        return "KeyY"_string;
    case KeyCode::Key_Z:
        return "KeyZ"_string;
    case KeyCode::Key_Minus:
    case KeyCode::Key_Underscore:
        return "Minus"_string;
    case KeyCode::Key_Period:
    case KeyCode::Key_GreaterThan:
        return "Period"_string;
    case KeyCode::Key_Apostrophe:
    case KeyCode::Key_DoubleQuote:
        return "Quote"_string;
    case KeyCode::Key_Semicolon:
    case KeyCode::Key_Colon:
        return "Semicolon"_string;
    case KeyCode::Key_Slash:
    case KeyCode::Key_QuestionMark:
        return "Slash"_string;

    // 3.1.2. Functional Keys, https://www.w3.org/TR/uievents-code/#key-alphanumeric-functional
    case KeyCode::Key_Alt:
        return "AltLeft"_string;
    case KeyCode::Key_RightAlt:
        return "AltRight"_string;
    case KeyCode::Key_Backspace:
        return "Backspace"_string;
    case KeyCode::Key_CapsLock:
        return "CapsLock"_string;
    case KeyCode::Key_Menu:
        return "ContextMenu"_string;
    case KeyCode::Key_Control:
        return "ControlLeft"_string;
    case KeyCode::Key_RightControl:
        return "ControlRight"_string;
    case KeyCode::Key_Return:
        return "Enter"_string;
    case KeyCode::Key_Super:
        return "Meta"_string; // FIXME: Detect left vs. right key.
    case KeyCode::Key_LeftShift:
        return "ShiftLeft"_string;
    case KeyCode::Key_RightShift:
        return "ShiftRight"_string;
    case KeyCode::Key_Space:
        return "Space"_string;
    case KeyCode::Key_Tab:
        return "Tab"_string;

    // 3.2. Control Pad Section, https://www.w3.org/TR/uievents-code/#key-controlpad-section
    case KeyCode::Key_Delete:
        return "Delete"_string;
    case KeyCode::Key_End:
        return "End"_string;
    // FIXME: Help
    case KeyCode::Key_Home:
        return "Home"_string;
    case KeyCode::Key_Insert:
        return "Insert"_string;
    case KeyCode::Key_PageDown:
        return "PageDown"_string;
    case KeyCode::Key_PageUp:
        return "PageUp"_string;

    // 3.3. Arrow Pad Section, https://www.w3.org/TR/uievents-code/#key-arrowpad-section
    case KeyCode::Key_Down:
        return "ArrowDown"_string;
    case KeyCode::Key_Left:
        return "ArrowLeft"_string;
    case KeyCode::Key_Right:
        return "ArrowRight"_string;
    case KeyCode::Key_Up:
        return "ArrowUp"_string;

    // 3.4. Numpad Section, https://www.w3.org/TR/uievents-code/#key-numpad-section
    case KeyCode::Key_NumLock:
        return "NumLock"_string;

    // 3.5. Function Section, https://www.w3.org/TR/uievents-code/#key-function-section
    case KeyCode::Key_Escape:
        return "Escape"_string;
    case KeyCode::Key_F1:
        return "F1"_string;
    case KeyCode::Key_F2:
        return "F2"_string;
    case KeyCode::Key_F3:
        return "F3"_string;
    case KeyCode::Key_F4:
        return "F4"_string;
    case KeyCode::Key_F5:
        return "F5"_string;
    case KeyCode::Key_F6:
        return "F6"_string;
    case KeyCode::Key_F7:
        return "F7"_string;
    case KeyCode::Key_F8:
        return "F8"_string;
    case KeyCode::Key_F9:
        return "F9"_string;
    case KeyCode::Key_F10:
        return "F10"_string;
    case KeyCode::Key_F11:
        return "F11"_string;
    case KeyCode::Key_F12:
        return "F12"_string;
    case KeyCode::Key_PrintScreen:
    case KeyCode::Key_SysRq:
        return "PrintScreen"_string;
    case KeyCode::Key_ScrollLock:
        return "ScrollLock"_string;
    case KeyCode::Key_PauseBreak:
        return "Pause"_string;

    // 3.6. Media Section, https://www.w3.org/TR/uievents-code/#media-keys
    case KeyCode::Key_BrowserSearch:
        return "BrowserSearch"_string;
    case KeyCode::Key_BrowserFavorites:
        return "BrowserFavorites"_string;
    case KeyCode::Key_BrowserHome:
        return "BrowserHome"_string;
    case KeyCode::Key_PreviousTrack:
        return "PreviousTrack"_string;
    case KeyCode::Key_BrowserBack:
        return "BrowserBack"_string;
    case KeyCode::Key_BrowserForward:
        return "BrowserForward"_string;
    case KeyCode::Key_BrowserRefresh:
        return "BrowserRefresh"_string;
    case KeyCode::Key_BrowserStop:
        return "BrowserStop"_string;
    case KeyCode::Key_VolumeDown:
        return "AudioVolumeDown"_string;
    case KeyCode::Key_VolumeUp:
        return "AudioVolumeUp"_string;
    case KeyCode::Key_Wake:
        return "WakeUp"_string;
    case KeyCode::Key_Sleep:
        return "Sleep"_string;
    case KeyCode::Key_NextTrack:
        return "NextTrack"_string;
    case KeyCode::Key_MediaSelect:
        return "MediaSelect"_string;
    case KeyCode::Key_Email:
        return "LaunchMail"_string;

    case KeyCode::Key_Power:
        return "Power"_string;
    case KeyCode::Key_Stop:
        return "MediaStop"_string;
    case KeyCode::Key_PlayPause:
        return "MediaPlayPause"_string;
    case KeyCode::Key_Mute:
        return "AudioVolumeMute"_string;
    case KeyCode::Key_Calculator:
        return "LaunchApp2"_string;
    case KeyCode::Key_MyComputer:
        return "LaunchApp1"_string;

    // FIXME: Are these correct?
    case KeyCode::Key_LeftGUI:
        return "LaunchApp2"_string;
    case KeyCode::Key_RightGUI:
    case KeyCode::Key_Apps:
        return "LaunchApp1"_string;

    // 3.7. Legacy, Non-Standard and Special Keys, https://www.w3.org/TR/uievents-code/#key-legacy
    case KeyCode::Key_Invalid:
        return "Unidentified"_string;
    }

    VERIFY_NOT_REACHED();
}

// 5.6.2. Keyboard Event Key Location, https://www.w3.org/TR/uievents/#events-keyboard-key-location
static DOMKeyLocation get_event_location(KeyCode platform_key, unsigned modifiers)
{
    if ((modifiers & Mod_Keypad) != 0)
        return DOMKeyLocation::Numpad;

    // FIXME: Detect left vs. right for Control and Alt keys.
    switch (platform_key) {
    case KeyCode::Key_LeftShift:
        return DOMKeyLocation::Left;
    case KeyCode::Key_RightShift:
        return DOMKeyLocation::Right;
    default:
        break;
    }

    return DOMKeyLocation::Standard;
}

JS::NonnullGCPtr<KeyboardEvent> KeyboardEvent::create_from_platform_event(JS::Realm& realm, FlyString const& event_name, KeyCode platform_key, unsigned modifiers, u32 code_point)
{
    auto event_key = MUST(get_event_key(platform_key, code_point, modifiers));
    auto event_code = MUST(get_event_code(platform_key, modifiers));

    auto key_code = determine_key_code(platform_key, code_point, modifiers);
    KeyboardEventInit event_init {};
    event_init.key = move(event_key);
    event_init.code = move(event_code);
    event_init.location = to_underlying(get_event_location(platform_key, modifiers));
    event_init.ctrl_key = modifiers & Mod_Ctrl;
    event_init.shift_key = modifiers & Mod_Shift;
    event_init.alt_key = modifiers & Mod_Alt;
    event_init.meta_key = modifiers & Mod_Super;
    event_init.repeat = false;
    event_init.is_composing = false;
    event_init.key_code = key_code;
    event_init.char_code = code_point;
    event_init.bubbles = true;
    event_init.cancelable = true;
    event_init.composed = true;
    auto event = KeyboardEvent::create(realm, event_name, event_init);
    event->set_is_trusted(true);
    return event;
}

bool KeyboardEvent::get_modifier_state(String const& key_arg) const
{
    if (key_arg == "Control")
        return m_ctrl_key;
    if (key_arg == "Shift")
        return m_shift_key;
    if (key_arg == "Alt")
        return m_alt_key;
    if (key_arg == "Meta")
        return m_meta_key;
    if (key_arg == "AltGraph")
        return m_modifier_alt_graph;
    if (key_arg == "CapsLock")
        return m_modifier_caps_lock;
    if (key_arg == "Fn")
        return m_modifier_fn;
    if (key_arg == "FnLock")
        return m_modifier_fn_lock;
    if (key_arg == "Hyper")
        return m_modifier_hyper;
    if (key_arg == "NumLock")
        return m_modifier_num_lock;
    if (key_arg == "ScrollLock")
        return m_modifier_scroll_lock;
    if (key_arg == "Super")
        return m_modifier_super;
    if (key_arg == "Symbol")
        return m_modifier_symbol;
    if (key_arg == "SymbolLock")
        return m_modifier_symbol_lock;
    return false;
}

// https://w3c.github.io/uievents/#dom-keyboardevent-initkeyboardevent
void KeyboardEvent::init_keyboard_event(String const& type, bool bubbles, bool cancelable, HTML::Window* view, String const& key, WebIDL::UnsignedLong location, bool ctrl_key, bool alt_key, bool shift_key, bool meta_key)
{
    // Initializes attributes of a KeyboardEvent object. This method has the same behavior as UIEvent.initUIEvent().
    // The value of detail remains undefined.

    // 1. If this’s dispatch flag is set, then return.
    if (dispatched())
        return;

    // 2. Initialize this with type, bubbles, and cancelable.
    initialize_event(type, bubbles, cancelable);

    // Implementation Defined: Initialise other values.
    m_view = view;
    m_key = key;
    m_location = location;
    m_ctrl_key = ctrl_key;
    m_alt_key = alt_key;
    m_shift_key = shift_key;
    m_meta_key = meta_key;
}

JS::NonnullGCPtr<KeyboardEvent> KeyboardEvent::create(JS::Realm& realm, FlyString const& event_name, KeyboardEventInit const& event_init)
{
    return realm.heap().allocate<KeyboardEvent>(realm, realm, event_name, event_init);
}

WebIDL::ExceptionOr<JS::NonnullGCPtr<KeyboardEvent>> KeyboardEvent::construct_impl(JS::Realm& realm, FlyString const& event_name, KeyboardEventInit const& event_init)
{
    return create(realm, event_name, event_init);
}

KeyboardEvent::KeyboardEvent(JS::Realm& realm, FlyString const& event_name, KeyboardEventInit const& event_init)
    : UIEvent(realm, event_name, event_init)
    , m_key(event_init.key)
    , m_code(event_init.code)
    , m_location(event_init.location)
    , m_ctrl_key(event_init.ctrl_key)
    , m_shift_key(event_init.shift_key)
    , m_alt_key(event_init.alt_key)
    , m_meta_key(event_init.meta_key)
    , m_modifier_alt_graph(event_init.modifier_alt_graph)
    , m_modifier_caps_lock(event_init.modifier_caps_lock)
    , m_modifier_fn(event_init.modifier_fn)
    , m_modifier_fn_lock(event_init.modifier_fn_lock)
    , m_modifier_hyper(event_init.modifier_hyper)
    , m_modifier_num_lock(event_init.modifier_num_lock)
    , m_modifier_scroll_lock(event_init.modifier_scroll_lock)
    , m_modifier_super(event_init.modifier_super)
    , m_modifier_symbol(event_init.modifier_symbol)
    , m_modifier_symbol_lock(event_init.modifier_symbol_lock)
    , m_repeat(event_init.repeat)
    , m_is_composing(event_init.is_composing)
    , m_key_code(event_init.key_code)
    , m_char_code(event_init.char_code)
{
}

KeyboardEvent::~KeyboardEvent() = default;

void KeyboardEvent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(KeyboardEvent);
}

}
