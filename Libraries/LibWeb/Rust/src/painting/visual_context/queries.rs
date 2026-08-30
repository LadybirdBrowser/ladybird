/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    ClipData, ClipMode, ContextRef, FrameData, FrameNode, FrameNodeIndex, IncludeVisualViewportTransform, SpatialData,
    SpatialNodeIndex, TransformDataRole, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree, device_offset_for_index,
};
use libgfx_rust::{
    CompositingAndBlendingOperator, FloatMatrix4x4, FloatPoint, FloatRect, IntPoint, IntRect, map_rect_through_matrix,
};

// Homogeneous coordinates this close to the eye plane have no meaningful projection.
const MINIMUM_PROJECTION_W: f32 = 0.0001;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ClipBehavior {
    Respect,
    // Transform the point without rejecting it against clip rects and clip paths. Used when searching for the
    // closest caret position within a scope the point may lie entirely outside of.
    Ignore,
}

impl ClipBehavior {
    pub fn from_respect_clip(respect_clip: bool) -> Self {
        if respect_clip { Self::Respect } else { Self::Ignore }
    }
}

pub fn should_cull_back_face(accumulated_matrix: FloatMatrix4x4, plane_root_matrix: FloatMatrix4x4) -> bool {
    let Some(inverse_plane_root_matrix) = plane_root_matrix.flattened().inverse() else {
        return false;
    };
    inverse_plane_root_matrix
        .multiplied(accumulated_matrix)
        .is_back_face_visible()
}

impl ClipData {
    pub fn contains(&self, point: FloatPoint) -> bool {
        let integral_rect = IntRect::new(
            self.rect.x as i32,
            self.rect.y as i32,
            self.rect.width as i32,
            self.rect.height as i32,
        );
        if integral_rect.to_float() == self.rect {
            return self.corner_radii.contains_int_point(
                IntPoint {
                    x: point.x as i32,
                    y: point.y as i32,
                },
                integral_rect,
            );
        }
        self.corner_radii.contains_float_point(point, self.rect)
    }
}

// One root path of spatial nodes with the screen point mapped down it one inverse step at a time. Frames attach
// to a path in non-decreasing depth order, so the steps still to apply before a frame always start where the
// previous frame left off.
struct SpatialChainWalk {
    chain: Vec<SpatialNodeIndex>,
    needs_accumulated_matrices: bool,
    has_3d_transform: bool,
    accumulated_matrices: Vec<FloatMatrix4x4>,
    point: FloatPoint,
    applied_steps: usize,
}

impl VisualContextTree {
    pub fn fill_frames_with_empty_effective_clip(&self, empty: &mut Vec<bool>) {
        empty.clear();
        empty.reserve(self.frame_nodes.len());
        for node in &self.frame_nodes {
            let parent_is_empty = !node.parent.is_none() && empty[node.parent.0 as usize];
            empty.push(node.clips_everything || parent_is_empty);
        }
    }

    pub fn frames_with_empty_effective_clip(&self) -> Vec<bool> {
        let mut empty = Vec::new();
        self.fill_frames_with_empty_effective_clip(&mut empty);
        empty
    }

    fn chain_contains_3d_transform(&self, index: SpatialNodeIndex) -> bool {
        let mut current = index;
        loop {
            let node = &self.spatial_nodes[current.0 as usize];
            match &node.data {
                SpatialData::Transform(transform) => {
                    if !transform.matrix.is_2d_affine() {
                        return true;
                    }
                }
                SpatialData::Perspective(_) => return true,
                _ => {}
            }
            if current == VISUAL_VIEWPORT_NODE_INDEX {
                break;
            }
            current = node.parent;
        }
        false
    }

    fn begin_hit_test_walk(&self, leaf: SpatialNodeIndex, screen_point: FloatPoint) -> SpatialChainWalk {
        let mut chain = self.ancestor_chain(leaf);
        chain.reverse();
        let chain_has_backface_marker = chain.iter().any(|index| {
            matches!(
                self.spatial_nodes[index.0 as usize].data,
                SpatialData::BackfaceVisibility(_)
            )
        });
        let has_3d_transform = self.chain_contains_3d_transform(leaf);
        let needs_accumulated_matrices = chain_has_backface_marker || has_3d_transform;
        let accumulated_matrices = if needs_accumulated_matrices {
            Vec::with_capacity(chain.len())
        } else {
            Vec::new()
        };
        SpatialChainWalk {
            chain,
            needs_accumulated_matrices,
            has_3d_transform,
            accumulated_matrices,
            point: screen_point,
            applied_steps: 0,
        }
    }

