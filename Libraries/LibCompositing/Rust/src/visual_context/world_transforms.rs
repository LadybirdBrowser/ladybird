/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    IncludeVisualViewportTransform, LocalTransformKind, SpatialData, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
    VisualContextTree, should_cull_back_face,
};
use crate::display_list::placement::is_finite_rect;
use libgfx_rust::{
    AffineTransform, FloatMatrix4x4, FloatPoint, FloatRect, FloatSize, map_rect_through_matrix, multiply_affine,
    scale_matrix, translation_matrix,
};

/// A spatial node whose subtree a table maps into the node's own coordinates times `raster_scale`, under the root's
/// parent matrix, rather than through the node's transform, which the compositor animates: the subtree lands alike in
/// every sample of the animation.
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct RasterRoot {
    pub anchor: SpatialNodeIndex,
    pub raster_scale: f32,
}

/// Homogeneous coordinates this close to the eye plane have no meaningful projection.
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

/// The translation by `offset` of a scroll, sticky or anchor scroll shift node under no transform, on whole device
/// pixels. A sticky offset carries the fraction of the box's own position, which replay rounds the box by already, so
/// for a sticky box recorded at `sticky_box_origin` where the box lands is rounded, not the offset, or a box at a half
/// pixel would round twice.
pub fn translation_on_whole_device_pixels(offset: FloatPoint, sticky_box_origin: Option<FloatPoint>) -> FloatPoint {
    match sticky_box_origin {
        Some(origin) => FloatPoint {
            x: (origin.x + offset.x).round() - origin.x.round(),
            y: (origin.y + offset.y).round() - origin.y.round(),
        },
        None => FloatPoint {
            x: offset.x.round(),
            y: offset.y.round(),
        },
    }
}

