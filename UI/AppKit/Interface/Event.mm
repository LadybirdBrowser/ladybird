/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <AK/Utf8View.h>
#include <LibCompositing/KeyCode.h>
#include <LibURL/URL.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWebView/Utilities.h>

#import <Carbon/Carbon.h>
#import <Interface/Event.h>
#import <Utilities/Conversions.h>

namespace Ladybird {

Compositing::KeyModifier ns_modifiers_to_key_modifiers(NSEventModifierFlags modifier_flags)
{
    unsigned modifiers = Compositing::KeyModifier::Mod_None;

    if ((modifier_flags & NSEventModifierFlagShift) != 0) {
        modifiers |= Compositing::KeyModifier::Mod_Shift;
    }
    if ((modifier_flags & NSEventModifierFlagControl) != 0) {
        modifiers |= Compositing::KeyModifier::Mod_Ctrl;
    }
    if ((modifier_flags & NSEventModifierFlagOption) != 0) {
        modifiers |= Compositing::KeyModifier::Mod_Alt;
    }
    if ((modifier_flags & NSEventModifierFlagCommand) != 0) {
        modifiers |= Compositing::KeyModifier::Mod_Super;
    }

    return static_cast<Compositing::KeyModifier>(modifiers);
}

static Compositing::ScrollGesturePhase ns_scroll_event_to_scroll_gesture_phase(NSEvent* event)
{
    // Fingers resting on the touchpad without moving continue the gesture they began.
    static constexpr NSEventPhase ongoing_phases = NSEventPhaseMayBegin | NSEventPhaseBegan | NSEventPhaseChanged | NSEventPhaseStationary;
    static constexpr NSEventPhase ending_phases = NSEventPhaseEnded | NSEventPhaseCancelled;

    if ((event.phase & ending_phases) != 0 || (event.momentumPhase & ending_phases) != 0)
        return Compositing::ScrollGesturePhase::Ended;
    if ((event.momentumPhase & ongoing_phases) != 0)
        return Compositing::ScrollGesturePhase::Momentum;
    if ((event.phase & ongoing_phases) != 0)
        return Compositing::ScrollGesturePhase::Ongoing;
    return Compositing::ScrollGesturePhase::None;
}

Compositing::MouseEvent ns_event_to_mouse_event(Compositing::MouseEvent::Type type, NSEvent* event, NSView* view, Compositing::MouseButton button)
{
    auto position = [view convertPoint:event.locationInWindow fromView:nil];
    auto device_position = ns_point_to_gfx_point(position).to_type<Compositing::DevicePixels>();

    auto screen_position = [NSEvent mouseLocation];
    auto device_screen_position = ns_point_to_gfx_point(screen_position).to_type<Compositing::DevicePixels>();

    auto modifiers = ns_modifiers_to_key_modifiers(event.modifierFlags);

    double wheel_delta_x = 0;
    double wheel_delta_y = 0;
    auto wheel_delta_precision = Compositing::WheelDeltaPrecision::Discrete;
    auto scroll_gesture_phase = Compositing::ScrollGesturePhase::None;

    if (type == Compositing::MouseEvent::Type::MouseWheel) {
        wheel_delta_x = -[event scrollingDeltaX];
        wheel_delta_y = -[event scrollingDeltaY];

        if ([event hasPreciseScrollingDeltas]) {
            wheel_delta_precision = Compositing::WheelDeltaPrecision::Precise;
        } else {
            static constexpr double imprecise_scroll_multiplier = 40;

            wheel_delta_x *= imprecise_scroll_multiplier;
            wheel_delta_y *= imprecise_scroll_multiplier;
        }

        scroll_gesture_phase = ns_scroll_event_to_scroll_gesture_phase(event);
    }

    int click_count = 0;
    if (type == Compositing::MouseEvent::Type::MouseDown || type == Compositing::MouseEvent::Type::MouseUp)
        click_count = static_cast<int>(event.clickCount);

    return { type, device_position, device_screen_position, button, button, modifiers, wheel_delta_x, wheel_delta_y, wheel_delta_precision, scroll_gesture_phase, click_count, nullptr };
}

struct DragData : public Compositing::BrowserInputData {
    explicit DragData(Vector<URL::URL> urls)
        : urls(move(urls))
    {
    }

