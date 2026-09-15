/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::RecordingInputs;
use super::cache::CaptureKind;
use super::inputs::UncapturedContentInputs;
use super::paint::background_resolution::root_background_canvas_rect;
use crate::css::css_pixels::CssPixelRect;
use crate::painting::force_dark::ForceDarkSettings;
use crate::painting::paint_state::PaintState;
use crate::painting::paintable_rows::PaintableRowsRef;

/// Recording inputs whose changes affect cache reuse independently of per-paintable dirtiness.
/// Keep these with the cache source: a read-only recording must not replace them.
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct PaintCacheInputs {
    // A default-constructed source's 0.0 never matches a real recording scale.
    pub(crate) device_pixels_per_css_pixel: f64,
    pub(crate) force_dark_enabled: bool,
    pub(crate) force_dark_settings: ForceDarkSettings,
    pub(crate) should_show_line_box_borders: bool,
    pub(crate) should_paint_overlay: bool,
    pub(crate) uncaptured: UncapturedContentInputs,
    // The area the root background paints: the viewport united with the root's overflow.
    // It is compared before recording so only the root's caches are dropped when it changes.
    pub(crate) root_background_canvas_rect: CssPixelRect,
    // Whether any box but the viewport could take a wheel event decides if the scroll metadata
    // inside subtree captures carries per-box wheel hit-test targets.
    pub(crate) has_non_viewport_wheel_scroll_target_candidate: bool,
}

impl PaintCacheInputs {
    pub(crate) fn from_recording_inputs(
        rows: &PaintableRowsRef<'_>,
        inputs: &RecordingInputs<'_>,
        paint_state: &PaintState,
    ) -> Self {
        Self {
            device_pixels_per_css_pixel: inputs.device_pixels_per_css_pixel,
            force_dark_enabled: inputs.force_dark_enabled,
            force_dark_settings: inputs.force_dark_settings,
            should_show_line_box_borders: inputs.should_show_line_box_borders,
            should_paint_overlay: inputs.should_paint_overlay,
            uncaptured: inputs.uncaptured,
            root_background_canvas_rect: root_background_canvas_rect(
                rows,
                inputs.uncaptured.root_background_source.root_layout_node,
                inputs.css_viewport_rect,
            ),
            has_non_viewport_wheel_scroll_target_candidate: paint_state
                .visual_context
                .scroll_state
                .has_non_viewport_wheel_scroll_target_candidate,
        }
    }

    pub(crate) fn compatibility_with(&self, source: &Self) -> PaintCacheCompatibility {
        // Preserve the full paint-cache invalidation used for changes to color resolution.
        let colors_match = self.uncaptured.canvas_color == source.uncaptured.canvas_color
            && self.force_dark_enabled == source.force_dark_enabled
            && self.force_dark_settings == source.force_dark_settings;
        // Commands use device pixels, while hit-test items use CSS pixels. Line-box borders
        // replace normal text painting but do not change hit-test items.
        let commands = colors_match
            && self.device_pixels_per_css_pixel == source.device_pixels_per_css_pixel
            && !self.should_show_line_box_borders
            && !source.should_show_line_box_borders;
        let hit_test_items = colors_match;
        // Subtree captures also hold the content recorded outside box-phase captures, the scroll
        // metadata and the wheel-target facts of hit-test items, which read the uncaptured-content
        // inputs and depend on whether any box but the viewport could take a wheel event. Every
        // reuse of a subtree capture, spliced whole or copied around a patch, passes this check.
        let descendant_subtrees = commands
            && hit_test_items
            && self.uncaptured == source.uncaptured
            && self.has_non_viewport_wheel_scroll_target_candidate
                == source.has_non_viewport_wheel_scroll_target_candidate;
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
