/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericShorthands.h>
#include <AK/Math.h>
#include <AK/OwnPtr.h>
#include <AK/Vector.h>
#include <LibGfx/Point.h>
#include <LibWebCommon/Export.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>
#include <LibWebCommon/Page/TextEditingCommand.h>
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

    OwnPtr<BrowserInputData> browser_data {};
    bool async_scroll_performed_default_action { false };

    // The UI process numbers the events it sends, and a completion names the event it finished.
    u64 id { 0 };
    Vector<TextEditingCommand> editing_commands {};
    bool native_editing_binding_resolved { false };
};

inline bool is_keyboard_scroll_key(Web::UIEvents::KeyCode key, u32 modifiers)
{
    switch (key) {
    case Web::UIEvents::KeyCode::Key_Space:
        return (modifiers & ~(Web::UIEvents::Mod_Shift | Web::UIEvents::Mod_Keypad)) == Web::UIEvents::Mod_None;
    case Web::UIEvents::KeyCode::Key_Up:
    case Web::UIEvents::KeyCode::Key_Down:
        return first_is_one_of(modifiers & ~Web::UIEvents::Mod_Keypad, Web::UIEvents::Mod_None, Web::UIEvents::Mod_PlatformCtrl);
    case Web::UIEvents::KeyCode::Key_PageUp:
    case Web::UIEvents::KeyCode::Key_PageDown:
    case Web::UIEvents::KeyCode::Key_Left:
    case Web::UIEvents::KeyCode::Key_Right:
    case Web::UIEvents::KeyCode::Key_Home:
    case Web::UIEvents::KeyCode::Key_End:
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

// Besides its pixel delta, wheel input counts how far the wheel turned in 120ths of a tick, the unit of the legacy
// wheelDelta. A tick is a wheel notch, or one line on platforms that accelerate the wheel by whole lines. Input that
// reports only a pixel delta counts its ticks from that: a notch scrolls three 40-pixel lines, and precise input has
// no notches, so it counts one tick per 40-pixel line step.
// INTEROP: For macOS touchpad scrolling, Blink, WebKit, and Gecko all report 3 units per pixel; Blink and WebKit
//          truncate, Gecko rounds. For the ctrl-wheel events of a pinch, WebKit and Gecko also report 3 units per
//          pixel, while Blink reports 120 units per event.
constexpr double wheel_tick_120ths_from_pixel_delta(double pixel_delta, WheelDeltaPrecision precision)
{
    constexpr double tick_120ths_per_tick = 120;
    constexpr double pixels_per_notch = 3 * 40;
    constexpr double pixels_per_precise_tick = 40;
    if (precision == WheelDeltaPrecision::Precise)
        return pixel_delta * (tick_120ths_per_tick / pixels_per_precise_tick);
    return pixel_delta * (tick_120ths_per_tick / pixels_per_notch);
}

// Input that scrolls with a gesture, such as a touchpad, reports whether the user is still making that gesture,
// whether a flick has handed the scrolling over to momentum, and when it ends.
enum class ScrollGesturePhase : u8 {
    None,
    Ongoing,
    Momentum,
    Ended,
};

enum class PointerType : u8 {
    Mouse,
    Pen,
    Touch,
};

// https://w3c.github.io/pointerevents/#pointerevent-interface
struct PointerProperties {
    AK_ALLOC_WITH_KMALLOC;

    i32 pointer_id { 0 };
    PointerType pointer_type { PointerType::Mouse };
    double width { 1 };
    double height { 1 };
    double pressure { 0 };
    double tangential_pressure { 0 };
    i32 tilt_x { 0 };
    i32 tilt_y { 0 };
    i32 twist { 0 };
    double altitude_angle { AK::Pi<double> / 2 };
    double azimuth_angle { 0 };
};

// NB: Native mouse input leaves this empty, avoiding an allocation and keeping the input event small.
class WEBCOMMON_API PointerInputData {
public:
    PointerInputData() = default;
    explicit PointerInputData(Optional<PointerProperties>);
    PointerInputData(PointerInputData const&);
    PointerInputData(PointerInputData&&) = default;
    PointerInputData& operator=(PointerInputData&&) = default;

    Optional<PointerProperties> properties() const;
    PointerProperties const* ptr() const { return m_properties.ptr(); }

private:
    OwnPtr<PointerProperties> m_properties;
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
    double wheel_tick_120ths_x { 0 };
    double wheel_tick_120ths_y { 0 };
    WheelDeltaPrecision wheel_delta_precision { WheelDeltaPrecision::Discrete };
    ScrollGesturePhase scroll_gesture_phase { ScrollGesturePhase::None };
    int click_count { 0 };

    OwnPtr<BrowserInputData> browser_data {};
    bool async_scroll_performed_default_action { false };
    u64 id { 0 };
    Optional<Web::ScrollbarDraggedByCompositor> scrollbar_dragged_by_compositor {};
    PointerInputData pointer_data {};
};

struct WEBCOMMON_API PinchEvent {
    Web::DevicePixelPoint position;
    Web::UIEvents::KeyModifier modifiers { Web::UIEvents::KeyModifier::Mod_None };
    double scale_delta;
    u64 id { 0 };
};

}

#include <LibWebCommon/Page/InputEventCodecs.h>