    fn apply_hit_test_spatial_step(
        &self,
        walk: &mut SpatialChainWalk,
        position: usize,
        screen_point: FloatPoint,
        scroll_offsets: &[FloatPoint],
    ) -> bool {
        let node_index = walk.chain[position];
        let node = &self.spatial_nodes[node_index.0 as usize];

        if walk.needs_accumulated_matrices {
            let local = self.local_spatial_matrix(node_index, scroll_offsets);
            if position == 0 {
                walk.accumulated_matrices.push(local.matrix);
            } else {
                let parent_matrix = *walk.accumulated_matrices.last().expect("walk has a parent matrix");
                let inherited = if local.flattens_inherited_transform {
                    parent_matrix.flattened()
                } else {
                    parent_matrix
                };
                walk.accumulated_matrices.push(inherited.multiplied(local.matrix));
            }
        }

        if walk.has_3d_transform && !matches!(node.data, SpatialData::BackfaceVisibility(_)) {
            let Some(inverse) = walk
                .accumulated_matrices
                .last()
                .expect("3D walk accumulates matrices")
                .flattened()
                .inverse()
            else {
                return false;
            };
            let mapped = inverse.map_vector4([screen_point.x, screen_point.y, 0.0, 1.0]);
            if mapped[3] < MINIMUM_PROJECTION_W {
                return false;
            }
            walk.point = FloatPoint {
                x: mapped[0] / mapped[3],
                y: mapped[1] / mapped[3],
            };
            return true;
        }

        match &node.data {
            SpatialData::Perspective(perspective) => {
                let Some(inverse) = perspective.matrix.extract_2d_affine().inverse() else {
                    return false;
                };
                walk.point = inverse.map_point(walk.point);
                true
            }
            SpatialData::BackfaceVisibility(backface) => {
                let plane_root_position = walk
                    .chain
                    .iter()
                    .position(|index| *index == backface.plane_root_index)
                    .expect("the plane root precedes the backface marker on its root path");
                !should_cull_back_face(
                    *walk
                        .accumulated_matrices
                        .last()
                        .expect("backface walk accumulates matrices"),
                    walk.accumulated_matrices[plane_root_position],
                )
            }
            SpatialData::Scroll(_) | SpatialData::Sticky(_) => {
                let offset = device_offset_for_index(scroll_offsets, node_index);
                walk.point = FloatPoint {
                    x: walk.point.x - offset.x,
                    y: walk.point.y - offset.y,
                };
                true
            }
            SpatialData::Transform(transform) => {
                let Some(inverse) = transform.matrix.extract_2d_affine().inverse() else {
                    return false;
                };
                let offset_point = FloatPoint {
                    x: walk.point.x - transform.origin.x,
                    y: walk.point.y - transform.origin.y,
                };
                let transformed = inverse.map_point(offset_point);
                walk.point = FloatPoint {
                    x: transformed.x + transform.origin.x,
                    y: transformed.y + transform.origin.y,
                };
                true
            }
            SpatialData::AnchorScrollShift(shift) => {
                let offset = shift.masked_offset(scroll_offsets);
                walk.point = FloatPoint {
                    x: walk.point.x - offset.x,
                    y: walk.point.y - offset.y,
                };
                true
            }
        }
    }

    fn apply_hit_test_spatial_steps_through_node(
        &self,
        walk: &mut SpatialChainWalk,
        node_index: SpatialNodeIndex,
        screen_point: FloatPoint,
        scroll_offsets: &[FloatPoint],
    ) -> bool {
        if walk.applied_steps > 0 && walk.chain[walk.applied_steps - 1] == node_index {
            return true;
        }
        loop {
            assert!(walk.applied_steps < walk.chain.len());
            if !self.apply_hit_test_spatial_step(walk, walk.applied_steps, screen_point, scroll_offsets) {
                return false;
            }
            if walk.chain[walk.applied_steps] == node_index {
                walk.applied_steps += 1;
                return true;
            }
            walk.applied_steps += 1;
        }
    }

    fn point_passes_frame(frame: &FrameNode, point: FloatPoint, clip_behavior: ClipBehavior) -> bool {
        if clip_behavior == ClipBehavior::Ignore {
            return true;
        }
        match &frame.data {
            FrameData::Clip(clip) => {
                // NOTE: The clip rect is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                let inside = clip.contains(point);
                if clip.mode == ClipMode::Intersect {
                    inside
                } else {
                    !inside
                }
            }
            FrameData::ClipPath(clip_path) => {
                // NOTE: The clip path is in absolute device-pixel coordinates. After inverse-transforming, `point`
                //       is also in device-pixel coordinates, so we compare them directly.
                if !clip_path.bounding_rect.contains_point(IntPoint {
                    x: point.x as i32,
                    y: point.y as i32,
                }) {
                    return false;
                }
                clip_path.path.contains(point.x, point.y, clip_path.fill_rule as i32)
            }
            FrameData::Effects(_) | FrameData::Mask(_) => true,
        }
    }

    pub fn transform_point_for_hit_test(
        &self,
        context: ContextRef,
        screen_point: FloatPoint,
        scroll_offsets: &[FloatPoint],
        clip_behavior: ClipBehavior,
    ) -> Option<FloatPoint> {
        let mut frame_chain: Vec<FrameNodeIndex> = Vec::with_capacity(8);
        let mut frame = context.frame;
        while frame != FrameNodeIndex::NONE {
            frame_chain.push(frame);
            frame = self.frame_nodes[frame.0 as usize].parent;
        }
        frame_chain.reverse();

        // The backface test needs forward matrices, but this walk only applies inverses. When the chain contains
        // backface markers, we accumulate the forward matrices as we walk, from the root down, so a marker can look up
        // the matrix at its plane root by chain position.
        let mut context_walk = self.begin_hit_test_walk(context.spatial, screen_point);
        for frame_index in frame_chain {
            let frame = &self.frame_nodes[frame_index.0 as usize];
            let mut frame_walk;
            let walk = if context_walk.chain.contains(&frame.spatial) {
                &mut context_walk
            } else {
                // A fixed-background context is rooted above the scroll nodes its frames record in, so such a frame
                // hangs below the context's node and takes its own walk from the root.
                frame_walk = self.begin_hit_test_walk(frame.spatial, screen_point);
                &mut frame_walk
            };
            if !self.apply_hit_test_spatial_steps_through_node(walk, frame.spatial, screen_point, scroll_offsets)
                || !Self::point_passes_frame(frame, walk.point, clip_behavior)
            {
                return None;
            }
        }
        if !self.apply_hit_test_spatial_steps_through_node(
            &mut context_walk,
            context.spatial,
            screen_point,
            scroll_offsets,
        ) {
            return None;
        }

        Some(context_walk.point)
    }

