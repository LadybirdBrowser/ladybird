/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use crate::layout::used_values;
use crate::layout::used_values::OptionalCssPixelRect;
use crate::painting::display_list::commands::OptionalF32;
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, MaskKind};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgMaskFacts {
    pub mask_area: OptionalCssPixelRect,
    pub mask_kind: MaskKind,
    pub clip_area: OptionalCssPixelRect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextBoxDirtyKind {
    StyleValueChange,
    StyleStructuralChange,
    ScrollableOverflowFlipped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextGlobalRebuildReason {
    FirstBuild,
    DocumentWideStructuralChange,
    FilterResourcesChanged,
    ForcedForTesting,
    CanonicalDumpRequested,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextBoxNodeList {
    SpatialNodes,
    FrameNodes,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiVisualContextUpdateOutcome {
    pub performed_full_build: bool,
    pub structural_epoch_changed: bool,
    pub requires_display_list_recording: bool,
    pub structural_epoch: u64,
}

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
pub struct FfiResolvedEffectsFilter {
    pub has_filter: bool,
    pub svg_filter_bounds: OptionalCssPixelRect,
}

pub(crate) struct ResolvedEffectsFilter {
    pub filter_bytes: Option<Vec<u8>>,
    pub svg_filter_bounds: OptionalCssPixelRect,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiVisualContextHostCallbacks {
    pub context: *mut c_void,
    pub tree_inputs: unsafe extern "C" fn(*mut c_void) -> FfiVisualContextTreeInputs,
    pub scroll_offset: unsafe extern "C" fn(*mut c_void, *mut c_void) -> used_values::FfiCssPixelPoint,
    pub svg_additional_element_transform:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut libgfx_rust::AffineTransform) -> bool,
    pub root_background_source: unsafe extern "C" fn(*mut c_void) -> FfiRootBackgroundSource,
    pub svg_mask_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiSvgMaskFacts,
    pub resolve_effects_filter: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void) -> FfiResolvedEffectsFilter,
}

impl FfiVisualContextHostCallbacks {
    pub(crate) fn tree_inputs(&self) -> FfiVisualContextTreeInputs {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.tree_inputs)(self.context) }
    }
    pub(crate) fn scroll_offset(&self, layout_node_shell: *mut c_void) -> used_values::FfiCssPixelPoint {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.scroll_offset)(self.context, layout_node_shell) }
    }
    pub(crate) fn svg_additional_element_transform(
        &self,
        layout_node_shell: *mut c_void,
    ) -> Option<libgfx_rust::AffineTransform> {
        let mut transform = libgfx_rust::AffineTransform::default();
        // SAFETY: The C++ host writes the transform synchronously when it returns true.
        let has_transform =
            unsafe { (self.svg_additional_element_transform)(self.context, layout_node_shell, &raw mut transform) };
        has_transform.then_some(transform)
    }
    pub(crate) fn root_background_source(&self) -> FfiRootBackgroundSource {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.root_background_source)(self.context) }
    }
    pub(crate) fn svg_mask_facts(&self, layout_node_shell: *mut c_void) -> FfiSvgMaskFacts {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.svg_mask_facts)(self.context, layout_node_shell) }
    }
    pub(crate) fn resolve_effects_filter(&self, layout_node_shell: *mut c_void) -> ResolvedEffectsFilter {
        let mut bytes: Vec<u8> = Vec::new();
        // SAFETY: The C++ host answers synchronously from a live layout node shell and only writes
        // into the Vec whose pointer it receives.
        let resolved =
            unsafe { (self.resolve_effects_filter)(self.context, layout_node_shell, (&raw mut bytes).cast()) };
        ResolvedEffectsFilter {
            filter_bytes: resolved.has_filter.then_some(bytes),
            svg_filter_bounds: resolved.svg_filter_bounds,
        }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualViewportTransform {
    pub matrix: FloatMatrix4x4,
    pub origin: FloatPoint,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFrameOpacitySample {
    pub frame: u32,
    pub opacity: f32,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFrameBackgroundColorSample {
    pub frame: u32,
    pub color: libgfx_rust::Color,
}

#[derive(Clone, Copy, Debug)]
#[repr(u8)]
pub enum FfiVisualAnimationTargetKind {
    Opacity,
    BackgroundColor,
    Filter,
    Transform,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFrameFilterSample {
    pub frame: u32,
    pub filter_bytes: *const u8,
    pub filter_size: usize,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiSpatialTransformSample {
    pub spatial: u32,
    pub matrix: FloatMatrix4x4,
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
