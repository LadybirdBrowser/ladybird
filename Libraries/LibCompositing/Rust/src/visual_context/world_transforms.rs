/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    IncludeVisualViewportTransform, LocalTransformKind, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
    VisualContextTree, should_cull_back_face,
};
use libgfx_rust::{
    FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, map_rect_through_matrix, multiply_affine, translation_matrix,
};

// Homogeneous coordinates this close to the eye plane have no meaningful projection.
pub const MINIMUM_PROJECTION_W: f32 = 0.0001;

/// The to-root matrix of every spatial node under one set of scroll offsets, filled in dependency order so that any
/// node's space is entered with a single matrix. A backface marker's entry carries the flattened matrix that feeds its
/// cull test and its descendants, while content recorded directly under the marker belongs to its parent's plane, so
/// `draw_space` redirects the marker to the parent's entry. A table filled without a root parent matrix is in the
/// layout viewport's space, and the queries below add the visual viewport transform where asked.
///
/// Scroll, sticky and anchor scroll shift translations under no transform but identities round to whole device pixels
/// of the layout viewport, so scrolled content never lands on subpixel positions. Under a transform they stay exact.
#[derive(Default)]
pub struct WorldTransforms {
    pub local: Vec<FloatMatrix4x4>,
    pub to_root: Vec<FloatMatrix4x4>,
    pub flattens_inherited_transform: Vec<bool>,
    pub backface_culled: Vec<bool>,
    pub draw_space: Vec<SpatialNodeIndex>,
    pub translations_snap_to_device_pixels: Vec<bool>,
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
        if tree.local_transform(index, &[]).establishes_sorting_context {
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
        root_parent_matrix: Option<FloatMatrix4x4>,
    ) -> Self {
        let mut world = Self::default();
        world.fill(
            tree,
            &tree.spatial_dependency_order(),
            scroll_offsets,
            root_parent_matrix,
        );
        world
    }

    pub fn clear(&mut self) {
        self.local.clear();
        self.to_root.clear();
        self.flattens_inherited_transform.clear();
        self.backface_culled.clear();
        self.draw_space.clear();
        self.translations_snap_to_device_pixels.clear();
    }

    /// Fills the table, with `root_parent_matrix` as what the visual viewport node's transform applies on top of, or
    /// with that transform left out.
    pub fn fill(
        &mut self,
        tree: &VisualContextTree,
        spatial_dependency_order: &[u32],
        scroll_offsets: &[FloatPoint],
        root_parent_matrix: Option<FloatMatrix4x4>,
    ) {
        let node_count = tree.spatial_nodes.len();
        self.clear();
        self.local.resize(node_count, FloatMatrix4x4::identity());
        self.to_root.resize(node_count, FloatMatrix4x4::identity());
        self.flattens_inherited_transform.resize(node_count, false);
        self.backface_culled.resize(node_count, false);
        self.draw_space
            .extend((0..node_count).map(|index| SpatialNodeIndex(index as u32)));
        self.translations_snap_to_device_pixels.resize(node_count, false);
        for &index in spatial_dependency_order {
            let i = index as usize;
            let parent = tree.spatial_nodes[i].parent.0 as usize;
            let mut local = tree.local_transform(SpatialNodeIndex(index), scroll_offsets);
            let transforms = local.is_transform_or_perspective() && local.matrix != FloatMatrix4x4::identity();
            let snaps = i == 0 || (self.translations_snap_to_device_pixels[parent] && !transforms);
            self.translations_snap_to_device_pixels[i] = snaps;
            if let LocalTransformKind::Translation(offset) = local.kind
                && snaps
            {
                local.matrix = translation_matrix(offset.x.round(), offset.y.round(), 0.0);
            }
            self.to_root[i] = match (i, root_parent_matrix) {
                (0, None) => FloatMatrix4x4::identity(),
                (0, Some(root_parent_matrix)) => local.applied_to(root_parent_matrix),
                _ => local.applied_to(self.to_root[parent]),
            };
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

    pub fn translations_snap_to_device_pixels(&self, node: SpatialNodeIndex) -> bool {
        self.translations_snap_to_device_pixels
            .get(node.0 as usize)
            .copied()
            .unwrap_or(false)
    }

    pub fn draw_matrix_if_known(&self, space: SpatialNodeIndex) -> Option<FloatMatrix4x4> {
        ((space.0 as usize) < self.draw_space.len()).then(|| self.draw_matrix(space))
    }

    /// Where content drawn within `rect` in `space` lands in the root's space.
    pub fn map_rect_to_root(&self, space: SpatialNodeIndex, rect: FloatRect) -> FloatRect {
        map_rect_through(self.draw_matrix(space), rect)
    }

    /// The matrix from `node`'s space to the viewport, in a table filled without a root parent matrix.
    pub fn to_viewport_matrix(
        &self,
        node: SpatialNodeIndex,
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatMatrix4x4 {
        debug_assert!(self.to_root[0] == FloatMatrix4x4::identity());
        let matrix = self.to_root[node.0 as usize];
        match include_visual_viewport_transform {
            IncludeVisualViewportTransform::No => matrix,
            IncludeVisualViewportTransform::Yes => self.local[0].multiplied(matrix),
        }
    }

    pub fn transform_rect_to_viewport(
        &self,
        node: SpatialNodeIndex,
        rect: FloatRect,
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatRect {
        map_rect_through(self.to_viewport_matrix(node, include_visual_viewport_transform), rect)
    }

    pub fn accumulated_2d_scale(
        &self,
        node: SpatialNodeIndex,
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> FloatSize {
        let affine = self
            .to_viewport_matrix(node, include_visual_viewport_transform)
            .extract_2d_affine();
        FloatSize {
            width: affine.x_scale(),
            height: affine.y_scale(),
        }
    }

    pub fn plane_depth_at_point_for_hit_test(&self, plane: SpatialNodeIndex, screen_point: FloatPoint) -> Option<f32> {
        let inverse = self
            .to_viewport_matrix(plane, IncludeVisualViewportTransform::Yes)
            .inverse()?;
        let matrix = &inverse.elements;
        let depth = -(screen_point.x * matrix[2][0] + screen_point.y * matrix[2][1] + matrix[2][3]) / matrix[2][2];
        depth.is_finite().then_some(depth)
    }

    /// How far the nodes on the spatial chain from `node` up to the viewport move a box when its transforms are left
    /// out, in device pixels.
    pub fn offset_to_viewport_ignoring_transforms(
        &self,
        tree: &VisualContextTree,
        node: SpatialNodeIndex,
    ) -> FloatPoint {
        let mut offset = FloatPoint::default();
        let mut current = node;
        while current != VISUAL_VIEWPORT_NODE_INDEX {
            if let LocalTransformKind::Translation(_) = tree.local_transform(current, &[]).kind {
                let local = &self.local[current.0 as usize].elements;
                offset.x += local[0][3];
                offset.y += local[1][3];
            }
            current = tree.spatial_nodes[current.0 as usize].parent;
        }
        offset
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

fn map_rect_through(matrix: FloatMatrix4x4, rect: FloatRect) -> FloatRect {
    if matrix.is_2d_affine() {
        return matrix.extract_2d_affine().map_rect(rect);
    }
    map_rect_through_matrix(matrix, rect, MINIMUM_PROJECTION_W)
}
