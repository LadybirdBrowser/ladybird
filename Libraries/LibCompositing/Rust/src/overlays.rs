/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Where the overlays the browser draws over a page put their parts. The compositor paints them there, and the browser
//! finds the buttons a press lands on with the same geometry.

use libgfx_rust::{IntRect, IntSize};

/// A length of an overlay in CSS pixels, in whole device pixels.
pub fn scaled_overlay_length(length: i32, device_pixel_ratio: f64) -> i32 {
    1.max((f64::from(length) * device_pixel_ratio).round() as i32)
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct PausedDebuggerOverlayGeometry {
    pub toolbar: IntRect,
    pub message: IntRect,
    pub step_over_button: IntRect,
    pub continue_button: IntRect,
}

pub fn paused_debugger_overlay_geometry(
    viewport_size: IntSize,
    device_pixel_ratio: f64,
) -> PausedDebuggerOverlayGeometry {
    let width = viewport_size.width.min(scaled_overlay_length(250, device_pixel_ratio));
    let height = viewport_size.height.min(scaled_overlay_length(38, device_pixel_ratio));
    let x = 0.max((viewport_size.width - width) / 2);
    let y = scaled_overlay_length(30, device_pixel_ratio).min(0.max(viewport_size.height - height));
    let button_width = scaled_overlay_length(40, device_pixel_ratio).min(width / 2);
    let message_width = width - button_width * 2;
    PausedDebuggerOverlayGeometry {
        toolbar: IntRect::new(x, y, width, height),
        message: IntRect::new(x, y, message_width, height),
        step_over_button: IntRect::new(x + message_width, y, button_width, height),
        continue_button: IntRect::new(x + message_width + button_width, y, button_width, height),
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct PictureInPictureControlsGeometry {
    pub back_to_tab_button: IntRect,
    pub close_button: IntRect,
}

impl PictureInPictureControlsGeometry {
    /// The rect that holds both buttons.
    pub fn buttons_bounds(&self) -> IntRect {
        self.back_to_tab_button.united(self.close_button)
    }
}

pub fn picture_in_picture_controls_geometry(
    viewport_size: IntSize,
    device_pixel_ratio: f64,
) -> PictureInPictureControlsGeometry {
    // Close is in the top left corner, and the way back to the tab in the top right one.
    let button_size = scaled_overlay_length(28, device_pixel_ratio);
    let inset = scaled_overlay_length(10, device_pixel_ratio);
    PictureInPictureControlsGeometry {
        back_to_tab_button: IntRect::new(
            viewport_size.width - inset - button_size,
            inset,
            button_size,
            button_size,
        ),
        close_button: IntRect::new(inset, inset, button_size, button_size),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn compositing_scaled_overlay_length(length: i32, device_pixel_ratio: f64) -> i32 {
    scaled_overlay_length(length, device_pixel_ratio)
}

#[unsafe(no_mangle)]
pub extern "C" fn compositing_paused_debugger_overlay_geometry(
    viewport_size: IntSize,
    device_pixel_ratio: f64,
    geometry: &mut PausedDebuggerOverlayGeometry,
) {
    *geometry = paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio);
}

#[unsafe(no_mangle)]
pub extern "C" fn compositing_picture_in_picture_controls_geometry(
    viewport_size: IntSize,
    device_pixel_ratio: f64,
    geometry: &mut PictureInPictureControlsGeometry,
) {
    *geometry = picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio);
}
