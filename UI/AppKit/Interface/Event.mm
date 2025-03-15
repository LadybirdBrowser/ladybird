/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Utf8View.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/UIEvents/KeyCode.h>

#import <Carbon/Carbon.h>
#import <Interface/Event.h>
#import <Utilities/Conversions.h>

namespace Ladybird {

static Web::UIEvents::KeyModifier ns_modifiers_to_key_modifiers(NSEventModifierFlags modifier_flags, Optional<Web::UIEvents::MouseButton&> button = {})
{
    unsigned modifiers = Web::UIEvents::KeyModifier::Mod_None;

    if ((modifier_flags & NSEventModifierFlagShift) != 0) {
        modifiers |= Web::UIEvents::KeyModifier::Mod_Shift;
    }
    if ((modifier_flags & NSEventModifierFlagControl) != 0) {
        if (button == Web::UIEvents::MouseButton::Primary) {
            *button = Web::UIEvents::MouseButton::Secondary;
        } else {
            modifiers |= Web::UIEvents::KeyModifier::Mod_Ctrl;
        }
    }
    if ((modifier_flags & NSEventModifierFlagOption) != 0) {
        modifiers |= Web::UIEvents::KeyModifier::Mod_Alt;
    }
    if ((modifier_flags & NSEventModifierFlagCommand) != 0) {
        modifiers |= Web::UIEvents::KeyModifier::Mod_Super;
    }

    return static_cast<Web::UIEvents::KeyModifier>(modifiers);
}

Web::MouseEvent ns_event_to_mouse_event(Web::MouseEvent::Type type, NSEvent* event, NSView* view, Web::UIEvents::MouseButton button)
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
            static constexpr CGFloat imprecise_scroll_multiplier = 24;

            delta_x *= imprecise_scroll_multiplier;
            delta_y *= imprecise_scroll_multiplier;
        }

        wheel_delta_x = static_cast<int>(delta_x);
        wheel_delta_y = static_cast<int>(delta_y);
    }

    return { type, device_position, device_screen_position, button, button, modifiers, wheel_delta_x, wheel_delta_y, nullptr };
}

struct DragData : public Web::BrowserInputData {
    explicit DragData(Vector<URL::URL> urls)
        : urls(move(urls))
    {
    }

    Vector<URL::URL> urls;
};

Web::DragEvent ns_event_to_drag_event(Web::DragEvent::Type type, id<NSDraggingInfo> event, NSView* view)
{
    auto position = [view convertPoint:event.draggingLocation fromView:nil];
    auto device_position = ns_point_to_gfx_point(position).to_type<Web::DevicePixels>();

    auto screen_position = [NSEvent mouseLocation];
    auto device_screen_position = ns_point_to_gfx_point(screen_position).to_type<Web::DevicePixels>();

    auto button = Web::UIEvents::MouseButton::Primary;
    auto modifiers = ns_modifiers_to_key_modifiers([NSEvent modifierFlags], button);

    Vector<Web::HTML::SelectedFile> files;
    OwnPtr<DragData> browser_data;

    auto for_each_file = [&](auto callback) {
        NSArray* file_list = [[event draggingPasteboard] readObjectsForClasses:@[ [NSURL class] ]
                                                                       options:nil];

        for (NSURL* file in file_list) {
            auto file_path = Ladybird::ns_string_to_byte_string([file path]);
            callback(file_path);
        }
    };

    if (type == Web::DragEvent::Type::DragStart) {
        for_each_file([&](ByteString const& file_path) {
            if (auto file = Web::HTML::SelectedFile::from_file_path(file_path); file.is_error())
                warnln("Unable to open file {}: {}", file_path, file.error());
            else
                files.append(file.release_value());
        });
    } else if (type == Web::DragEvent::Type::Drop) {
        Vector<URL::URL> urls;

        for_each_file([&](ByteString const& file_path) {
            if (auto url = URL::create_with_url_or_path(file_path); url.is_valid())
                urls.append(move(url));
        });

        browser_data = make<DragData>(move(urls));
    }

    return { type, device_position, device_screen_position, button, button, modifiers, move(files), move(browser_data) };
}