    Vector<URL::URL> urls;
};

Web::DragEvent ns_event_to_drag_event(Web::DragEvent::Type type, id<NSDraggingInfo> event, NSView* view)
{
    auto position = [view convertPoint:event.draggingLocation fromView:nil];
    auto device_position = ns_point_to_gfx_point(position).to_type<Compositing::DevicePixels>();

    auto screen_position = [NSEvent mouseLocation];
    auto device_screen_position = ns_point_to_gfx_point(screen_position).to_type<Compositing::DevicePixels>();

    auto button = Compositing::MouseButton::Primary;
    auto modifiers = ns_modifiers_to_key_modifiers([NSEvent modifierFlags]);

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
            if (auto file = WebView::create_selected_file(file_path); file.is_error())
                warnln("Unable to open file {}: {}", file_path, file.error());
            else
                files.append(file.release_value());
        });
    } else if (type == Web::DragEvent::Type::Drop) {
        Vector<URL::URL> urls;

        for_each_file([&](ByteString const& file_path) {
            if (auto url = URL::create_with_url_or_path(file_path); url.has_value())
                urls.append(url.release_value());
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

static Compositing::KeyCode ns_key_code_to_key_code(unsigned short key_code, Compositing::KeyModifier& modifiers)
{
    auto augment_modifiers_and_return = [&](auto key, auto modifier) {
        modifiers = static_cast<Compositing::KeyModifier>(static_cast<unsigned>(modifiers) | modifier);
        return key;
    };

    // clang-format off
    switch (key_code) {
    case kVK_ANSI_0: return Compositing::KeyCode::Key_0;
    case kVK_ANSI_1: return Compositing::KeyCode::Key_1;
    case kVK_ANSI_2: return Compositing::KeyCode::Key_2;
    case kVK_ANSI_3: return Compositing::KeyCode::Key_3;
    case kVK_ANSI_4: return Compositing::KeyCode::Key_4;
    case kVK_ANSI_5: return Compositing::KeyCode::Key_5;
    case kVK_ANSI_6: return Compositing::KeyCode::Key_6;
    case kVK_ANSI_7: return Compositing::KeyCode::Key_7;
    case kVK_ANSI_8: return Compositing::KeyCode::Key_8;
    case kVK_ANSI_9: return Compositing::KeyCode::Key_9;
    case kVK_ANSI_A: return Compositing::KeyCode::Key_A;
    case kVK_ANSI_B: return Compositing::KeyCode::Key_B;
    case kVK_ANSI_C: return Compositing::KeyCode::Key_C;
    case kVK_ANSI_D: return Compositing::KeyCode::Key_D;
    case kVK_ANSI_E: return Compositing::KeyCode::Key_E;
    case kVK_ANSI_F: return Compositing::KeyCode::Key_F;
    case kVK_ANSI_G: return Compositing::KeyCode::Key_G;
    case kVK_ANSI_H: return Compositing::KeyCode::Key_H;
    case kVK_ANSI_I: return Compositing::KeyCode::Key_I;
    case kVK_ANSI_J: return Compositing::KeyCode::Key_J;
    case kVK_ANSI_K: return Compositing::KeyCode::Key_K;
    case kVK_ANSI_L: return Compositing::KeyCode::Key_L;
    case kVK_ANSI_M: return Compositing::KeyCode::Key_M;
    case kVK_ANSI_N: return Compositing::KeyCode::Key_N;
    case kVK_ANSI_O: return Compositing::KeyCode::Key_O;
    case kVK_ANSI_P: return Compositing::KeyCode::Key_P;
    case kVK_ANSI_Q: return Compositing::KeyCode::Key_Q;
    case kVK_ANSI_R: return Compositing::KeyCode::Key_R;
    case kVK_ANSI_S: return Compositing::KeyCode::Key_S;
    case kVK_ANSI_T: return Compositing::KeyCode::Key_T;
    case kVK_ANSI_U: return Compositing::KeyCode::Key_U;
    case kVK_ANSI_V: return Compositing::KeyCode::Key_V;
    case kVK_ANSI_W: return Compositing::KeyCode::Key_W;
    case kVK_ANSI_X: return Compositing::KeyCode::Key_X;
    case kVK_ANSI_Y: return Compositing::KeyCode::Key_Y;
    case kVK_ANSI_Z: return Compositing::KeyCode::Key_Z;
    case kVK_ANSI_Backslash: return Compositing::KeyCode::Key_Backslash;
    case kVK_ANSI_Comma: return Compositing::KeyCode::Key_Comma;
    case kVK_ANSI_Equal: return Compositing::KeyCode::Key_Equal;
    case kVK_ANSI_Grave: return Compositing::KeyCode::Key_Backtick;
    case kVK_ANSI_Keypad0: return augment_modifiers_and_return(Compositing::KeyCode::Key_0, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad1: return augment_modifiers_and_return(Compositing::KeyCode::Key_1, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad2: return augment_modifiers_and_return(Compositing::KeyCode::Key_2, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad3: return augment_modifiers_and_return(Compositing::KeyCode::Key_3, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad4: return augment_modifiers_and_return(Compositing::KeyCode::Key_4, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad5: return augment_modifiers_and_return(Compositing::KeyCode::Key_5, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad6: return augment_modifiers_and_return(Compositing::KeyCode::Key_6, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad7: return augment_modifiers_and_return(Compositing::KeyCode::Key_7, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad8: return augment_modifiers_and_return(Compositing::KeyCode::Key_8, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_Keypad9: return augment_modifiers_and_return(Compositing::KeyCode::Key_9, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadClear: return augment_modifiers_and_return(Compositing::KeyCode::Key_Delete, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadDecimal: return augment_modifiers_and_return(Compositing::KeyCode::Key_Period, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadDivide: return augment_modifiers_and_return(Compositing::KeyCode::Key_Slash, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadEnter: return augment_modifiers_and_return(Compositing::KeyCode::Key_Return, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadEquals: return augment_modifiers_and_return(Compositing::KeyCode::Key_Equal, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadMinus: return augment_modifiers_and_return(Compositing::KeyCode::Key_Minus, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadMultiply: return augment_modifiers_and_return(Compositing::KeyCode::Key_Asterisk, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_KeypadPlus: return augment_modifiers_and_return(Compositing::KeyCode::Key_Plus, Compositing::KeyModifier::Mod_Keypad);
    case kVK_ANSI_LeftBracket: return Compositing::KeyCode::Key_LeftBracket;
    case kVK_ANSI_Minus: return Compositing::KeyCode::Key_Minus;
    case kVK_ANSI_Period: return Compositing::KeyCode::Key_Period;
    case kVK_ANSI_Quote: return Compositing::KeyCode::Key_Apostrophe;
    case kVK_ANSI_RightBracket: return Compositing::KeyCode::Key_RightBracket;
    case kVK_ANSI_Semicolon: return Compositing::KeyCode::Key_Semicolon;
    case kVK_ANSI_Slash: return Compositing::KeyCode::Key_Slash;
    case kVK_CapsLock: return Compositing::KeyCode::Key_CapsLock;
    case kVK_Command: return Compositing::KeyCode::Key_LeftSuper;
    case kVK_Control: return Compositing::KeyCode::Key_LeftControl;
    case kVK_Delete: return Compositing::KeyCode::Key_Backspace;
    case kVK_DownArrow: return Compositing::KeyCode::Key_Down;
    case kVK_End: return Compositing::KeyCode::Key_End;
    case kVK_Escape: return Compositing::KeyCode::Key_Escape;
    case kVK_F1: return Compositing::KeyCode::Key_F1;
    case kVK_F2: return Compositing::KeyCode::Key_F2;
    case kVK_F3: return Compositing::KeyCode::Key_F3;
    case kVK_F4: return Compositing::KeyCode::Key_F4;
    case kVK_F5: return Compositing::KeyCode::Key_F5;
    case kVK_F6: return Compositing::KeyCode::Key_F6;
    case kVK_F7: return Compositing::KeyCode::Key_F7;
    case kVK_F8: return Compositing::KeyCode::Key_F8;
    case kVK_F9: return Compositing::KeyCode::Key_F9;
    case kVK_F10: return Compositing::KeyCode::Key_F10;
    case kVK_F11: return Compositing::KeyCode::Key_F11;
    case kVK_F12: return Compositing::KeyCode::Key_F12;
    case kVK_ForwardDelete: return Compositing::KeyCode::Key_Delete;
    case kVK_Home: return Compositing::KeyCode::Key_Home;
    case kVK_LeftArrow: return Compositing::KeyCode::Key_Left;
    case kVK_Option: return Compositing::KeyCode::Key_LeftAlt;
    case kVK_PageDown: return Compositing::KeyCode::Key_PageDown;
    case kVK_PageUp: return Compositing::KeyCode::Key_PageUp;
    case kVK_Return: return Compositing::KeyCode::Key_Return;
    case kVK_RightArrow: return Compositing::KeyCode::Key_Right;
    case kVK_RightCommand: return Compositing::KeyCode::Key_RightSuper;
    case kVK_RightControl: return Compositing::KeyCode::Key_RightControl;
    case kVK_RightOption: return Compositing::KeyCode::Key_RightAlt;
    case kVK_RightShift: return Compositing::KeyCode::Key_RightShift;
    case kVK_Shift: return Compositing::KeyCode::Key_LeftShift;
    case kVK_Space: return Compositing::KeyCode::Key_Space;
    case kVK_Tab: return Compositing::KeyCode::Key_Tab;
    case kVK_UpArrow: return Compositing::KeyCode::Key_Up;
    default: break;
    }
    // clang-format on

    return Compositing::KeyCode::Key_Invalid;
}

class KeyData : public Compositing::BrowserInputData {
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

Compositing::KeyEvent ns_event_to_key_event(Compositing::KeyEvent::Type type, NSEvent* event, bool should_insert_text)
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

    return { type, key_code, modifiers, code_point, repeat, should_insert_text, make<KeyData>(event) };
}

NSEvent* key_event_to_ns_event(Compositing::KeyEvent const& event)
{
    auto& browser_data = as<KeyData>(*event.browser_data);
    return browser_data.take_event();
}

}
