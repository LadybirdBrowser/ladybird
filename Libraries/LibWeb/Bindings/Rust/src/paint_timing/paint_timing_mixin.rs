/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! PaintTimingMixin, https://w3c.github.io/paint-timing/#sec-PaintTimingMixin

/// https://w3c.github.io/paint-timing/#paint-timing-info
#[derive(Clone, Copy)]
pub(crate) struct PaintTimingInfo {
    /// https://w3c.github.io/paint-timing/#paint-timing-info-rendering-update-end-time
    pub(crate) rendering_update_end_time: f64,
    /// https://w3c.github.io/paint-timing/#paint-timing-info-implementation-defined-presentation-time
    pub(crate) implementation_defined_presentation_time: Option<f64>,
}

impl PaintTimingInfo {
    pub(crate) fn from_abi(
        rendering_update_end_time: f64,
        has_presentation_time: bool,
        implementation_defined_presentation_time: f64,
    ) -> Self {
        Self {
            rendering_update_end_time,
            implementation_defined_presentation_time: has_presentation_time
                .then_some(implementation_defined_presentation_time),
        }
    }
}

// https://w3c.github.io/paint-timing/#default-paint-timestamp
pub(crate) fn default_paint_timestamp(paint_timing_info: &PaintTimingInfo) -> f64 {
    // To get the default paint timestamp for a paint timing info paintTimingInfo, return paintTimingInfo's
    // implementation-defined presentation time if it is non-null, otherwise paintTimingInfo's rendering update end time.
    paint_timing_info
        .implementation_defined_presentation_time
        .unwrap_or(paint_timing_info.rendering_update_end_time)
}

/// https://w3c.github.io/paint-timing/#sec-PaintTimingMixin
pub(crate) struct PaintTimingMixin {
    // https://w3c.github.io/paint-timing/#painttimingmixin-paint-timing-info
    // NB: Every object that includes this mixin is created with a paint timing info, so it is never null here.
    paint_timing_info: PaintTimingInfo,
}

impl PaintTimingMixin {
    pub(crate) fn new(paint_timing_info: PaintTimingInfo) -> Self {
        Self { paint_timing_info }
    }

    // https://w3c.github.io/paint-timing/#dom-painttimingmixin-painttime
    pub(crate) fn paint_time(&self) -> f64 {
        // The paintTime attribute's getter step is to return this's paint timing info's rendering update end time.
        self.paint_timing_info.rendering_update_end_time
    }

    // https://w3c.github.io/paint-timing/#dom-painttimingmixin-presentationtime
    pub(crate) fn presentation_time(&self) -> Option<f64> {
        // The presentationTime attribute's getter step, if exists, is to return this's paint timing info's
        // implementation-defined presentation time.
        self.paint_timing_info.implementation_defined_presentation_time
    }

    pub(crate) fn paint_timing_info(&self) -> &PaintTimingInfo {
        &self.paint_timing_info
    }
}
