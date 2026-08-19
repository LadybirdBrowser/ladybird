/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgMaskFacts {
    pub has_mask_area: bool,
    pub mask_area: crate::layout::FfiCssPixelRect,
    pub mask_kind: i32,
    pub has_clip_area: bool,
    pub clip_area: crate::layout::FfiCssPixelRect,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextTreeInputs {
    pub device_pixels_per_css_pixel: f64,
    pub visual_viewport_offset_x: f64,
    pub visual_viewport_offset_y: f64,
    pub visual_viewport_scale: f64,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiVisualContextHostCallbacks {
    pub context: *mut c_void,
    pub tree_inputs: unsafe extern "C" fn(*mut c_void) -> FfiVisualContextTreeInputs,
    pub scroll_offset: unsafe extern "C" fn(*mut c_void, *mut c_void) -> crate::layout::FfiCssPixelPoint,
    pub svg_transform_view_box_rect:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut crate::layout::FfiCssPixelRect) -> bool,
    pub svg_additional_element_transform: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut f32) -> bool,
    pub root_background_source: unsafe extern "C" fn(*mut c_void) -> FfiRootBackgroundSource,
    pub svg_mask_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiSvgMaskFacts,
    pub resolve_effects_filter: unsafe extern "C" fn(*mut c_void, *mut c_void) -> *mut c_void,
    pub default_scroll_shift_anchor:
        unsafe extern "C" fn(*mut c_void, *mut c_void) -> crate::layout::node_data::NodeSlotId,
}

impl FfiVisualContextHostCallbacks {
    pub(crate) fn tree_inputs(&self) -> FfiVisualContextTreeInputs {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.tree_inputs)(self.context) }
    }
    pub(crate) fn scroll_offset(&self, paintable_shell: *mut c_void) -> crate::layout::FfiCssPixelPoint {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.scroll_offset)(self.context, paintable_shell) }
    }
    pub(crate) fn svg_transform_view_box_rect(
        &self,
        paintable_shell: *mut c_void,
    ) -> Option<crate::layout::FfiCssPixelRect> {
        let mut rect = crate::layout::FfiCssPixelRect::default();
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.svg_transform_view_box_rect)(self.context, paintable_shell, &raw mut rect) }.then_some(rect)
    }
    pub(crate) fn svg_additional_element_transform(
        &self,
        paintable_shell: *mut c_void,
    ) -> Option<libgfx_rust::AffineTransform> {
        let mut values = [0.0f32; 6];
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.svg_additional_element_transform)(self.context, paintable_shell, values.as_mut_ptr()) }
            .then_some(libgfx_rust::AffineTransform { values })
    }
    pub(crate) fn root_background_source(&self) -> FfiRootBackgroundSource {
        // SAFETY: The C++ host answers synchronously.
        unsafe { (self.root_background_source)(self.context) }
    }
    pub(crate) fn svg_mask_facts(&self, paintable_shell: *mut c_void) -> FfiSvgMaskFacts {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.svg_mask_facts)(self.context, paintable_shell) }
    }
    pub(crate) fn resolve_effects_filter(&self, paintable_shell: *mut c_void) -> *mut c_void {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.resolve_effects_filter)(self.context, paintable_shell) }
    }
    pub(crate) fn default_scroll_shift_anchor(
        &self,
        paintable_shell: *mut c_void,
    ) -> crate::layout::node_data::NodeSlotId {
        // SAFETY: The C++ host answers synchronously from a live paintable shell.
        unsafe { (self.default_scroll_shift_anchor)(self.context, paintable_shell) }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiVisualContextNodeKind {
    Scroll,
    Clip,
    Transform,
    Perspective,
    BackfaceVisibility,
    ClipPath,
    Effects,
    ScrollCompensation,
    AnchorScrollShift,
    Mask,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextNodeExport {
    pub kind: FfiVisualContextNodeKind,
    pub parent_index: usize,
    pub matrix: [f32; 16],
    pub origin: [f32; 2],
    pub flattens_inherited_transform: bool,
    pub transform_role: u8,
    pub rect: [i32; 4],
    pub corner_radii: [i32; 8],
    pub opacity: f32,
    pub blend_mode: i32,
    pub filter: *mut c_void,
    pub path: *mut c_void,
    pub winding_rule: i32,
    pub mask_kind: i32,
    pub mask_origin: u8,
    pub index_value: usize,
    pub is_sticky: bool,
    pub state_slot: usize,
    pub negate: bool,
    pub compensate_horizontal_scroll: bool,
    pub compensate_vertical_scroll: bool,
}
