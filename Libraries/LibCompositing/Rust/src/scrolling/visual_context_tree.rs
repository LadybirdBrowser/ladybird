/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The visual context tree as the scroll and input logic reads it, under the method names of the C++ facade.

use super::{ContextRef, DisplayList, ScrollStateSnapshot, SpatialNodeIndex};
use crate::visual_context::VisualContextTree;
use libgfx_rust::{FloatPoint, FloatRect, IntRect};
use std::sync::Arc;

pub use crate::visual_context::ffi_types::FfiVisualViewportTransform as TransformWithOrigin;
pub use crate::visual_context::{ClipBehavior, IncludeVisualViewportTransform};

#[derive(Clone)]
pub struct AccumulatedVisualContextTree(Arc<VisualContextTree>);

impl AccumulatedVisualContextTree {
    pub fn from_arc(tree: Arc<VisualContextTree>) -> Self {
        Self(tree)
    }

    pub fn as_arc(&self) -> &Arc<VisualContextTree> {
        &self.0
    }

    pub fn structural_epoch(&self) -> u64 {
        self.0.structural_epoch
    }

    // Node counts cover all slots, live or dead, or only live slots.
    pub fn spatial_node_count(&self) -> usize {
        self.0.spatial_nodes.len()
    }

    pub fn context_is_valid(&self, context: ContextRef) -> bool {
        self.0.context_is_valid(context)
    }

    pub fn has_every_node_the_runs_of_display_list_name(&self, display_list: &DisplayList) -> bool {
        self.0
            .display_list_references_only_live_nodes(display_list.storage.command_runs())
    }

    pub fn visual_viewport_transform(&self) -> TransformWithOrigin {
        let crate::visual_context::SpatialData::Transform(transform) =
            &self.0.spatial_nodes[crate::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX.0 as usize].data
        else {
            unreachable!("the visual viewport node is a transform");
        };
        TransformWithOrigin {
            matrix: transform.matrix,
            origin: transform.origin,
        }
    }

    pub fn with_visual_viewport_transform(&self, transform: &TransformWithOrigin) -> Self {
        let mut copy = self.0.as_ref().clone();
        copy.set_visual_viewport_matrix_and_origin(transform.matrix, transform.origin);
        Self(Arc::new(copy))
    }

    pub fn has_visual_animations(&self) -> bool {
        self.0.has_visual_animations()
    }

    pub fn has_active_visual_animation_at(&self, monotonic_time_ns: i64) -> bool {
        self.0.has_active_visual_animation_at(monotonic_time_ns)
    }

    // Whether the point lies in the bounds of a node that a transform animation running at the time moves, through the
    // values this tree holds: those of a sample at the time, where a frame shows it.
    pub fn running_transform_animation_covers_point(
        &self,
        monotonic_time_ns: i64,
        screen_point: FloatPoint,
        scroll_state: &ScrollStateSnapshot,
    ) -> bool {
        self.0
            .running_transform_animation_covers_point(monotonic_time_ns, screen_point, scroll_state.device_offsets())
    }

    // One flag per spatial node: whether a transform animation moves the node or an ancestor.
    pub fn spatial_nodes_in_subtrees_of_transform_animations(&self) -> Vec<bool> {
        self.0.spatial_nodes_in_subtrees_of_transform_animations()
    }

    // A copy whose nodes carry the values the animations take at the time.
    pub fn with_visual_animation_samples(&self, monotonic_time_ns: i64) -> Self {
        Self(Arc::new(self.0.with_visual_animation_samples(monotonic_time_ns)))
    }

    pub fn transform_point_for_hit_test(
        &self,
        context: ContextRef,
        screen_point: FloatPoint,
        scroll_state: &ScrollStateSnapshot,
        clip_behavior: ClipBehavior,
    ) -> Option<FloatPoint> {
        self.0.transform_point_for_hit_test(
            &scroll_state.world_transforms(self),
            context,
            screen_point,
            clip_behavior,
        )
    }

    pub fn transform_rect_to_viewport(
        &self,
        index: SpatialNodeIndex,
        source_rect: FloatRect,
        scroll_state: &ScrollStateSnapshot,
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatRect {
        scroll_state.world_transforms(self).transform_rect_to_viewport(
            index,
            source_rect,
            include_visual_viewport_transform,
        )
    }
}

// Fills the snapshot entries of the tree's sticky nodes from the scroll containers' entries.
pub fn resolve_sticky_offsets(tree: &AccumulatedVisualContextTree, scroll_state: &mut ScrollStateSnapshot) {
    let resolved_sticky_entries = tree.0.resolve_sticky_offsets(scroll_state.device_offsets());
    for (node_index, offset) in resolved_sticky_entries {
        scroll_state.set_device_offset_for_index(node_index, offset);
    }
}

pub struct AnimatedContentViewportEffect {
    pub may_affect_viewport: bool,
    // The answer holds until the scene changes: no finite animation is still running, so none can stop
    // contributing on its own.
    pub stable_until_scene_changes: bool,
}

impl Default for AnimatedContentViewportEffect {
    fn default() -> Self {
        Self {
            may_affect_viewport: true,
            stable_until_scene_changes: false,
        }
    }
}

// Whether the content the tree's animations move at the sample time can reach the viewport.
pub fn animated_content_may_affect_viewport(
    display_list: &DisplayList,
    visual_context_tree: &AccumulatedVisualContextTree,
    scroll_state: &ScrollStateSnapshot,
    viewport_rect: IntRect,
    sample_time_ns: i64,
) -> AnimatedContentViewportEffect {
    let effect = crate::display_list::damage::animated_content_may_affect_viewport_at(
        display_list.storage.bytes(),
        display_list.storage.command_runs(),
        visual_context_tree.as_arc(),
        scroll_state.device_offsets(),
        viewport_rect,
        sample_time_ns,
    );
    AnimatedContentViewportEffect {
        may_affect_viewport: effect.may_affect_viewport,
        stable_until_scene_changes: effect.stable_until_scene_changes,
    }
}