    pub fn inverse_transform_point(&self, index: SpatialNodeIndex, screen_point: FloatPoint) -> FloatPoint {
        let chain = self.ancestor_chain(index);

        // This walk deliberately skips translation-only nodes. Callers resolve offsets and scroll positions
        // themselves. The per-node inverses below are only exact for chains of 2D transforms, so a chain containing a
        // 3D transform inverts the flattened accumulated matrix, mapping the screen point onto the plane the content
        // was rendered into.
        if self.chain_contains_3d_transform(index) {
            let mut matrix = FloatMatrix4x4::identity();
            for node_index in chain.iter().rev() {
                match &self.spatial_nodes[node_index.0 as usize].data {
                    SpatialData::Transform(transform) => {
                        let inherited = if transform.flattens_inherited_transform {
                            matrix.flattened()
                        } else {
                            matrix
                        };
                        matrix = inherited.multiplied(transform.matrix_including_origin());
                    }
                    SpatialData::Perspective(perspective) => {
                        let inherited = if perspective.flattens_inherited_transform {
                            matrix.flattened()
                        } else {
                            matrix
                        };
                        matrix = inherited.multiplied(perspective.matrix);
                    }
                    _ => {}
                }
            }
            let Some(inverse) = matrix.flattened().inverse() else {
                return screen_point;
            };
            let mapped = inverse.map_vector4([screen_point.x, screen_point.y, 0.0, 1.0]);
            if mapped[3] < MINIMUM_PROJECTION_W {
                return screen_point;
            }
            return FloatPoint {
                x: mapped[0] / mapped[3],
                y: mapped[1] / mapped[3],
            };
        }

        let mut point = screen_point;
        for node_index in chain.iter().rev() {
            match &self.spatial_nodes[node_index.0 as usize].data {
                SpatialData::Perspective(perspective) => {
                    if let Some(inverse) = perspective.matrix.extract_2d_affine().inverse() {
                        point = inverse.map_point(point);
                    }
                }
                SpatialData::Transform(transform) => {
                    if let Some(inverse) = transform.matrix.extract_2d_affine().inverse() {
                        let offset_point = FloatPoint {
                            x: point.x - transform.origin.x,
                            y: point.y - transform.origin.y,
                        };
                        let transformed = inverse.map_point(offset_point);
                        point = FloatPoint {
                            x: transformed.x + transform.origin.x,
                            y: transformed.y + transform.origin.y,
                        };
                    }
                }
                _ => {}
            }
        }

        point
    }

