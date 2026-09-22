/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The plain FFI types the visual context tree hands across the C++ boundary on both sides of the
//! compositor process boundary.

use crate::painting::display_list::commands::OptionalF32;
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect, FloatSize};

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextTreeInputs {
    pub device_pixels_per_css_pixel: f64,
    pub visual_viewport_offset_x: f64,
    pub visual_viewport_offset_y: f64,
    pub visual_viewport_scale: f64,
    pub viewport_wheel_overflow_x: u8,
    pub viewport_wheel_overflow_y: u8,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualViewportTransform {
    pub matrix: FloatMatrix4x4,
    pub origin: FloatPoint,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualAnimationTargetKind {
    Opacity,
    BackgroundColor,
    Filter,
    Transform,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualAnimationPlaybackDirection {
    Normal,
    Reverse,
    Alternate,
    AlternateReverse,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualAnimationFillMode {
    None,
    Backwards,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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

/// Whether the content the tree's animations move can reach the viewport, and whether that answer
/// holds until the scene changes: it does once no finite animation is still running, since none can
/// then stop contributing on its own.
#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiAnimatedContentViewportEffect {
    pub may_affect_viewport: bool,
    pub stable_until_scene_changes: bool,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiTestStickyConstraints {
    pub scroller: u32,
    pub has_parent_sticky: bool,
    pub parent_sticky: u32,
    pub position_relative_to_scroller: FloatPoint,
    pub border_box_size: FloatSize,
    pub scrollport_size: FloatSize,
    pub containing_block_region: FloatRect,
    pub needs_parent_offset_adjustment: bool,
    pub inset_top: OptionalF32,
    pub inset_right: OptionalF32,
    pub inset_bottom: OptionalF32,
    pub inset_left: OptionalF32,
}
