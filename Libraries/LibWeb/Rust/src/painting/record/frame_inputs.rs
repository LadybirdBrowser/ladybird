/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::RecordingInputs;
use super::inputs::UncapturedContentInputs;
use crate::painting::force_dark::ForceDarkSettings;
use crate::painting::paint_state::PaintState;

/// The recording inputs every producer may read. A frame copies from the published frame only
/// while they are unchanged; otherwise it records from scratch. They stay with the published
/// frame, which a read-only recording never replaces.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct FrameInputs {
    // A default-constructed source's 0.0 never matches a real recording scale.
    pub(crate) device_pixels_per_css_pixel: f64,
    pub(crate) force_dark_enabled: bool,
    pub(crate) force_dark_settings: ForceDarkSettings,
    pub(crate) should_show_line_box_borders: bool,
    pub(crate) should_paint_overlay: bool,
    pub(crate) uncaptured: UncapturedContentInputs,
    // Whether any box but the viewport could take a wheel event decides if the scroll metadata
    // inside subtree captures carries per-box wheel hit-test targets.
    pub(crate) has_non_viewport_wheel_scroll_target_candidate: bool,
}

impl FrameInputs {
    pub(crate) fn from_recording_inputs(inputs: &RecordingInputs<'_>, paint_state: &PaintState) -> Self {
        Self {
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            force_dark_enabled: inputs.force_dark_enabled,
            force_dark_settings: inputs.force_dark_settings,
            should_show_line_box_borders: inputs.should_show_line_box_borders,
            should_paint_overlay: inputs.should_paint_overlay,
            uncaptured: inputs.uncaptured,
            has_non_viewport_wheel_scroll_target_candidate: paint_state
                .visual_context
                .scroll_state
                .has_non_viewport_wheel_scroll_target_candidate,
        }
    }
}
