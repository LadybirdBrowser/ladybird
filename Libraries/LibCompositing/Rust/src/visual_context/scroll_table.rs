/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{SpatialData, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree};
use crate::css_pixels::{FfiCssPixelPoint, FfiCssPixelRect};
use crate::display_list::commands::UniqueNodeId;
use crate::ffi_enum_bytes;
use libgfx_rust::{FloatPoint, IntRect};
use std::sync::Arc;

// The values of SnapStrictness and SnapAlign in scrolling::scroll_snap_selection.
pub(crate) const SNAP_STRICTNESS_COUNT: u8 = 3;
pub(crate) const SNAP_ALIGN_COUNT: u8 = 4;

#[derive(Clone, Copy, PartialEq, Eq)]
enum ParentChainMark {
    Unvisited,
    OnTheChainBeingWalked,
    KnownToEndAtARoot,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum CompositorScrollNodeKind {
    Viewport,
    #[default]
    Element,
    PseudoElement,
}
ffi_enum_bytes!(CompositorScrollNodeKind as u8);

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct CompositorScrollNodeRecord {
    pub scrollable_node_id: UniqueNodeId,
    pub scroll_node_index: SpatialNodeIndex,
    pub parent_scroll_node_index: SpatialNodeIndex,
    pub scrollport_rect: IntRect,
    pub min_scroll_offset: FloatPoint,
    pub max_scroll_offset: FloatPoint,
    pub scroll_node_kind: CompositorScrollNodeKind,
    pub pseudo_element_type: u8,
    pub is_viewport: bool,
    pub can_be_wheel_scrolled_horizontally: bool,
    pub can_be_wheel_scrolled_vertically: bool,
    pub chains_scroll_to_ancestors_horizontally: bool,
    pub chains_scroll_to_ancestors_vertically: bool,
    pub shows_overscroll_effect_horizontally: bool,
    pub shows_overscroll_effect_vertically: bool,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct CompositorSnapContainerRecord {
    pub scroll_node_index: SpatialNodeIndex,
    pub snapport: FfiCssPixelRect,
    pub min_scroll_offset: FfiCssPixelPoint,
    pub max_scroll_offset: FfiCssPixelPoint,
    pub strictness: u8,
    pub snaps_x: bool,
    pub snaps_y: bool,
    pub horizontal_writing_mode: bool,
    pub first_area_index: u32,
    pub area_count: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct CompositorSnapAreaRecord {
    pub area_node_id: UniqueNodeId,
    pub pseudo_element_type: u8,
    pub rect: FfiCssPixelRect,
    pub align_x: u8,
    pub align_y: u8,
    pub always_stop: bool,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct CompositorScrollTable {
    pub nodes: Arc<[CompositorScrollNodeRecord]>,
    pub snap_containers: Arc<[CompositorSnapContainerRecord]>,
    pub snap_areas: Arc<[CompositorSnapAreaRecord]>,
}

impl CompositorScrollTable {
    pub(super) fn is_valid_for_tree(&self, tree: &VisualContextTree) -> bool {
        if !self
            .nodes
            .windows(2)
            .all(|pair| pair[0].scroll_node_index < pair[1].scroll_node_index)
            || self.nodes.iter().filter(|node| node.is_viewport).count() > 1
            || !self.nodes.iter().all(|node| {
                tree.spatial_nodes
                    .get(node.scroll_node_index.0 as usize)
                    .is_some_and(|spatial| matches!(spatial.data, SpatialData::Scroll(_)))
                    && [
                        node.min_scroll_offset.x,
                        node.min_scroll_offset.y,
                        node.max_scroll_offset.x,
                        node.max_scroll_offset.y,
                    ]
                    .iter()
                    .all(|value| value.is_finite())
            })
        {
            return false;
        }

        let row_for_index = |index| {
            self.nodes
                .binary_search_by_key(&index, |node| node.scroll_node_index)
                .ok()
        };
        let mut parents = Vec::with_capacity(self.nodes.len());
        for node in self.nodes.iter() {
            parents.push(if node.parent_scroll_node_index == VISUAL_VIEWPORT_NODE_INDEX {
                None
            } else if let Some(parent) = row_for_index(node.parent_scroll_node_index) {
                Some(parent)
            } else {
                return false;
            });
        }
        let mut marks = vec![ParentChainMark::Unvisited; self.nodes.len()];
        let mut chain = Vec::new();
        for row in 0..self.nodes.len() {
            let mut current = Some(row);
            while let Some(index) = current {
                match marks[index] {
                    ParentChainMark::OnTheChainBeingWalked => return false,
                    ParentChainMark::KnownToEndAtARoot => break,
                    ParentChainMark::Unvisited => {}
                }
                marks[index] = ParentChainMark::OnTheChainBeingWalked;
                chain.push(index);
                current = parents[index];
            }
            for index in chain.drain(..) {
                marks[index] = ParentChainMark::KnownToEndAtARoot;
            }
        }

        if !self
            .snap_containers
            .windows(2)
            .all(|pair| pair[0].scroll_node_index < pair[1].scroll_node_index)
        {
            return false;
        }
        let mut next_area_index = 0usize;
        for container in self.snap_containers.iter() {
            if row_for_index(container.scroll_node_index).is_none()
                || container.strictness >= SNAP_STRICTNESS_COUNT
                || container.first_area_index as usize != next_area_index
            {
                return false;
            }
            let Some(end) = next_area_index.checked_add(container.area_count as usize) else {
                return false;
            };
            if end > self.snap_areas.len() {
                return false;
            }
            next_area_index = end;
        }
        next_area_index == self.snap_areas.len()
            && self
                .snap_areas
                .iter()
                .all(|area| area.align_x < SNAP_ALIGN_COUNT && area.align_y < SNAP_ALIGN_COUNT)
    }
}
