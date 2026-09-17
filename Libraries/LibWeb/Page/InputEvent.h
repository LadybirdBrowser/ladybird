/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWeb/HTML/SelectedFile.h>
#include <LibWeb/Page/PageId.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/UIEvents/MouseButton.h>

namespace Web {

struct BrowserInputData {
    virtual ~BrowserInputData() = default;
};

struct WEB_API KeyEvent {
    enum class Type : u8 {
        KeyDown,
        KeyUp,
    };

    KeyEvent clone_without_browser_data() const;

    Type type;
    UIEvents::KeyCode key { UIEvents::KeyCode::Key_Invalid };
    UIEvents::KeyModifier modifiers { UIEvents::KeyModifier::Mod_None };
    u32 code_point { 0 };
    bool repeat { false };
    bool should_insert_text { false };

    OwnPtr<BrowserInputData> browser_data;
    bool async_scroll_performed_default_action { false };

    // The UI process numbers the events it sends, and a completion names the event it finished.
    u64 id { 0 };
};

inline bool is_keyboard_scroll_key(UIEvents::KeyCode key, u32 modifiers)
{
    switch (key) {
    case UIEvents::KeyCode::Key_Space:
        return (modifiers & ~(UIEvents::Mod_Shift | UIEvents::Mod_Keypad)) == UIEvents::Mod_None;
    case UIEvents::KeyCode::Key_PageUp:
    case UIEvents::KeyCode::Key_PageDown:
    case UIEvents::KeyCode::Key_Up:
    case UIEvents::KeyCode::Key_Down:
    case UIEvents::KeyCode::Key_Left:
    case UIEvents::KeyCode::Key_Right:
        return (modifiers & ~UIEvents::Mod_Keypad) == UIEvents::Mod_None;
    default:
        return false;
    }
}

// Discrete wheel deltas come from stepwise input such as mouse wheel notches; precise wheel deltas come from input
// that reports exact pixel distances, such as touchpad panning gestures.
enum class WheelDeltaPrecision : u8 {
    Discrete,
    Precise,
};

// Input that scrolls with a gesture, such as a touchpad, reports whether the user is still making that gesture,
// whether a flick has handed the scrolling over to momentum, and when it ends.
enum class ScrollGesturePhase : u8 {
    None,
    Ongoing,
    Momentum,
    Ended,
};

struct WEB_API MouseEvent {
    enum class Type : u8 {
        MouseDown,
        MouseUp,
        MouseMove,
        MouseLeave,
        MouseWheel,
    };

    MouseEvent clone_without_browser_data() const;

    Type type;
    Web::DevicePixelPoint position;
    Web::DevicePixelPoint screen_position;
    UIEvents::MouseButton button { UIEvents::MouseButton::None };
    UIEvents::MouseButton buttons { UIEvents::MouseButton::None };
    UIEvents::KeyModifier modifiers { UIEvents::KeyModifier::Mod_None };
    double wheel_delta_x { 0 };
    double wheel_delta_y { 0 };
    WheelDeltaPrecision wheel_delta_precision { WheelDeltaPrecision::Discrete };
    ScrollGesturePhase scroll_gesture_phase { ScrollGesturePhase::None };
    int click_count { 0 };

    OwnPtr<BrowserInputData> browser_data;
    bool async_scroll_performed_default_action { false };
    u64 id { 0 };
};

struct WEB_API DragEvent {
    enum class Type : u8 {
        DragStart,
        DragMove,
        DragEnd,
        Drop,
    };

    DragEvent clone_without_browser_data() const;

    Type type;
    Web::DevicePixelPoint position;
    Web::DevicePixelPoint screen_position;
    UIEvents::MouseButton button { UIEvents::MouseButton::None };
    UIEvents::MouseButton buttons { UIEvents::MouseButton::None };
    UIEvents::KeyModifier modifiers { UIEvents::KeyModifier::Mod_None };
    Vector<HTML::SelectedFile> files;

    OwnPtr<BrowserInputData> browser_data;
    u64 id { 0 };
};

struct WEB_API PinchEvent {
    Web::DevicePixelPoint position;
    UIEvents::KeyModifier modifiers { UIEvents::KeyModifier::Mod_None };
    double scale_delta;
    u64 id { 0 };
};

using InputEvent = Variant<KeyEvent, MouseEvent, DragEvent, PinchEvent>;

inline u64 input_event_id(InputEvent const& event)
{
    return event.visit([](auto const& event) { return event.id; });
}

inline void set_input_event_id(InputEvent& event, u64 id)
{
    event.visit([&](auto& event) { event.id = id; });
}

struct QueuedInputEvent {
    Web::PageId page_id { 0 };
    InputEvent event;
    // The events coalesced into this one, which finish when it does.
    Vector<u64> coalesced_event_ids;
    // The local root the event targets when it is not the page's traversable: a navigable whose parent's document
    // another process hosts, which the UI process addresses by id.
    Optional<HTML::CrossProcessId> navigable_id;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::KeyEvent const&);

template<>
WEB_API ErrorOr<Web::KeyEvent> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::MouseEvent const&);

template<>
WEB_API ErrorOr<Web::MouseEvent> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::DragEvent const&);

template<>
WEB_API ErrorOr<Web::DragEvent> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::PinchEvent const&);

template<>
WEB_API ErrorOr<Web::PinchEvent> decode(Decoder&);

}
