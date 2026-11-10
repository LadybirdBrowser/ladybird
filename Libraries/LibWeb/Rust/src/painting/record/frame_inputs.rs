/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::RecordingInputs;
use super::inputs::UncapturedContentInputs;
use crate::painting::force_dark::ForceDarkSettings;
use crate::painting::published_frame::PublishedPaintState;
use libgfx_rust::Color;

/// The recording inputs every producer may read. A frame copies from the published recording only
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
    // Whether any box but the viewport could take or keep a wheel event decides if the scroll metadata
    // inside subtree captures carries per-box wheel hit-test targets.
    pub(crate) has_non_viewport_wheel_scroll_target_candidate: bool,
    // The default selection colors come from the system palette, which changes with the system's highlight color.
    pub(crate) selection_colors: SelectionColors,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct SelectionColors {
    from_palette: Color,
    light: Color,
    dark: Color,
    inactive_from_palette: Color,
    inactive_light: Color,
    inactive_dark: Color,
    palette_is_dark: bool,
}

impl FrameInputs {
    pub(crate) fn from_recording_inputs(inputs: &RecordingInputs, paint_state: &PublishedPaintState) -> Self {
        Self {
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            force_dark_enabled: inputs.force_dark_enabled,
            force_dark_settings: inputs.force_dark_settings,
            should_show_line_box_borders: inputs.should_show_line_box_borders,
            should_paint_overlay: inputs.should_paint_overlay,
            uncaptured: inputs.uncaptured,
            has_non_viewport_wheel_scroll_target_candidate: paint_state.has_non_viewport_wheel_scroll_target_candidate,
            selection_colors: SelectionColors {
                from_palette: inputs.selection_background_from_palette,
                light: inputs.selection_background_light,
                dark: inputs.selection_background_dark,
                inactive_from_palette: inputs.inactive_selection_background_from_palette,
                inactive_light: inputs.inactive_selection_background_light,
                inactive_dark: inputs.inactive_selection_background_dark,
                palette_is_dark: inputs.palette_is_dark,
            },
        }
    }

    pub(crate) fn records_per_box_wheel_hit_test_targets(&self) -> bool {
        self.has_non_viewport_wheel_scroll_target_candidate
            || self.uncaptured.has_navigable_container_with_composited_context
    }
}
