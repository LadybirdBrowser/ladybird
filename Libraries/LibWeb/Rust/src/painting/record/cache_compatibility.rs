/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::RecordingInputs;
use super::cache::CaptureKind;
use crate::painting::force_dark::ForceDarkSettings;
use libgfx_rust::Color;

/// Recording inputs whose changes affect cache reuse independently of per-paintable dirtiness.
/// Keep these with the cache source: a read-only recording must not replace them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct PaintCacheInputs {
    // A default-constructed source's 0.0 never matches a real recording scale.
    pub(crate) device_pixels_per_css_pixel: f64,
    pub(crate) canvas_color: Color,
    pub(crate) force_dark_enabled: bool,
    pub(crate) force_dark_settings: ForceDarkSettings,
    pub(crate) should_show_line_box_borders: bool,
    pub(crate) should_paint_overlay: bool,
    pub(crate) has_blocking_wheel_event_region_covering_viewport: bool,
}

impl PaintCacheInputs {
    pub(crate) fn from_recording_inputs(inputs: &RecordingInputs<'_>) -> Self {
        Self {
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            canvas_color: inputs.canvas_color,
            force_dark_enabled: inputs.force_dark_enabled,
            force_dark_settings: inputs.force_dark_settings,
            should_show_line_box_borders: inputs.should_show_line_box_borders,
            should_paint_overlay: inputs.should_paint_overlay,
            has_blocking_wheel_event_region_covering_viewport: inputs.has_blocking_wheel_event_region_covering_viewport,
        }
    }

    pub(crate) fn compatibility_with(&self, source: &Self) -> PaintCacheCompatibility {
        // Preserve the full paint-cache invalidation used for changes to color resolution.
        let colors_match = self.canvas_color == source.canvas_color
            && self.force_dark_enabled == source.force_dark_enabled
            && self.force_dark_settings == source.force_dark_settings;
        // Commands use device pixels, while hit-test items use CSS pixels. Line-box borders
        // replace normal text painting but do not change hit-test items.
        let commands = colors_match
            && self.device_pixels_per_css_pixel == source.device_pixels_per_css_pixel
            && !self.should_show_line_box_borders
            && !source.should_show_line_box_borders;
        let hit_test_items = colors_match;
        // Subtree captures also contain scrolling metadata, emitted outside box-phase captures.
        let descendant_subtrees = commands
            && hit_test_items
            && self.has_blocking_wheel_event_region_covering_viewport
                == source.has_blocking_wheel_event_region_covering_viewport;
        // Only painting a whole stacking context conditionally includes its overlay phase.
        let stacking_contexts = descendant_subtrees && self.should_paint_overlay == source.should_paint_overlay;
        PaintCacheCompatibility {
            commands,
            hit_test_items,
            descendant_subtrees,
            stacking_contexts,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PaintCacheCompatibility {
    pub(crate) commands: bool,
    pub(crate) hit_test_items: bool,
    descendant_subtrees: bool,
    stacking_contexts: bool,
}

impl PaintCacheCompatibility {
    pub(crate) fn allows_subtree(self, kind: CaptureKind) -> bool {
        match kind {
            CaptureKind::DescendantSubtreePhase(_) => self.descendant_subtrees,
            CaptureKind::PaintedAsStackingContext => self.stacking_contexts,
            CaptureKind::BoxPhase(_) => unreachable!("a box phase capture is not a subtree capture"),
        }
    }
}
