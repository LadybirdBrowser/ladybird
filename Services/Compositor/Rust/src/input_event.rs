/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcompositing_rust::scrolling::{KeyCode, KeyModifier, WheelDeltaPrecision};

pub fn is_keyboard_scroll_key(key: KeyCode, modifiers: u32) -> bool {
    match key {
        KeyCode::Key_Space => {
            modifiers & !(KeyModifier::Mod_Shift | KeyModifier::Mod_Keypad).0 == KeyModifier::Mod_None.0
        }
        KeyCode::Key_Up | KeyCode::Key_Down => {
            let modifiers = modifiers & !KeyModifier::Mod_Keypad.0;
            modifiers == KeyModifier::Mod_None.0 || modifiers == KeyModifier::Mod_PlatformCtrl.0
        }
        KeyCode::Key_PageUp
        | KeyCode::Key_PageDown
        | KeyCode::Key_Left
        | KeyCode::Key_Right
        | KeyCode::Key_Home
        | KeyCode::Key_End => modifiers & !KeyModifier::Mod_Keypad.0 == KeyModifier::Mod_None.0,
        _ => false,
    }
}

// Besides its pixel delta, wheel input counts how far the wheel turned in 120ths of a tick, the unit of the legacy
// wheelDelta. A tick is a wheel notch, or one line on platforms that accelerate the wheel by whole lines. Input that
// reports only a pixel delta counts its ticks from that: a notch scrolls three 40-pixel lines, and precise input has
// no notches, so it counts one tick per 40-pixel line step.
// INTEROP: For macOS touchpad scrolling, Blink, WebKit, and Gecko all report 3 units per pixel; Blink and WebKit
//          truncate, Gecko rounds. For the ctrl-wheel events of a pinch, WebKit and Gecko also report 3 units per
//          pixel, while Blink reports 120 units per event.
pub fn wheel_tick_120ths_from_pixel_delta(pixel_delta: f64, precision: WheelDeltaPrecision) -> f64 {
    const TICK_120THS_PER_TICK: f64 = 120.0;
    const PIXELS_PER_NOTCH: f64 = 3.0 * 40.0;
    const PIXELS_PER_PRECISE_TICK: f64 = 40.0;
    if precision == WheelDeltaPrecision::Precise {
        return pixel_delta * (TICK_120THS_PER_TICK / PIXELS_PER_PRECISE_TICK);
    }
    pixel_delta * (TICK_120THS_PER_TICK / PIXELS_PER_NOTCH)
}