    pub fn transform_rect_to_viewport(
        &self,
        index: SpatialNodeIndex,
        source_rect: FloatRect,
        scroll_offsets: &[FloatPoint],
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatRect {
        // A chain with three-dimensional transforms cannot be applied one two-dimensional projection at a time.
        if self.chain_contains_3d_transform(index) {
            return map_rect_through_matrix(
                self.accumulated_matrix(index, scroll_offsets, include_visual_viewport_transform),
                source_rect,
                MINIMUM_PROJECTION_W,
            );
        }

        let mut rect = source_rect;
        let mut current = index;
        loop {
            let node = &self.spatial_nodes[current.0 as usize];
            if current != VISUAL_VIEWPORT_NODE_INDEX
                || !self.root_is_visual_viewport
                || include_visual_viewport_transform == IncludeVisualViewportTransform::Yes
            {
                match &node.data {
                    SpatialData::Transform(transform) => {
                        let affine = transform.matrix.extract_2d_affine();
                        rect = rect.translated(-transform.origin.x, -transform.origin.y);
                        rect = affine.map_rect(rect);
                        rect = rect.translated(transform.origin.x, transform.origin.y);
                    }
                    SpatialData::Perspective(perspective) => {
                        rect = perspective.matrix.extract_2d_affine().map_rect(rect);
                    }
                    SpatialData::Scroll(_) | SpatialData::Sticky(_) => {
                        let offset = device_offset_for_index(scroll_offsets, current);
                        rect = rect.translated(offset.x, offset.y);
                    }
                    SpatialData::AnchorScrollShift(shift) => {
                        let offset = shift.masked_offset(scroll_offsets);
                        rect = rect.translated(offset.x, offset.y);
                    }
                    SpatialData::BackfaceVisibility(_) => {}
                }
            }
            if current == VISUAL_VIEWPORT_NODE_INDEX {
                break;
            }
            current = node.parent;
        }

        rect
    }

    pub fn cumulative_scroll_chain_offset(&self, index: SpatialNodeIndex, scroll_offsets: &[FloatPoint]) -> FloatPoint {
        let nearest_scroll_like_ancestor = |from: SpatialNodeIndex| -> Option<SpatialNodeIndex> {
            let mut current = from;
            while current != VISUAL_VIEWPORT_NODE_INDEX {
                current = self.spatial_nodes[current.0 as usize].parent;
                if current == VISUAL_VIEWPORT_NODE_INDEX {
                    return None;
                }
                if self.spatial_nodes[current.0 as usize].data.is_scroll_like() {
                    return Some(current);
                }
            }
            None
        };

        let mut offset = FloatPoint::default();
        let mut current = Some(index);
        while let Some(node_index) = current
            && node_index != VISUAL_VIEWPORT_NODE_INDEX
        {
            let entry = device_offset_for_index(scroll_offsets, node_index);
            offset = FloatPoint {
                x: offset.x + entry.x,
                y: offset.y + entry.y,
            };
            current = match &self.spatial_nodes[node_index.0 as usize].data {
                SpatialData::Sticky(sticky) => Some(sticky.parent_sticky.unwrap_or(sticky.scroller)),
                _ => nearest_scroll_like_ancestor(node_index),
            };
        }
        offset
    }

    pub fn resolve_sticky_offsets(&self, scroll_offsets: &[FloatPoint]) -> Vec<(SpatialNodeIndex, FloatPoint)> {
        if !self
            .spatial_nodes
            .iter()
            .any(|node| matches!(node.data, SpatialData::Sticky(_)))
        {
            return Vec::new();
        }
        let mut resolved_offsets = scroll_offsets.to_vec();
        let mut resolved_sticky_entries = Vec::new();
        let mut record_entry =
            |resolved_offsets: &mut Vec<FloatPoint>, node_index: SpatialNodeIndex, offset: FloatPoint| {
                let slot = node_index.0 as usize;
                if slot >= resolved_offsets.len() {
                    resolved_offsets.resize(slot + 1, FloatPoint::default());
                }
                resolved_offsets[slot] = offset;
                resolved_sticky_entries.push((node_index, offset));
            };
        for (i, node) in self.spatial_nodes.iter().enumerate() {
            let SpatialData::Sticky(sticky) = &node.data else {
                continue;
            };
            let node_index = SpatialNodeIndex(i as u32);
            if sticky.scroller == VISUAL_VIEWPORT_NODE_INDEX {
                record_entry(&mut resolved_offsets, node_index, FloatPoint::default());
                continue;
            }
            assert!((sticky.scroller.0 as usize) < i);

            // Sticky ancestors along the containing block chain precede this node, so their entries are
            // already resolved in this pass.
            let mut parent_sticky_offset = FloatPoint::default();
            let mut ancestor = sticky.parent_sticky;
            while let Some(ancestor_index) = ancestor {
                assert!((ancestor_index.0 as usize) < i);
                let entry = device_offset_for_index(&resolved_offsets, ancestor_index);
                parent_sticky_offset = FloatPoint {
                    x: parent_sticky_offset.x + entry.x,
                    y: parent_sticky_offset.y + entry.y,
                };
                ancestor = match &self.spatial_nodes[ancestor_index.0 as usize].data {
                    SpatialData::Sticky(ancestor_sticky) => ancestor_sticky.parent_sticky,
                    _ => panic!("a parent sticky reference names a sticky node"),
                };
            }

            let position_in_scroller = FloatPoint {
                x: sticky.position_relative_to_scroller.x + parent_sticky_offset.x,
                y: sticky.position_relative_to_scroller.y + parent_sticky_offset.y,
            };
            let mut containing_block_region = sticky.containing_block_region;
            if sticky.needs_parent_offset_adjustment {
                containing_block_region =
                    containing_block_region.translated(parent_sticky_offset.x, parent_sticky_offset.y);
            }
            let min_offset_within_containing_block = containing_block_region.top_left();
            let max_offset_within_containing_block = FloatPoint {
                x: containing_block_region.right() - sticky.border_box_size.width,
                y: containing_block_region.bottom() - sticky.border_box_size.height,
            };

            // A scroll container's entry is its negated scroll offset.
            let scroller_entry = device_offset_for_index(&resolved_offsets, sticky.scroller);
            let scrollport_rect = FloatRect::new(
                -scroller_entry.x,
                -scroller_entry.y,
                sticky.scrollport_size.width,
                sticky.scrollport_size.height,
            );

            let mut sticky_offset = FloatPoint::default();
            if let Some(inset_top) = sticky.inset_top
                && scrollport_rect.y > position_in_scroller.y - inset_top
            {
                sticky_offset.y =
                    (scrollport_rect.y + inset_top).min(max_offset_within_containing_block.y) - position_in_scroller.y;
            }
            if let Some(inset_left) = sticky.inset_left
                && scrollport_rect.x > position_in_scroller.x - inset_left
            {
                sticky_offset.x =
                    (scrollport_rect.x + inset_left).min(max_offset_within_containing_block.x) - position_in_scroller.x;
            }
            if let Some(inset_bottom) = sticky.inset_bottom
                && scrollport_rect.bottom() < position_in_scroller.y + sticky.border_box_size.height + inset_bottom
            {
                sticky_offset.y = (scrollport_rect.bottom() - sticky.border_box_size.height - inset_bottom)
                    .max(min_offset_within_containing_block.y)
                    - position_in_scroller.y;
            }
            if let Some(inset_right) = sticky.inset_right
                && scrollport_rect.right() < position_in_scroller.x + sticky.border_box_size.width + inset_right
            {
                sticky_offset.x = (scrollport_rect.right() - sticky.border_box_size.width - inset_right)
                    .max(min_offset_within_containing_block.x)
                    - position_in_scroller.x;
            }

            record_entry(&mut resolved_offsets, node_index, sticky_offset);
        }
        resolved_sticky_entries
    }
}

impl VisualContextTree {
    pub fn frame_is_isolated_by_layer_frame(&self, mut frame: FrameNodeIndex) -> bool {
        while !frame.is_none() {
            let node = &self.frame_nodes[frame.0 as usize];
            if matches!(node.data, FrameData::Effects(_) | FrameData::Mask(_)) {
                return true;
            }
            frame = node.parent;
        }
        false
    }

