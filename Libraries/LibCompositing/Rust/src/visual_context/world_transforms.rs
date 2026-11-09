/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{ScrollSnap, SpatialNodeIndex, VisualContextTree, should_cull_back_face};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect, map_rect_through_matrix, multiply_affine};

// Homogeneous coordinates this close to the eye plane have no meaningful projection.
pub const MINIMUM_PROJECTION_W: f32 = 0.0001;

/// The to-root matrix of every spatial node under one set of scroll offsets, filled in dependency order so that any
/// node's space is entered with a single matrix. A backface marker's entry carries the flattened matrix that feeds its
/// cull test and its descendants, while content recorded directly under the marker belongs to its parent's plane, so
/// `draw_space` redirects the marker to the parent's entry.
#[derive(Default)]
pub struct WorldTransforms {
    pub local: Vec<FloatMatrix4x4>,
    pub to_root: Vec<FloatMatrix4x4>,
    pub flattens_inherited_transform: Vec<bool>,
    pub backface_culled: Vec<bool>,
    pub draw_space: Vec<SpatialNodeIndex>,
}

// The cull state of the element that establishes the 3D rendering context rooted at root: the cull state of the
// plane the context is flattened into.
fn backface_culled_outside_sorting_context(
    tree: &VisualContextTree,
    backface_culled: &[bool],
    root: SpatialNodeIndex,
) -> bool {
    let mut index = root;
    loop {
        if tree
            .local_transform(index, &[], ScrollSnap::None)
            .establishes_sorting_context
        {
            return backface_culled[index.0 as usize];
        }
        if index.0 == 0 {
            return false;
        }
        index = tree.spatial_nodes[index.0 as usize].parent;
    }
}

impl WorldTransforms {
    pub fn new(
        tree: &VisualContextTree,
        scroll_offsets: &[FloatPoint],
        root_parent_matrix: FloatMatrix4x4,
        scroll_snap: ScrollSnap,
    ) -> Self {
        let mut world = Self::default();
        world.fill(
            tree,
            &tree.spatial_dependency_order(),
            scroll_offsets,
            root_parent_matrix,
            scroll_snap,
        );
        world
    }

    pub fn clear(&mut self) {
        self.local.clear();
        self.to_root.clear();
        self.flattens_inherited_transform.clear();
        self.backface_culled.clear();
        self.draw_space.clear();
    }

    /// Fills the table, with `root_parent_matrix` as what the visual viewport node's transform applies on top of.
    pub fn fill(
        &mut self,
        tree: &VisualContextTree,
        spatial_dependency_order: &[u32],
        scroll_offsets: &[FloatPoint],
        root_parent_matrix: FloatMatrix4x4,
        scroll_snap: ScrollSnap,
    ) {
        let node_count = tree.spatial_nodes.len();
        self.clear();
        self.local.resize(node_count, FloatMatrix4x4::identity());
        self.to_root.resize(node_count, root_parent_matrix);
        self.flattens_inherited_transform.resize(node_count, false);
        self.backface_culled.resize(node_count, false);
        self.draw_space
            .extend((0..node_count).map(|index| SpatialNodeIndex(index as u32)));
        for &index in spatial_dependency_order {
            let i = index as usize;
            let parent = tree.spatial_nodes[i].parent.0 as usize;
            let local = tree.local_transform(SpatialNodeIndex(index), scroll_offsets, scroll_snap);
            let parent_matrix = if i == 0 {
                root_parent_matrix
            } else {
                self.to_root[parent]
            };
            self.to_root[i] = local.applied_to(parent_matrix);
            self.local[i] = local.matrix;
            self.flattens_inherited_transform[i] = local.flattens_inherited_transform;
            let mut culled = i != 0 && self.backface_culled[parent];
            // https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
            // A participant in a 3D rendering context is its own plane, so the hidden backfaces of other planes in the
            // context don't cull it. But a context is part of the content flattened into the plane its establishing
            // element sits in, so whatever culls that plane culls the whole context: A participant starts from the
            // cull state of the element that establishes its context. The spec doesn't yet say if a hidden backface
            // culls the 3D rendering contexts nested in it:
            // https://github.com/w3c/csswg-drafts/issues/918
            if let Some(root) = local.sorting_context_root {
                culled = backface_culled_outside_sorting_context(tree, &self.backface_culled, root);
            }
            if let Some(plane_root) = local.backface_plane_root {
                self.draw_space[i] = self.draw_space[parent];
                culled = culled || should_cull_back_face(self.to_root[i], self.to_root[plane_root.0 as usize]);
            }
            self.backface_culled[i] = culled;
        }
    }

    /// The matrix that content recorded in `space` is drawn with.
    pub fn draw_matrix(&self, space: SpatialNodeIndex) -> FloatMatrix4x4 {
        self.to_root[self.draw_space[space.0 as usize].0 as usize]
    }

    pub fn draw_matrix_if_known(&self, space: SpatialNodeIndex) -> Option<FloatMatrix4x4> {
        ((space.0 as usize) < self.draw_space.len()).then(|| self.draw_matrix(space))
    }

    /// Where content drawn within `rect` in `space` lands in the root's space.
    pub fn map_rect_to_root(&self, space: SpatialNodeIndex, rect: FloatRect) -> FloatRect {
        let matrix = self.draw_matrix(space);
        if matrix.is_2d_affine() {
            return matrix.extract_2d_affine().map_rect(rect);
        }
        map_rect_through_matrix(matrix, rect, MINIMUM_PROJECTION_W)
    }

    /// Maps `rect` from `from`'s space into `to`'s, or gives `None` when either space is not a two-dimensional
    /// transform of the root or `to`'s cannot be inverted.
    pub fn map_rect_between_spaces(
        &self,
        rect: FloatRect,
        from: SpatialNodeIndex,
        to: SpatialNodeIndex,
    ) -> Option<FloatRect> {
        if from == to {
            return Some(rect);
        }
        let from_matrix = self.draw_matrix(from);
        let to_matrix = self.draw_matrix(to);
        if !from_matrix.is_2d_affine() || !to_matrix.is_2d_affine() {
            return None;
        }
        let root_to_target = to_matrix.extract_2d_affine().inverse()?;
        Some(multiply_affine(root_to_target, from_matrix.extract_2d_affine()).map_rect(rect))
    }
}
