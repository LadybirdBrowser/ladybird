/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>
#include <LibWebCommon/PixelUnits.h>
#include <LibWebCommon/UIEvents/KeyCode.h>
#include <LibWebCommon/UIEvents/MouseButton.h>

namespace Web {

struct BrowserInputData {
    AK_ALLOC_WITH_KMALLOC;

    virtual ~BrowserInputData() = default;
};

struct WEBCOMMON_API KeyEvent {
    enum class Type : u8 {
        KeyDown,
        KeyUp,
    };

    KeyEvent clone_without_browser_data() const;

    Type type;
    Web::UIEvents::KeyCode key { Web::UIEvents::KeyCode::Key_Invalid };
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
    u32 code_point { 0 };
    bool repeat { false };
    bool should_insert_text { false };

    OwnPtr<BrowserInputData> browser_data;
    bool async_scroll_performed_default_action { false };

    // The UI process numbers the events it sends, and a completion names the event it finished.
    u64 id { 0 };
};

inline bool is_keyboard_scroll_key(Web::UIEvents::KeyCode key, u32 modifiers)
{
    switch (key) {
    case Web::UIEvents::KeyCode::Key_Space:
        return (modifiers & ~(Web::UIEvents::Mod_Shift | Web::UIEvents::Mod_Keypad)) == Web::UIEvents::Mod_None;
    case Web::UIEvents::KeyCode::Key_PageUp:
    case Web::UIEvents::KeyCode::Key_PageDown:
    case Web::UIEvents::KeyCode::Key_Up:
    case Web::UIEvents::KeyCode::Key_Down:
    case Web::UIEvents::KeyCode::Key_Left:
    case Web::UIEvents::KeyCode::Key_Right:
        return (modifiers & ~Web::UIEvents::Mod_Keypad) == Web::UIEvents::Mod_None;
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

struct WEBCOMMON_API MouseEvent {
    enum class Type : u8 {
        MouseDown,
        MouseUp,
        MouseMove,
        MouseLeave,
        MouseCancel,
        MouseWheel,
    };

    MouseEvent clone_without_browser_data() const;

    Type type;
    Web::DevicePixelPoint position;
    Web::DevicePixelPoint screen_position;
    Web::UIEvents::MouseButton button { Web::UIEvents::MouseButton::None };
    Web::UIEvents::MouseButton buttons { Web::UIEvents::MouseButton::None };
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
    double wheel_delta_x { 0 };
    double wheel_delta_y { 0 };
    WheelDeltaPrecision wheel_delta_precision { WheelDeltaPrecision::Discrete };
    ScrollGesturePhase scroll_gesture_phase { ScrollGesturePhase::None };
    int click_count { 0 };

    OwnPtr<BrowserInputData> browser_data;
    bool async_scroll_performed_default_action { false };
    u64 id { 0 };
    Optional<Web::ScrollbarDraggedByCompositor> scrollbar_dragged_by_compositor {};
};

struct WEBCOMMON_API PinchEvent {
    Web::DevicePixelPoint position;
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
    double scale_delta;
    u64 id { 0 };
};

}

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::KeyEvent const&);

template<>
WEBCOMMON_API ErrorOr<Web::KeyEvent> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::MouseEvent const&);

template<>
WEBCOMMON_API ErrorOr<Web::MouseEvent> decode(Decoder&);

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::PinchEvent const&);

template<>
WEBCOMMON_API ErrorOr<Web::PinchEvent> decode(Decoder&);

}