    pub fn has_unisolated_blending_frame(&self) -> bool {
        self.frame_nodes.iter().any(|node| {
            matches!(&node.data, FrameData::Effects(effects) if effects.blend_mode != CompositingAndBlendingOperator::Normal)
                && !self.frame_is_isolated_by_layer_frame(node.parent)
        })
    }
}

impl VisualContextTree {
    pub fn with_sampled_visual_animation_values(
        &self,
        frame_opacities: &[(FrameNodeIndex, f32)],
        spatial_matrices: &[(SpatialNodeIndex, FloatMatrix4x4)],
    ) -> VisualContextTree {
        let mut sampled = self.clone();
        for (frame, opacity) in frame_opacities {
            if let Some(FrameData::Effects(effects)) =
                sampled.frame_nodes.get_mut(frame.0 as usize).map(|node| &mut node.data)
            {
                effects.opacity = *opacity;
            }
        }
        for (spatial, matrix) in spatial_matrices {
            if let Some(SpatialData::Transform(transform)) = sampled
                .spatial_nodes
                .get_mut(spatial.0 as usize)
                .map(|node| &mut node.data)
            {
                transform.matrix = *matrix;
            }
        }
        sampled
    }

    pub fn visual_animation_targets_are_valid(&self, targets_are_frames: bool, targets: &[u32]) -> bool {
        targets.iter().all(|&target| {
            if targets_are_frames {
                matches!(self.frame_nodes.get(target as usize).map(|node| &node.data), Some(FrameData::Effects(_)))
            } else {
                matches!(
                    self.spatial_nodes.get(target as usize).map(|node| &node.data),
                    Some(SpatialData::Transform(transform)) if transform.role == TransformDataRole::CssTransform && !transform.synthetic_plane
                )
            }
        })
    }

    pub fn effects_opacity(&self, frame: FrameNodeIndex) -> Option<f32> {
        match self.frame_nodes.get(frame.0 as usize).map(|node| &node.data) {
            Some(FrameData::Effects(effects)) => Some(effects.opacity),
            _ => None,
        }
    }

    pub fn spatial_nodes_in_subtrees_of(&self, roots: &[SpatialNodeIndex]) -> Vec<bool> {
        let mut in_subtree = vec![false; self.spatial_nodes.len()];
        for root in roots {
            if let Some(flag) = in_subtree.get_mut(root.0 as usize) {
                *flag = true;
            }
        }
        for index in 1..self.spatial_nodes.len() {
            if in_subtree[self.spatial_nodes[index].parent.0 as usize] {
                in_subtree[index] = true;
            }
        }
        in_subtree
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::visual_context::{
        BackfaceVisibilityData, ClipData, ClipMode, EffectsData, FrameData, PerspectiveData, ScrollData, SpatialData,
        StickyData, TransformData, TransformDataRole, scroll_state::NO_SCROLL_STATE_SLOT,
    };
    use libgfx_rust::{CornerRadii, FloatMatrix4x4, FloatSize, perspective_matrix, scale_matrix, translation_matrix};

    fn transform_data(matrix: FloatMatrix4x4, origin: FloatPoint) -> TransformData {
        TransformData {
            matrix,
            origin,
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
        }
    }

    fn identity_tree() -> VisualContextTree {
        VisualContextTree::create(transform_data(FloatMatrix4x4::identity(), FloatPoint::default()))
    }

    fn scroll() -> SpatialData {
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
        })
    }

