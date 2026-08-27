/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

use crate::layout::used_values;
use crate::layout::used_values::OptionalCssPixelRect;
use crate::painting::display_list::commands::OptionalF32;
use crate::painting::visual_context::{ClipMode, MaskLayerOrigin, TransformDataRole};
use libgfx_rust::{
    CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, IntRect, MaskKind,
    WindingRule,
};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiSvgMaskFacts {
    pub mask_area: OptionalCssPixelRect,
    pub mask_kind: MaskKind,
    pub clip_area: OptionalCssPixelRect,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextTreeInputs {
    pub device_pixels_per_css_pixel: f64,
    pub visual_viewport_offset_x: f64,
    pub visual_viewport_offset_y: f64,
    pub visual_viewport_scale: f64,
    pub may_have_default_scroll_shift_anchor: bool,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiResolvedEffectsFilter {
    pub gfx_filter: *mut c_void,
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
    pub resolve_effects_filter: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiResolvedEffectsFilter,
    pub default_scroll_shift_anchor:
        unsafe extern "C" fn(*mut c_void, *mut c_void) -> crate::layout::node_data::NodeSlotId,
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
    pub(crate) fn resolve_effects_filter(&self, layout_node_shell: *mut c_void) -> FfiResolvedEffectsFilter {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.resolve_effects_filter)(self.context, layout_node_shell) }
    }
    pub(crate) fn default_scroll_shift_anchor(
        &self,
        layout_node_shell: *mut c_void,
    ) -> crate::layout::node_data::NodeSlotId {
        // SAFETY: The C++ host answers synchronously from a live layout node shell.
        unsafe { (self.default_scroll_shift_anchor)(self.context, layout_node_shell) }
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
    AnchorScrollShift,
    Mask,
    Sticky,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiVisualContextNodeExport {
    pub kind: FfiVisualContextNodeKind,
    pub parent: u32,
    pub spatial: u32,
    pub matrix: FloatMatrix4x4,
    pub origin: FloatPoint,
    pub flattens_inherited_transform: bool,
    pub transform_role: TransformDataRole,
    pub has_sorting_context_root: bool,
    pub synthetic_plane: bool,
    pub rect: IntRect,
    pub corner_radii: CornerRadii,
    pub clip_rect: FloatRect,
    pub clip_mode: ClipMode,
    pub opacity: f32,
    pub blend_mode: CompositingAndBlendingOperator,
    pub filter: *mut c_void,
    pub filter_bytes: *const u8,
    pub filter_bytes_length: usize,
    pub path: *mut c_void,
    pub winding_rule: WindingRule,
    pub mask_kind: MaskKind,
    pub mask_origin: MaskLayerOrigin,
    pub index_value: u32,
    pub sticky_parent_sticky_index: u32,
    pub sticky_position_relative_to_scroller: FloatPoint,
    pub sticky_border_box_size: FloatSize,
    pub sticky_scrollport_size: FloatSize,
    pub sticky_containing_block_region: FloatRect,
    pub sticky_needs_parent_offset_adjustment: bool,
    pub sticky_inset_top: OptionalF32,
    pub sticky_inset_right: OptionalF32,
    pub sticky_inset_bottom: OptionalF32,
    pub sticky_inset_left: OptionalF32,
    pub negate: bool,
    pub compensate_horizontal_scroll: bool,
    pub compensate_vertical_scroll: bool,
}
