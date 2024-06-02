/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Utf8View.h>

#import <System/Carbon.h>
#import <UI/Event.h>
#import <Utilities/Conversions.h>

namespace Ladybird {

static KeyModifier ns_modifiers_to_key_modifiers(NSEventModifierFlags modifier_flags, Optional<Web::UIEvents::MouseButton&> button = {})
{
    unsigned modifiers = KeyModifier::Mod_None;

    if ((modifier_flags & NSEventModifierFlagShift) != 0) {
        modifiers |= KeyModifier::Mod_Shift;
    }
    if ((modifier_flags & NSEventModifierFlagControl) != 0) {
        if (button == Web::UIEvents::MouseButton::Primary) {
            *button = Web::UIEvents::MouseButton::Secondary;
        } else {
            modifiers |= KeyModifier::Mod_Ctrl;
        }
    }
    if ((modifier_flags & NSEventModifierFlagOption) != 0) {
        modifiers |= KeyModifier::Mod_Alt;
    }
    if ((modifier_flags & NSEventModifierFlagCommand) != 0) {
        modifiers |= KeyModifier::Mod_Super;
    }

    return static_cast<KeyModifier>(modifiers);
}

Web::MouseEvent ns_event_to_mouse_event(Web::MouseEvent::Type type, NSEvent* event, NSView* view, NSScrollView* scroll_view, Web::UIEvents::MouseButton button)
{
    auto position = [view convertPoint:event.locationInWindow fromView:nil];
    auto device_position = ns_point_to_gfx_point(position).to_type<Web::DevicePixels>();

    auto screen_position = [NSEvent mouseLocation];
    auto device_screen_position = ns_point_to_gfx_point(screen_position).to_type<Web::DevicePixels>();

    auto modifiers = ns_modifiers_to_key_modifiers(event.modifierFlags, button);

    int wheel_delta_x = 0;
    int wheel_delta_y = 0;

    if (type == Web::MouseEvent::Type::MouseDown) {
        if (event.clickCount % 2 == 0) {
            type = Web::MouseEvent::Type::DoubleClick;
        }
    } else if (type == Web::MouseEvent::Type::MouseWheel) {
        CGFloat delta_x = -[event scrollingDeltaX];
        CGFloat delta_y = -[event scrollingDeltaY];

        if (![event hasPreciseScrollingDeltas]) {
            delta_x *= scroll_view.horizontalLineScroll;
            delta_y *= scroll_view.verticalLineScroll;
        }

        wheel_delta_x = static_cast<int>(delta_x);
        wheel_delta_y = static_cast<int>(delta_y);
    }

    return { type, device_position, device_screen_position, button, button, modifiers, wheel_delta_x, wheel_delta_y, nullptr };
}

NSEvent* create_context_menu_mouse_event(NSView* view, Gfx::IntPoint position)
{
    return create_context_menu_mouse_event(view, gfx_point_to_ns_point(position));
}

NSEvent* create_context_menu_mouse_event(NSView* view, NSPoint position)
{
    return [NSEvent mouseEventWithType:NSEventTypeRightMouseUp
                              location:[view convertPoint:position fromView:nil]
                         modifierFlags:0
                             timestamp:0
                          windowNumber:[[view window] windowNumber]
                               context:nil
                           eventNumber:1
                            clickCount:1
                              pressure:1.0];
}

static KeyCode ns_key_code_to_key_code(unsigned short key_code, KeyModifier& modifiers)
{
    auto augment_modifiers_and_return = [&](auto key, auto modifier) {
        modifiers = static_cast<KeyModifier>(static_cast<unsigned>(modifiers) | modifier);
        return key;
    };

    // clang-format off
    switch (key_code) {
    case kVK_ANSI_0: return KeyCode::Key_0;
    case kVK_ANSI_1: return KeyCode::Key_1;
    case kVK_ANSI_2: return KeyCode::Key_2;
    case kVK_ANSI_3: return KeyCode::Key_3;
    case kVK_ANSI_4: return KeyCode::Key_4;
    case kVK_ANSI_5: return KeyCode::Key_5;
    case kVK_ANSI_6: return KeyCode::Key_6;
    case kVK_ANSI_7: return KeyCode::Key_7;
    case kVK_ANSI_8: return KeyCode::Key_8;
    case kVK_ANSI_9: return KeyCode::Key_9;
    case kVK_ANSI_A: return KeyCode::Key_A;
    case kVK_ANSI_B: return KeyCode::Key_B;
    case kVK_ANSI_C: return KeyCode::Key_C;
    case kVK_ANSI_D: return KeyCode::Key_D;
    case kVK_ANSI_E: return KeyCode::Key_E;
    case kVK_ANSI_F: return KeyCode::Key_F;
    case kVK_ANSI_G: return KeyCode::Key_G;
    case kVK_ANSI_H: return KeyCode::Key_H;
    case kVK_ANSI_I: return KeyCode::Key_I;
    case kVK_ANSI_J: return KeyCode::Key_J;
    case kVK_ANSI_K: return KeyCode::Key_K;
    case kVK_ANSI_L: return KeyCode::Key_L;
    case kVK_ANSI_M: return KeyCode::Key_M;
    case kVK_ANSI_N: return KeyCode::Key_N;
    case kVK_ANSI_O: return KeyCode::Key_O;
    case kVK_ANSI_P: return KeyCode::Key_P;
    case kVK_ANSI_Q: return KeyCode::Key_Q;
    case kVK_ANSI_R: return KeyCode::Key_R;
    case kVK_ANSI_S: return KeyCode::Key_S;
    case kVK_ANSI_T: return KeyCode::Key_T;
    case kVK_ANSI_U: return KeyCode::Key_U;
    case kVK_ANSI_V: return KeyCode::Key_V;
    case kVK_ANSI_W: return KeyCode::Key_W;
    case kVK_ANSI_X: return KeyCode::Key_X;
    case kVK_ANSI_Y: return KeyCode::Key_Y;
    case kVK_ANSI_Z: return KeyCode::Key_Z;
    case kVK_ANSI_Backslash: return KeyCode::Key_Backslash;
    case kVK_ANSI_Comma: return KeyCode::Key_Comma;
    case kVK_ANSI_Equal: return KeyCode::Key_Equal;
    case kVK_ANSI_Grave: return KeyCode::Key_Backtick;
    case kVK_ANSI_Keypad0: return augment_modifiers_and_return(KeyCode::Key_0, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad1: return augment_modifiers_and_return(KeyCode::Key_1, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad2: return augment_modifiers_and_return(KeyCode::Key_2, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad3: return augment_modifiers_and_return(KeyCode::Key_3, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad4: return augment_modifiers_and_return(KeyCode::Key_4, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad5: return augment_modifiers_and_return(KeyCode::Key_5, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad6: return augment_modifiers_and_return(KeyCode::Key_6, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad7: return augment_modifiers_and_return(KeyCode::Key_7, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad8: return augment_modifiers_and_return(KeyCode::Key_8, KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad9: return augment_modifiers_and_return(KeyCode::Key_9, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadClear: return augment_modifiers_and_return(KeyCode::Key_Delete, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadDecimal: return augment_modifiers_and_return(KeyCode::Key_Period, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadDivide: return augment_modifiers_and_return(KeyCode::Key_Slash, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadEnter: return augment_modifiers_and_return(KeyCode::Key_Return, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadEquals: return augment_modifiers_and_return(KeyCode::Key_Equal, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadMinus: return augment_modifiers_and_return(KeyCode::Key_Minus, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadMultiply: return augment_modifiers_and_return(KeyCode::Key_Asterisk, KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadPlus: return augment_modifiers_and_return(KeyCode::Key_Plus, KeyModifier::Mod_Keypad);
    case kVK_ANSI_LeftBracket: return KeyCode::Key_LeftBracket;
    case kVK_ANSI_Minus: return KeyCode::Key_Minus;
    case kVK_ANSI_Period: return KeyCode::Key_Period;
    case kVK_ANSI_Quote: return KeyCode::Key_Apostrophe;
    case kVK_ANSI_RightBracket: return KeyCode::Key_RightBracket;
    case kVK_ANSI_Semicolon: return KeyCode::Key_Semicolon;
    case kVK_ANSI_Slash: return KeyCode::Key_Slash;
    case kVK_CapsLock: return KeyCode::Key_CapsLock;
    case kVK_Command: return KeyCode::Key_Super;
    case kVK_Control: return KeyCode::Key_Control;
    case kVK_Delete: return KeyCode::Key_Backspace;
    case kVK_DownArrow: return KeyCode::Key_Down;
    case kVK_End: return KeyCode::Key_End;
    case kVK_Escape: return KeyCode::Key_Escape;
    case kVK_F1: return KeyCode::Key_F1;
    case kVK_F2: return KeyCode::Key_F2;
    case kVK_F3: return KeyCode::Key_F3;
    case kVK_F4: return KeyCode::Key_F4;
    case kVK_F5: return KeyCode::Key_F5;
    case kVK_F6: return KeyCode::Key_F6;
    case kVK_F7: return KeyCode::Key_F7;
    case kVK_F8: return KeyCode::Key_F8;
    case kVK_F9: return KeyCode::Key_F9;
    case kVK_F10: return KeyCode::Key_F10;
    case kVK_F11: return KeyCode::Key_F11;
    case kVK_F12: return KeyCode::Key_F12;
    case kVK_ForwardDelete: return KeyCode::Key_Delete;
    case kVK_Home: return KeyCode::Key_Home;
    case kVK_LeftArrow: return KeyCode::Key_Left;
    case kVK_Option: return KeyCode::Key_Alt;
    case kVK_PageDown: return KeyCode::Key_PageDown;
    case kVK_PageUp: return KeyCode::Key_PageUp;
    case kVK_Return: return KeyCode::Key_Return;
    case kVK_RightArrow: return KeyCode::Key_Right;
    case kVK_RightCommand: return KeyCode::Key_Super; // FIXME: We do not distinguish left-vs-right.
    case kVK_RightControl: return KeyCode::Key_Control; // FIXME: We do not distinguish left-vs-right.
    case kVK_RightOption: return KeyCode::Key_Alt; // FIXME: We do not distinguish left-vs-right.
    case kVK_RightShift: return KeyCode::Key_RightShift;
    case kVK_Shift: return KeyCode::Key_Shift;
    case kVK_Space: return KeyCode::Key_Space;
    case kVK_Tab: return KeyCode::Key_Tab;
    case kVK_UpArrow: return KeyCode::Key_Up;
    default: break;
    }
    // clang-format on

    return KeyCode::Key_Invalid;
}

class KeyData : public Web::ChromeInputData {
public:
    explicit KeyData(NSEvent* event)
        : m_event(CFBridgingRetain(event))
    {
    }

    virtual ~KeyData() override
    {
        if (m_event != nullptr) {
            CFBridgingRelease(m_event);
        }
    }

    NSEvent* take_event()
    {
        VERIFY(m_event != nullptr);

        CFTypeRef event = exchange(m_event, nullptr);
        return CFBridgingRelease(event);
    }

private:
    CFTypeRef m_event { nullptr };
};

Web::KeyEvent ns_event_to_key_event(Web::KeyEvent::Type type, NSEvent* event)
{
    auto modifiers = ns_modifiers_to_key_modifiers(event.modifierFlags);
    auto key_code = ns_key_code_to_key_code(event.keyCode, modifiers);

    auto const* utf8 = [event.characters UTF8String];
    Utf8View utf8_view { StringView { utf8, strlen(utf8) } };

    // FIXME: WebContent should really support multi-code point key events.
    auto code_point = utf8_view.is_empty() ? 0u : *utf8_view.begin();

    return { type, key_code, modifiers, code_point, make<KeyData>(event) };
}

NSEvent* key_event_to_ns_event(Web::KeyEvent const& event)
{
    auto& chrome_data = verify_cast<KeyData>(*event.chrome_data);
    return chrome_data.take_event();
}

}