Vector<URL::URL> drag_event_url_list(Web::DragEvent const& event)
{
    auto& browser_data = as<DragData>(*event.browser_data);
    return move(browser_data.urls);
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

static Web::UIEvents::KeyCode ns_key_code_to_key_code(unsigned short key_code, Web::UIEvents::KeyModifier& modifiers)
{
    auto augment_modifiers_and_return = [&](auto key, auto modifier) {
        modifiers = static_cast<Web::UIEvents::KeyModifier>(static_cast<unsigned>(modifiers) | modifier);
        return key;
    };

    // clang-format off
    switch (key_code) {
    case kVK_ANSI_0: return Web::UIEvents::KeyCode::Key_0;
    case kVK_ANSI_1: return Web::UIEvents::KeyCode::Key_1;
    case kVK_ANSI_2: return Web::UIEvents::KeyCode::Key_2;
    case kVK_ANSI_3: return Web::UIEvents::KeyCode::Key_3;
    case kVK_ANSI_4: return Web::UIEvents::KeyCode::Key_4;
    case kVK_ANSI_5: return Web::UIEvents::KeyCode::Key_5;
    case kVK_ANSI_6: return Web::UIEvents::KeyCode::Key_6;
    case kVK_ANSI_7: return Web::UIEvents::KeyCode::Key_7;
    case kVK_ANSI_8: return Web::UIEvents::KeyCode::Key_8;
    case kVK_ANSI_9: return Web::UIEvents::KeyCode::Key_9;
    case kVK_ANSI_A: return Web::UIEvents::KeyCode::Key_A;
    case kVK_ANSI_B: return Web::UIEvents::KeyCode::Key_B;
    case kVK_ANSI_C: return Web::UIEvents::KeyCode::Key_C;
    case kVK_ANSI_D: return Web::UIEvents::KeyCode::Key_D;
    case kVK_ANSI_E: return Web::UIEvents::KeyCode::Key_E;
    case kVK_ANSI_F: return Web::UIEvents::KeyCode::Key_F;
    case kVK_ANSI_G: return Web::UIEvents::KeyCode::Key_G;
    case kVK_ANSI_H: return Web::UIEvents::KeyCode::Key_H;
    case kVK_ANSI_I: return Web::UIEvents::KeyCode::Key_I;
    case kVK_ANSI_J: return Web::UIEvents::KeyCode::Key_J;
    case kVK_ANSI_K: return Web::UIEvents::KeyCode::Key_K;
    case kVK_ANSI_L: return Web::UIEvents::KeyCode::Key_L;
    case kVK_ANSI_M: return Web::UIEvents::KeyCode::Key_M;
    case kVK_ANSI_N: return Web::UIEvents::KeyCode::Key_N;
    case kVK_ANSI_O: return Web::UIEvents::KeyCode::Key_O;
    case kVK_ANSI_P: return Web::UIEvents::KeyCode::Key_P;
    case kVK_ANSI_Q: return Web::UIEvents::KeyCode::Key_Q;
    case kVK_ANSI_R: return Web::UIEvents::KeyCode::Key_R;
    case kVK_ANSI_S: return Web::UIEvents::KeyCode::Key_S;
    case kVK_ANSI_T: return Web::UIEvents::KeyCode::Key_T;
    case kVK_ANSI_U: return Web::UIEvents::KeyCode::Key_U;
    case kVK_ANSI_V: return Web::UIEvents::KeyCode::Key_V;
    case kVK_ANSI_W: return Web::UIEvents::KeyCode::Key_W;
    case kVK_ANSI_X: return Web::UIEvents::KeyCode::Key_X;
    case kVK_ANSI_Y: return Web::UIEvents::KeyCode::Key_Y;
    case kVK_ANSI_Z: return Web::UIEvents::KeyCode::Key_Z;
    case kVK_ANSI_Backslash: return Web::UIEvents::KeyCode::Key_Backslash;
    case kVK_ANSI_Comma: return Web::UIEvents::KeyCode::Key_Comma;
    case kVK_ANSI_Equal: return Web::UIEvents::KeyCode::Key_Equal;
    case kVK_ANSI_Grave: return Web::UIEvents::KeyCode::Key_Backtick;
    case kVK_ANSI_Keypad0: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_0, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad1: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_1, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad2: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_2, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad3: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_3, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad4: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_4, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad5: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_5, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad6: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_6, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad7: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_7, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad8: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_8, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad9: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_9, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadClear: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Delete, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadDecimal: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Period, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadDivide: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Slash, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadEnter: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Return, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadEquals: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Equal, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadMinus: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Minus, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadMultiply: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Asterisk, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadPlus: return augment_modifiers_and_return(Web::UIEvents::KeyCode::Key_Plus, Web::UIEvents::KeyModifier::Mod_Keypad);
    case kVK_ANSI_LeftBracket: return Web::UIEvents::KeyCode::Key_LeftBracket;
    case kVK_ANSI_Minus: return Web::UIEvents::KeyCode::Key_Minus;
    case kVK_ANSI_Period: return Web::UIEvents::KeyCode::Key_Period;
    case kVK_ANSI_Quote: return Web::UIEvents::KeyCode::Key_Apostrophe;
    case kVK_ANSI_RightBracket: return Web::UIEvents::KeyCode::Key_RightBracket;
    case kVK_ANSI_Semicolon: return Web::UIEvents::KeyCode::Key_Semicolon;
    case kVK_ANSI_Slash: return Web::UIEvents::KeyCode::Key_Slash;
    case kVK_CapsLock: return Web::UIEvents::KeyCode::Key_CapsLock;
    case kVK_Command: return Web::UIEvents::KeyCode::Key_LeftSuper;
    case kVK_Control: return Web::UIEvents::KeyCode::Key_LeftControl;
    case kVK_Delete: return Web::UIEvents::KeyCode::Key_Backspace;
    case kVK_DownArrow: return Web::UIEvents::KeyCode::Key_Down;
    case kVK_End: return Web::UIEvents::KeyCode::Key_End;
    case kVK_Escape: return Web::UIEvents::KeyCode::Key_Escape;
    case kVK_F1: return Web::UIEvents::KeyCode::Key_F1;
    case kVK_F2: return Web::UIEvents::KeyCode::Key_F2;
    case kVK_F3: return Web::UIEvents::KeyCode::Key_F3;
    case kVK_F4: return Web::UIEvents::KeyCode::Key_F4;
    case kVK_F5: return Web::UIEvents::KeyCode::Key_F5;
    case kVK_F6: return Web::UIEvents::KeyCode::Key_F6;
    case kVK_F7: return Web::UIEvents::KeyCode::Key_F7;
    case kVK_F8: return Web::UIEvents::KeyCode::Key_F8;
    case kVK_F9: return Web::UIEvents::KeyCode::Key_F9;
    case kVK_F10: return Web::UIEvents::KeyCode::Key_F10;
    case kVK_F11: return Web::UIEvents::KeyCode::Key_F11;
    case kVK_F12: return Web::UIEvents::KeyCode::Key_F12;
    case kVK_ForwardDelete: return Web::UIEvents::KeyCode::Key_Delete;
    case kVK_Home: return Web::UIEvents::KeyCode::Key_Home;
    case kVK_LeftArrow: return Web::UIEvents::KeyCode::Key_Left;
    case kVK_Option: return Web::UIEvents::KeyCode::Key_LeftAlt;
    case kVK_PageDown: return Web::UIEvents::KeyCode::Key_PageDown;
    case kVK_PageUp: return Web::UIEvents::KeyCode::Key_PageUp;
    case kVK_Return: return Web::UIEvents::KeyCode::Key_Return;
    case kVK_RightArrow: return Web::UIEvents::KeyCode::Key_Right;
    case kVK_RightCommand: return Web::UIEvents::KeyCode::Key_RightSuper;
    case kVK_RightControl: return Web::UIEvents::KeyCode::Key_RightControl;
    case kVK_RightOption: return Web::UIEvents::KeyCode::Key_RightAlt;
    case kVK_RightShift: return Web::UIEvents::KeyCode::Key_RightShift;
    case kVK_Shift: return Web::UIEvents::KeyCode::Key_LeftShift;
    case kVK_Space: return Web::UIEvents::KeyCode::Key_Space;
    case kVK_Tab: return Web::UIEvents::KeyCode::Key_Tab;
    case kVK_UpArrow: return Web::UIEvents::KeyCode::Key_Up;
    default: break;
    }
    // clang-format on

    return Web::UIEvents::KeyCode::Key_Invalid;
}

class KeyData : public Web::BrowserInputData {
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
    auto repeat = false;

    // FIXME: WebContent should really support multi-code point key events.
    u32 code_point = 0;

    if (event.type == NSEventTypeKeyDown || event.type == NSEventTypeKeyUp) {
        auto const* utf8 = [event.characters UTF8String];
        Utf8View utf8_view { StringView { utf8, strlen(utf8) } };

        code_point = utf8_view.is_empty() ? 0u : *utf8_view.begin();

        repeat = event.isARepeat;
    }

    // NSEvent assigns PUA code points to to functional keys, e.g. arrow keys. Do not propagate them.
    if (code_point >= 0xE000 && code_point <= 0xF8FF)
        code_point = 0;

    return { type, key_code, modifiers, code_point, repeat, make<KeyData>(event) };
}

NSEvent* key_event_to_ns_event(Web::KeyEvent const& event)
{
    auto& browser_data = as<KeyData>(*event.browser_data);
    return browser_data.take_event();
}

}
