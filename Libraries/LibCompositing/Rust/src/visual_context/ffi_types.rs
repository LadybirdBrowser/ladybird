/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The plain FFI types the visual context tree hands across the C++ boundary on both sides of the
//! compositor process boundary.

use libgfx_rust::{FloatMatrix4x4, FloatPoint, translation_matrix};
use libipc_rust::wire::{Decode, Encode};

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextTreeInputs {
    pub device_pixels_per_css_pixel: f64,
    pub visual_viewport_offset_x: f64,
    pub visual_viewport_offset_y: f64,
    pub visual_viewport_scale: f64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualViewportTransform {
    pub matrix: FloatMatrix4x4,
    pub origin: FloatPoint,
}

impl FfiVisualViewportTransform {
    pub fn matrix_including_origin(&self) -> FloatMatrix4x4 {
        translation_matrix(self.origin.x, self.origin.y, 0.0)
            .multiplied(self.matrix)
            .multiplied(translation_matrix(-self.origin.x, -self.origin.y, 0.0))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum FfiVisualAnimationTargetKind {
    Opacity,
    BackgroundColor,
    Filter,
    Transform,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, Encode, Decode)]
#[repr(u8)]
pub enum FfiVisualAnimationTransformOperationKind {
    Translate,
    Translate3d,
    TranslateX,
    TranslateY,
    TranslateZ,
    Scale,
    Scale3d,
    ScaleX,
    ScaleY,
    ScaleZ,
    Rotate,
    RotateX,
    RotateY,
    RotateZ,
    Skew,
    SkewX,
    SkewY,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiCompositorAnimationPublishOutcome {
    /// The tree took the list; false when it already carried the same animations.
    pub published: bool,
    pub parameters_changed: bool,
    pub timing_anchors_changed: bool,
}

/// What a tree reports about the animations it carries, for test introspection.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiVisualAnimationSummary {
    pub count: usize,
    pub local_time_at_anchor_ms_of_first: f64,
    pub share_timing_anchor: bool,
    pub targets_are_valid: bool,
}
