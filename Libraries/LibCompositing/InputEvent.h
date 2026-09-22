/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/OwnPtr.h>
#include <LibCompositing/Export.h>
#include <LibCompositing/KeyCode.h>
#include <LibCompositing/MouseButton.h>
#include <LibCompositing/PixelUnits.h>
#include <LibCompositing/Scrolling/AsyncScrollNodeStableID.h>
#include <LibGfx/Point.h>
#include <LibIPC/Forward.h>

namespace Compositing {

struct BrowserInputData {
    AK_ALLOC_WITH_KMALLOC;

    virtual ~BrowserInputData() = default;
};

struct COMPOSITING_API KeyEvent {
    enum class Type : u8 {
        KeyDown,
        KeyUp,
    };

    KeyEvent clone_without_browser_data() const;

    Type type;
    Compositing::KeyCode key { Compositing::KeyCode::Key_Invalid };
    Compositing::KeyModifier modifiers { Compositing::KeyModifier::Mod_None };
    u32 code_point { 0 };
    bool repeat { false };
    bool should_insert_text { false };

    OwnPtr<BrowserInputData> browser_data;
    bool async_scroll_performed_default_action { false };

    // The UI process numbers the events it sends, and a completion names the event it finished.
    u64 id { 0 };
};

inline bool is_keyboard_scroll_key(Compositing::KeyCode key, u32 modifiers)
{
    switch (key) {
    case Compositing::KeyCode::Key_Space:
        return (modifiers & ~(Compositing::Mod_Shift | Compositing::Mod_Keypad)) == Compositing::Mod_None;
    case Compositing::KeyCode::Key_PageUp:
    case Compositing::KeyCode::Key_PageDown:
    case Compositing::KeyCode::Key_Up:
    case Compositing::KeyCode::Key_Down:
    case Compositing::KeyCode::Key_Left:
    case Compositing::KeyCode::Key_Right:
        return (modifiers & ~Compositing::Mod_Keypad) == Compositing::Mod_None;
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

struct COMPOSITING_API MouseEvent {
    enum class Type : u8 {
        MouseDown,
        MouseUp,
        MouseMove,
        MouseLeave,
        MouseWheel,
    };

    MouseEvent clone_without_browser_data() const;

    Type type;
    Compositing::DevicePixelPoint position;
    Compositing::DevicePixelPoint screen_position;
    Compositing::MouseButton button { Compositing::MouseButton::None };
    Compositing::MouseButton buttons { Compositing::MouseButton::None };
    Compositing::KeyModifier modifiers { Compositing::KeyModifier::Mod_None };
    double wheel_delta_x { 0 };
    double wheel_delta_y { 0 };
    WheelDeltaPrecision wheel_delta_precision { WheelDeltaPrecision::Discrete };
    ScrollGesturePhase scroll_gesture_phase { ScrollGesturePhase::None };
    int click_count { 0 };

    OwnPtr<BrowserInputData> browser_data;
    bool async_scroll_performed_default_action { false };
    u64 id { 0 };
    Optional<Compositing::ScrollbarDraggedByCompositor> scrollbar_dragged_by_compositor {};
};

struct COMPOSITING_API PinchEvent {
    Compositing::DevicePixelPoint position;
    Compositing::KeyModifier modifiers { Compositing::KeyModifier::Mod_None };
    double scale_delta;
    u64 id { 0 };
};

}

namespace IPC {

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::KeyEvent const&);

template<>
COMPOSITING_API ErrorOr<Compositing::KeyEvent> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::MouseEvent const&);

template<>
COMPOSITING_API ErrorOr<Compositing::MouseEvent> decode(Decoder&);

template<>
COMPOSITING_API ErrorOr<void> encode(Encoder&, Compositing::PinchEvent const&);

template<>
COMPOSITING_API ErrorOr<Compositing::PinchEvent> decode(Decoder&);

}
