/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::layout::used_values;
use crate::painting::display_list::commands::ContextRef;
use std::ffi::c_void;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiHitTestQueryCallbacks {
    pub device_pixels_per_css_pixel: f64,
    pub scroll_offsets: *const libgfx_rust::FloatPoint,
    pub scroll_offsets_len: usize,
    /// Private: the world transforms a Rust entry point builds for the query, from the tree it reads and the offsets.
    world_transforms: *const c_void,
    pub has_chrome_metrics: bool,
    pub chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    /// Where set, the query reads the compositor's animations at this time, as a frame of the compositor's shows them.
    pub has_visual_animation_sample_time: bool,
    pub visual_animation_sample_time_ns: i64,
}

impl FfiHitTestQueryCallbacks {
    /// Callbacks for a point query on the render owner, which reaches nothing of the host: the scroll offsets and
    /// chrome metrics are copies the host sealed.
    pub(crate) fn sealed(
        device_pixels_per_css_pixel: f64,
        scroll_offsets: &[libgfx_rust::FloatPoint],
        chrome_metrics: crate::painting::ffi::FfiChromeMetrics,
    ) -> Self {
        Self {
            device_pixels_per_css_pixel,
            scroll_offsets: scroll_offsets.as_ptr(),
            scroll_offsets_len: scroll_offsets.len(),
            world_transforms: std::ptr::null(),
            has_chrome_metrics: true,
            chrome_metrics,
            has_visual_animation_sample_time: false,
            visual_animation_sample_time_ns: 0,
        }
    }

    pub(crate) fn visual_animation_sample_time_ns(&self) -> Option<i64> {
        self.has_visual_animation_sample_time
            .then_some(self.visual_animation_sample_time_ns)
    }

    /// The world transforms of `tree`, the tree the query reads, under these callbacks' offsets.
    pub(crate) fn build_world_transforms(
        &self,
        tree: &crate::painting::visual_context::VisualContextTree,
    ) -> crate::painting::visual_context::WorldTransforms {
        crate::painting::visual_context::WorldTransforms::new(tree, self.scroll_offsets(), None)
    }

    /// These callbacks with the world transforms the query reads, which `world` holds for the query.
    pub(crate) fn with_world_transforms(mut self, world: &crate::painting::visual_context::WorldTransforms) -> Self {
        self.world_transforms = std::ptr::from_ref(world).cast();
        self
    }

    /// The mapping between the viewport and the spaces of `tree`, the tree the query reads, under these callbacks'
    /// offsets.
    pub(crate) fn viewport_mapping<'a>(
        &'a self,
        tree: &'a crate::painting::visual_context::VisualContextTree,
    ) -> crate::painting::viewport_mapping::ViewportMapping<'a> {
        crate::painting::viewport_mapping::ViewportMapping::new(
            tree,
            self.world_transforms(),
            self.scroll_offsets(),
            self.device_pixels_per_css_pixel as f32,
        )
    }

    pub(crate) fn world_transforms(&self) -> &crate::painting::visual_context::WorldTransforms {
        assert!(
            !self.world_transforms.is_null(),
            "the query's entry point builds its world transforms"
        );
        // SAFETY: The entry point keeps the table alive for the synchronous query.
        unsafe { libcompositing_rust::ffi::world_transforms_from_handle(self.world_transforms) }
    }

    pub(crate) fn scroll_offsets(&self) -> &[libgfx_rust::FloatPoint] {
        if self.scroll_offsets.is_null() {
            return &[];
        }
        // SAFETY: QueryContext keeps the document and its resolved scroll snapshot alive for the synchronous query.
        unsafe { ak::slice_from_raw_parts(self.scroll_offsets, self.scroll_offsets_len) }
    }
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiTopmostItem {
    pub has_item: bool,
    pub index: usize,
    pub local: used_values::FfiCssPixelPoint,
}

pub use crate::layout::node_data::FfiNodeIdentity;

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiResolvedHit {
    /// The node the hit dispatches its events to, and the node it falls back to when there is
    /// none.
    pub dispatch: FfiNodeIdentity,
    pub fallback_dispatch: FfiNodeIdentity,
    pub has_index_in_node: bool,
    pub index_in_node: usize,
    pub is_text_fragment: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiCaretBoundaryKind {
    #[default]
    Offset = 0,
    BeforeNode = 1,
    AfterNode = 2,
    IndexOfNodeInParent = 3,
}

#[derive(Clone, Copy, Debug, Default)]
#[repr(C)]
pub struct FfiResolvedCaret {
    pub has_position: bool,
    pub node: FfiNodeIdentity,
    pub boundary: FfiCaretBoundaryKind,
    pub offset: usize,
    pub affinity_is_upstream: bool,
    pub has_debug_rect: bool,
    pub debug_rect: used_values::FfiCssPixelRect,
}

/// A caret position a query resolved, and the paintable of the item it resolved on.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiCaretAt {
    pub paintable: NodeSlotId,
    pub caret: FfiResolvedCaret,
}

impl FfiCaretAt {
    pub(crate) fn none() -> Self {
        Self {
            paintable: NodeSlotId::INVALID,
            caret: FfiResolvedCaret::default(),
        }
    }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiHitTestItemExport {
    pub paintable: NodeSlotId,
    pub hit_node: NodeSlotId,
    pub chrome_widget_kind: u8,
    pub context: ContextRef,
}