fn backface_culled_at_element_establishing_sorting_context(
    tree: &VisualContextTree,
    backface_culled: &[bool],
    sorting_context_root: SpatialNodeIndex,
) -> bool {
    let mut index = sorting_context_root;
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
        Self::with_raster_root(tree, scroll_offsets, root_parent_matrix, None)
    }

    pub fn with_raster_root(
        tree: &VisualContextTree,
        scroll_offsets: &[FloatPoint],
        root_parent_matrix: Option<FloatMatrix4x4>,
        raster_root: Option<RasterRoot>,
    ) -> Self {
        let mut world = Self::default();
        world.fill(
            tree,
            &tree.spatial_dependency_order(),
            scroll_offsets,
            root_parent_matrix,
            raster_root,
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
    /// with that transform left out, and the subtree of `raster_root` mapped as it says.
    pub fn fill(
        &mut self,
        tree: &VisualContextTree,
        spatial_dependency_order: &[u32],
        scroll_offsets: &[FloatPoint],
        root_parent_matrix: Option<FloatMatrix4x4>,
        raster_root: Option<RasterRoot>,
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
            let raster_root_here = raster_root.filter(|raster_root| raster_root.anchor.0 == index);
            if let Some(raster_root) = raster_root_here {
                let scale = scale_matrix(raster_root.raster_scale, raster_root.raster_scale, 1.0);
                local.matrix = scale;
                local.flattens_inherited_transform = false;
            }
            let has_non_identity_transform = raster_root_here.is_some()
                || (local.is_transform_or_perspective() && local.matrix != FloatMatrix4x4::identity());
            let translations_snap =
                i == 0 || (self.translations_snap_to_device_pixels[parent] && !has_non_identity_transform);
            self.translations_snap_to_device_pixels[i] = translations_snap;
            if let LocalTransformKind::Translation(offset) = local.kind
                && translations_snap
            {
                let sticky_box_origin = match &tree.spatial_nodes[i].data {
                    SpatialData::Sticky(sticky) => Some(sticky.border_box_origin),
                    _ => None,
                };
                let snapped_offset = translation_on_whole_device_pixels(offset, sticky_box_origin);
                local.matrix = translation_matrix(snapped_offset.x, snapped_offset.y, 0.0);
            }
            self.to_root[i] = match (i, root_parent_matrix) {
                (0, None) => FloatMatrix4x4::identity(),
                (0, Some(root_parent_matrix)) => local.applied_to(root_parent_matrix),
                _ if raster_root_here.is_some() => {
                    local.applied_to(root_parent_matrix.unwrap_or(FloatMatrix4x4::identity()))
                }
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
                culled = backface_culled_at_element_establishing_sorting_context(tree, &self.backface_culled, root);
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

    /// The mapping from `node`'s space to the viewport, for many rects of one node.
    pub fn viewport_rect_mapping(
        &self,
        node: SpatialNodeIndex,
        include_visual_viewport_transform: IncludeVisualViewportTransform,
    ) -> RectMapping {
        RectMapping::new(self.to_viewport_matrix(node, include_visual_viewport_transform))
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

    /// As `map_rect_between_spaces`, with spaces in perspective too, unless a corner of `rect` lies behind the eye or
    /// does not project back onto `to`'s plane in front of the eye.
    pub fn bounds_of_rect_mapped_between_spaces(
        &self,
        rect: FloatRect,
        from: SpatialNodeIndex,
        to: SpatialNodeIndex,
    ) -> Option<FloatRect> {
        if let Some(mapped) = self.map_rect_between_spaces(rect, from, to) {
            return Some(mapped);
        }
        let corners_in_root = corners_projected_if_in_front_of_eye(self.draw_matrix(from), corners_of(rect))?;
        let corners_in_to = corners_on_plane_projecting_onto(self.draw_matrix(to), corners_in_root)?;
        Some(bounds_of_corners(corners_in_to)).filter(|bounds| is_finite_rect(*bounds))
    }

    pub fn bounds_in_root_of_rect_drawn_in(&self, space: SpatialNodeIndex, rect: FloatRect) -> Option<FloatRect> {
        bounds_of_projected_rect_if_corners_in_front_of_eye(self.draw_matrix(space), rect)
            .filter(|bounds| is_finite_rect(*bounds))
    }

    pub fn bounds_in_space_of_root_rect(&self, space: SpatialNodeIndex, rect: FloatRect) -> Option<FloatRect> {
        let corners = corners_on_plane_projecting_onto(self.draw_matrix(space), corners_of(rect))?;
        Some(bounds_of_corners(corners)).filter(|bounds| is_finite_rect(*bounds))
    }
}

fn corners_of(rect: FloatRect) -> [(f32, f32); 4] {
    [
        (rect.x, rect.y),
        (rect.right(), rect.y),
        (rect.x, rect.bottom()),
        (rect.right(), rect.bottom()),
    ]
}

fn bounds_of_corners(corners: [(f32, f32); 4]) -> FloatRect {
    let (left, right) = corners
        .iter()
        .fold((f32::INFINITY, f32::NEG_INFINITY), |(left, right), &(x, _)| {
            (left.min(x), right.max(x))
        });
    let (top, bottom) = corners
        .iter()
        .fold((f32::INFINITY, f32::NEG_INFINITY), |(top, bottom), &(_, y)| {
            (top.min(y), bottom.max(y))
        });
    FloatRect::new(left, top, right - left, bottom - top)
}

fn corners_projected_if_in_front_of_eye(matrix: FloatMatrix4x4, corners: [(f32, f32); 4]) -> Option<[(f32, f32); 4]> {
    const SMALLEST_W_IN_FRONT_OF_EYE: f32 = 0.00001;
    let corners = corners.map(|(x, y)| matrix.map_vector4([x, y, 0.0, 1.0]));
    if corners.iter().any(|corner| corner[3] <= SMALLEST_W_IN_FRONT_OF_EYE) {
        return None;
    }
    Some(corners.map(|corner| (corner[0] / corner[3], corner[1] / corner[3])))
}

pub(crate) fn bounds_of_projected_rect_if_corners_in_front_of_eye(
    matrix: FloatMatrix4x4,
    rect: FloatRect,
) -> Option<FloatRect> {
    corners_projected_if_in_front_of_eye(matrix, corners_of(rect)).map(bounds_of_corners)
}

/// The points of the z = 0 plane that project through `matrix` onto `points`, if each is where a point in front of the
/// eye projects. The points of the plane then bound every point that projects into the quad of `points`.
fn corners_on_plane_projecting_onto(matrix: FloatMatrix4x4, points: [(f32, f32); 4]) -> Option<[(f32, f32); 4]> {
    let m = &matrix.elements;
    let plane_to_root = [0, 1, 3].map(|row| [0, 1, 3].map(|column| m[row][column] as f64));
    let root_to_plane = inverse_of_3x3(plane_to_root)?;
    let points = points.map(|(x, y)| root_to_plane.map(|row| row[0] * x as f64 + row[1] * y as f64 + row[2]));
    if points.iter().any(|point| point[2] <= 0.0) {
        return None;
    }
    Some(points.map(|point| ((point[0] / point[2]) as f32, (point[1] / point[2]) as f32)))
}

fn inverse_of_3x3(m: [[f64; 3]; 3]) -> Option<[[f64; 3]; 3]> {
    let cofactor = |row: usize, column: usize| {
        let (r0, r1) = ((row + 1) % 3, (row + 2) % 3);
        let (c0, c1) = ((column + 1) % 3, (column + 2) % 3);
        m[r0][c0] * m[r1][c1] - m[r0][c1] * m[r1][c0]
    };
    let determinant = m[0][0] * cofactor(0, 0) + m[0][1] * cofactor(0, 1) + m[0][2] * cofactor(0, 2);
    if determinant == 0.0 || !determinant.is_finite() {
        return None;
    }
    Some([0, 1, 2].map(|row| [0, 1, 2].map(|column| cofactor(column, row) / determinant)))
}

fn map_rect_through(matrix: FloatMatrix4x4, rect: FloatRect) -> FloatRect {
    RectMapping::new(matrix).map_rect(rect)
}

/// A matrix that maps rects, with the check for a 2D affine matrix done once.
#[derive(Clone, Copy)]
pub enum RectMapping {
    Affine(AffineTransform),
    Projective(FloatMatrix4x4),
}

impl RectMapping {
    pub fn new(matrix: FloatMatrix4x4) -> Self {
        if matrix.is_2d_affine() {
            return Self::Affine(matrix.extract_2d_affine());
        }
        Self::Projective(matrix)
    }

    pub fn map_rect(&self, rect: FloatRect) -> FloatRect {
        match self {
            Self::Affine(affine) => affine.map_rect(rect),
            Self::Projective(matrix) => map_rect_through_matrix(*matrix, rect, MINIMUM_PROJECTION_W),
        }
    }
}