    fn clip(rect: FloatRect, mode: ClipMode) -> FrameData {
        FrameData::Clip(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode,
        })
    }

    fn point(x: f32, y: f32) -> FloatPoint {
        FloatPoint { x, y }
    }

    fn context_of(spatial: SpatialNodeIndex, frame: FrameNodeIndex) -> ContextRef {
        ContextRef { spatial, frame }
    }

    fn rotate_y_180_degrees() -> FloatMatrix4x4 {
        scale_matrix(-1.0, 1.0, -1.0)
    }

    fn effects(opacity: f32, blend_mode: CompositingAndBlendingOperator) -> FrameData {
        FrameData::Effects(EffectsData {
            opacity,
            blend_mode,
            filter: None,
        })
    }

    #[test]
    fn a_frame_is_isolated_by_an_effects_or_mask_ancestor_or_self() {
        let mut tree = identity_tree();
        let clip_frame = tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 10.0, 10.0), ClipMode::Intersect),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effects_frame = tree.append_frame(
            effects(0.5, CompositingAndBlendingOperator::Normal),
            clip_frame,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let nested_clip_frame = tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 5.0, 5.0), ClipMode::Intersect),
            effects_frame,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert!(!tree.frame_is_isolated_by_layer_frame(FrameNodeIndex::NONE));
        assert!(!tree.frame_is_isolated_by_layer_frame(clip_frame));
        assert!(tree.frame_is_isolated_by_layer_frame(effects_frame));
        assert!(tree.frame_is_isolated_by_layer_frame(nested_clip_frame));
    }

    #[test]
    fn a_blending_frame_counts_as_unisolated_only_without_a_layer_ancestor() {
        let mut tree = identity_tree();
        assert!(!tree.has_unisolated_blending_frame());
        let opacity_frame = tree.append_frame(
            effects(0.5, CompositingAndBlendingOperator::Normal),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        tree.append_frame(
            effects(1.0, CompositingAndBlendingOperator::Multiply),
            opacity_frame,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert!(!tree.has_unisolated_blending_frame());
        tree.append_frame(
            effects(1.0, CompositingAndBlendingOperator::Multiply),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert!(tree.has_unisolated_blending_frame());
    }

    #[test]
    fn a_difference_clip_passes_only_points_outside_its_rect() {
        let mut tree = identity_tree();
        let frame = tree.append_frame(
            clip(FloatRect::new(10.0, 10.0, 20.0, 20.0), ClipMode::Difference),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let context = context_of(VISUAL_VIEWPORT_NODE_INDEX, frame);
        assert!(
            tree.transform_point_for_hit_test(context, point(15.0, 15.0), &[], ClipBehavior::Respect)
                .is_none()
        );
        assert!(
            tree.transform_point_for_hit_test(context, point(5.0, 5.0), &[], ClipBehavior::Respect)
                .is_some()
        );
    }

    #[test]
    fn a_fractional_clip_rect_contains_points_by_float_containment() {
        let mut tree = identity_tree();
        let frame = tree.append_frame(
            clip(FloatRect::new(10.5, 10.5, 20.0, 20.0), ClipMode::Intersect),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let context = context_of(VISUAL_VIEWPORT_NODE_INDEX, frame);
        assert!(
            tree.transform_point_for_hit_test(context, point(10.25, 15.0), &[], ClipBehavior::Respect)
                .is_none()
        );
        assert!(
            tree.transform_point_for_hit_test(context, point(10.75, 15.0), &[], ClipBehavior::Respect)
                .is_some()
        );
    }

    #[test]
    fn an_integral_clip_rect_contains_points_by_integer_containment() {
        let mut tree = identity_tree();
        let frame = tree.append_frame(
            clip(FloatRect::new(10.0, 10.0, 20.0, 20.0), ClipMode::Intersect),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let context = context_of(VISUAL_VIEWPORT_NODE_INDEX, frame);
        assert!(
            tree.transform_point_for_hit_test(context, point(9.5, 15.0), &[], ClipBehavior::Respect)
                .is_none()
        );
        assert!(
            tree.transform_point_for_hit_test(context, point(29.5, 15.0), &[], ClipBehavior::Respect)
                .is_some()
        );
        assert!(
            tree.transform_point_for_hit_test(context, point(9.5, 15.0), &[], ClipBehavior::Ignore)
                .is_some()
        );
    }

    #[test]
    fn a_rounded_clip_rejects_points_outside_its_corner_ellipses() {
        let mut tree = identity_tree();
        let frame = tree.append_frame(
            FrameData::Clip(ClipData {
                rect: FloatRect::new(0.0, 0.0, 100.0, 100.0),
                corner_radii: CornerRadii::uniform(20.0 as i32),
                mode: ClipMode::Intersect,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let context = context_of(VISUAL_VIEWPORT_NODE_INDEX, frame);
        assert!(
            tree.transform_point_for_hit_test(context, point(1.0, 1.0), &[], ClipBehavior::Respect)
                .is_none()
        );
        assert!(
            tree.transform_point_for_hit_test(context, point(50.0, 1.0), &[], ClipBehavior::Respect)
                .is_some()
        );
        assert!(
            tree.transform_point_for_hit_test(context, point(98.0, 98.0), &[], ClipBehavior::Respect)
                .is_none()
        );
    }

    #[test]
    fn a_2d_transform_chain_inverts_each_node_around_its_origin() {
        let mut tree = identity_tree();
        let translated = tree.append_spatial(
            SpatialData::Transform(transform_data(
                translation_matrix(10.0, 20.0, 0.0),
                FloatPoint::default(),
            )),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let scaled = tree.append_spatial(
            SpatialData::Transform(transform_data(scale_matrix(2.0, 2.0, 1.0), point(10.0, 10.0))),
            translated,
        );
        let scroll_node = tree.append_spatial(scroll(), scaled);
        let mut scroll_offsets = vec![FloatPoint::default(); 4];
        scroll_offsets[scroll_node.0 as usize] = point(0.0, -100.0);

        let local = tree
            .transform_point_for_hit_test(
                context_of(scroll_node, FrameNodeIndex::NONE),
                point(40.0, 60.0),
                &scroll_offsets,
                ClipBehavior::Respect,
            )
            .expect("the point maps");
        assert_eq!(local, point(20.0, 125.0));
        assert_eq!(
            tree.inverse_transform_point(scroll_node, point(40.0, 60.0)),
            point(20.0, 25.0)
        );
    }

    #[test]
    fn a_3d_chain_inverts_the_flattened_accumulated_matrix() {
        let mut tree = identity_tree();
        let perspective = tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: perspective_matrix(1000.0),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let lifted = tree.append_spatial(
            SpatialData::Transform(transform_data(
                translation_matrix(0.0, 0.0, 500.0),
                FloatPoint::default(),
            )),
            perspective,
        );
        let local = tree
            .transform_point_for_hit_test(
                context_of(lifted, FrameNodeIndex::NONE),
                point(100.0, 100.0),
                &[],
                ClipBehavior::Respect,
            )
            .expect("the point maps");
        assert_eq!(local, point(50.0, 50.0));
        assert_eq!(
            tree.inverse_transform_point(lifted, point(100.0, 100.0)),
            point(50.0, 50.0)
        );
        assert_eq!(
            tree.transform_rect_to_viewport(
                lifted,
                FloatRect::new(0.0, 0.0, 10.0, 10.0),
                &[],
                IncludeVisualViewportTransform::Yes
            ),
            FloatRect::new(0.0, 0.0, 20.0, 20.0)
        );
    }

    #[test]
    fn a_rect_crossing_the_eye_plane_is_clamped_instead_of_projected_without_bound() {
        let mut tree = identity_tree();
        let perspective = tree.append_spatial(
            SpatialData::Perspective(PerspectiveData {
                matrix: perspective_matrix(1000.0),
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let on_the_eye_plane = tree.append_spatial(
            SpatialData::Transform(transform_data(
                translation_matrix(0.0, 0.0, 1000.0),
                FloatPoint::default(),
            )),
            perspective,
        );
        let rect = tree.transform_rect_to_viewport(
            on_the_eye_plane,
            FloatRect::new(0.0, 0.0, 10.0, 10.0),
            &[],
            IncludeVisualViewportTransform::Yes,
        );
        assert!(rect.width.is_finite() && rect.height.is_finite());
        assert_eq!(rect.width, 100000.0);
        assert!(
            tree.transform_point_for_hit_test(
                context_of(on_the_eye_plane, FrameNodeIndex::NONE),
                point(100.0, 100.0),
                &[],
                ClipBehavior::Respect,
            )
            .is_none()
        );
    }

    #[test]
    fn a_backface_marker_rejects_points_on_a_flipped_plane() {
        let mut tree = identity_tree();
        let flipped = tree.append_spatial(
            SpatialData::Transform(transform_data(rotate_y_180_degrees(), FloatPoint::default())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let marker = tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: VISUAL_VIEWPORT_NODE_INDEX,
                flattens_inherited_transform: false,
            }),
            flipped,
        );
        assert!(
            tree.transform_point_for_hit_test(
                context_of(marker, FrameNodeIndex::NONE),
                point(0.0, 0.0),
                &[],
                ClipBehavior::Respect
            )
            .is_none()
        );

        let mut front_facing_tree = identity_tree();
        let front_marker = front_facing_tree.append_spatial(
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: VISUAL_VIEWPORT_NODE_INDEX,
                flattens_inherited_transform: false,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(
            front_facing_tree.transform_point_for_hit_test(
                context_of(front_marker, FrameNodeIndex::NONE),
                point(3.0, 4.0),
                &[],
                ClipBehavior::Respect
            ),
            Some(point(3.0, 4.0))
        );
    }

    #[test]
    fn a_frame_below_the_context_node_takes_its_own_walk_from_the_root() {
        let mut tree = identity_tree();
        let scroll_node = tree.append_spatial(scroll(), VISUAL_VIEWPORT_NODE_INDEX);
        let frame = tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 50.0, 50.0), ClipMode::Intersect),
            FrameNodeIndex::NONE,
            scroll_node,
        );
        let mut scroll_offsets = vec![FloatPoint::default(); 2];
        scroll_offsets[scroll_node.0 as usize] = point(0.0, -100.0);
        let fixed_background_context = context_of(VISUAL_VIEWPORT_NODE_INDEX, frame);

        assert!(
            tree.transform_point_for_hit_test(
                fixed_background_context,
                point(10.0, 120.0),
                &scroll_offsets,
                ClipBehavior::Respect
            )
            .is_none()
        );
        assert_eq!(
            tree.transform_point_for_hit_test(
                fixed_background_context,
                point(10.0, -60.0),
                &scroll_offsets,
                ClipBehavior::Respect
            ),
            Some(point(10.0, -60.0))
        );
    }

    #[test]
    fn a_2d_rect_maps_node_by_node_and_may_leave_out_the_visual_viewport() {
        let mut tree = VisualContextTree::create(transform_data(scale_matrix(2.0, 2.0, 1.0), FloatPoint::default()));
        let translated = tree.append_spatial(
            SpatialData::Transform(transform_data(translation_matrix(10.0, 20.0, 0.0), point(5.0, 5.0))),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let scroll_node = tree.append_spatial(scroll(), translated);
        let mut scroll_offsets = vec![FloatPoint::default(); 3];
        scroll_offsets[scroll_node.0 as usize] = point(-1.0, -2.0);
        let rect = FloatRect::new(0.0, 0.0, 10.0, 10.0);

        assert_eq!(
            tree.transform_rect_to_viewport(scroll_node, rect, &scroll_offsets, IncludeVisualViewportTransform::No),
            FloatRect::new(9.0, 18.0, 10.0, 10.0)
        );
        assert_eq!(
            tree.transform_rect_to_viewport(scroll_node, rect, &scroll_offsets, IncludeVisualViewportTransform::Yes),
            FloatRect::new(18.0, 36.0, 20.0, 20.0)
        );
    }

    #[test]
    fn the_cumulative_scroll_chain_offset_sums_entries_up_the_scroll_parent_chain() {
        let mut tree = identity_tree();
        let outer_scroll = tree.append_spatial(scroll(), VISUAL_VIEWPORT_NODE_INDEX);
        let transformed = tree.append_spatial(
            SpatialData::Transform(transform_data(FloatMatrix4x4::identity(), FloatPoint::default())),
            outer_scroll,
        );
        let inner_scroll = tree.append_spatial(scroll(), transformed);
        let sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(inner_scroll, None, NO_SCROLL_STATE_SLOT)),
            inner_scroll,
        );
        let mut scroll_offsets = vec![FloatPoint::default(); 5];
        scroll_offsets[outer_scroll.0 as usize] = point(0.0, -100.0);
        scroll_offsets[inner_scroll.0 as usize] = point(0.0, -30.0);
        scroll_offsets[sticky.0 as usize] = point(0.0, 5.0);

        assert_eq!(
            tree.cumulative_scroll_chain_offset(sticky, &scroll_offsets),
            point(0.0, -125.0)
        );
        assert_eq!(
            tree.cumulative_scroll_chain_offset(inner_scroll, &scroll_offsets),
            point(0.0, -130.0)
        );
        assert_eq!(
            tree.cumulative_scroll_chain_offset(VISUAL_VIEWPORT_NODE_INDEX, &scroll_offsets),
            FloatPoint::default()
        );
    }

    #[test]
    fn sticky_offsets_resolve_from_the_scroller_entry_and_the_parent_sticky_chain() {
        let mut tree = identity_tree();
        let viewport_scroll_node = tree.append_spatial(scroll(), VISUAL_VIEWPORT_NODE_INDEX);
        // A 50px header 100px down the document, sticking to the top of the scrollport within a 2000px containing block.
        let header_node = tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: viewport_scroll_node,
                parent_sticky: None,
                position_relative_to_scroller: point(0.0, 100.0),
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 50.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 0.0, 800.0, 2000.0),
                needs_parent_offset_adjustment: true,
                inset_top: Some(0.0),
                inset_right: None,
                inset_bottom: None,
                inset_left: None,
                state_slot: NO_SCROLL_STATE_SLOT,
            }),
            viewport_scroll_node,
        );
        // A 10px bar inside the header, sticking 20px below the scrollport top within the header's box.
        let bar_node = tree.append_spatial(
            SpatialData::Sticky(StickyData {
                scroller: viewport_scroll_node,
                parent_sticky: Some(header_node),
                position_relative_to_scroller: point(0.0, 110.0),
                border_box_size: FloatSize {
                    width: 800.0,
                    height: 10.0,
                },
                scrollport_size: FloatSize {
                    width: 800.0,
                    height: 600.0,
                },
                containing_block_region: FloatRect::new(0.0, 100.0, 800.0, 50.0),
                needs_parent_offset_adjustment: true,
                inset_top: Some(20.0),
                inset_right: None,
                inset_bottom: None,
                inset_left: None,
                state_slot: NO_SCROLL_STATE_SLOT,
            }),
            header_node,
        );

        let resolve = |scroll_position: f32| {
            let scroll_offsets = [FloatPoint::default(), point(0.0, -scroll_position)];
            tree.resolve_sticky_offsets(&scroll_offsets)
        };

        // The scrollport top passed the header by 200px, so the header follows it by that much;
        // with the header at 300, the bar sits at 310 and its inset asks for 320.
        assert_eq!(
            resolve(300.0),
            vec![(header_node, point(0.0, 200.0)), (bar_node, point(0.0, 10.0))]
        );
        // Past the end of the containing block, the header pins to the block's bottom edge and the bar to the header's.
        assert_eq!(
            resolve(1960.0),
            vec![(header_node, point(0.0, 1850.0)), (bar_node, point(0.0, 20.0))]
        );
        // Scrolled back above the header, nothing sticks.
        assert_eq!(
            resolve(50.0),
            vec![(header_node, point(0.0, 0.0)), (bar_node, point(0.0, 0.0))]
        );
    }

    #[test]
    fn a_sticky_node_under_the_root_scroller_resolves_to_a_zero_entry() {
        let mut tree = identity_tree();
        let sticky = tree.append_spatial(
            SpatialData::Sticky(StickyData::unconstrained(
                VISUAL_VIEWPORT_NODE_INDEX,
                None,
                NO_SCROLL_STATE_SLOT,
            )),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(tree.resolve_sticky_offsets(&[]), vec![(sticky, FloatPoint::default())]);
    }
}
