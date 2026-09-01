/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::box_build::PaintableVisualContextAssignment;
use super::scroll_state::NO_SCROLL_STATE_SLOT;
use super::*;
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::{FfiVisualContextHostCallbacks, FfiVisualContextTreeInputs};
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use libgfx_rust::{
    AffineTransform, CompositingAndBlendingOperator, FloatPoint, IntRect, WindingRule, affine_to_matrix,
    scale_matrix_for_device_pixels, translated_then_multiplied,
};

// Content below a viewport node records in the viewport's user units scaled by the device pixel
// ratio, mirroring how ordinary content records in CSS pixels scaled by it; the node folds the
// viewport box's position in its own recorded space together with the viewBox transform.
pub(crate) fn compute_svg_viewport_transform_data(
    layout_arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    viewbox_transform: AffineTransform,
    pixel_ratio: f64,
) -> TransformData {
    let location = paintable_geometry::absolute_rect(layout_arena, slot).location();
    let matrix = translated_then_multiplied(
        FloatPoint {
            x: location.x.to_float(),
            y: location.y.to_float(),
        },
        viewbox_transform,
    );
    TransformData {
        matrix: scale_matrix_for_device_pixels(affine_to_matrix(matrix), pixel_ratio as f32),
        origin: FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: TransformDataRole::SvgViewportTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
    }
}

pub(crate) fn svg_viewport_transform_of(
    layout_arena: &crate::layout::LayoutNodeArena,
    slot: NodeSlotId,
) -> Option<AffineTransform> {
    let t = crate::painting::paintable_geometry::committed_svg_viewport_transform(layout_arena, slot)?;
    Some(AffineTransform {
        values: [t.a, t.b, t.c, t.d, t.e, t.f],
    })
}

pub(crate) struct BoxFacts {
    pub transform: Option<TransformData>,
    pub transform_is_invertible: bool,
    pub perspective: Option<PerspectiveData>,
    pub effects: Option<std::rc::Rc<EffectsData>>,
    pub overflow_clip: Option<ClipData>,
    pub css_clip: Option<ClipData>,
    pub clip_path: Option<(std::rc::Rc<libgfx_rust::path::OwnedPath>, IntRect, WindingRule)>,
    pub mask_layers: Vec<MaskData>,
    pub establishes_absolute_containing_block: bool,
    pub establishes_fixed_containing_block: bool,
    pub backface_hidden: bool,
    pub establishes_or_extends_3d_rendering_context: bool,
    pub may_have_clip: bool,
    pub default_scroll_shift_anchor: NodeSlotId,
}

impl BoxFacts {
    pub(crate) fn effects_data(&self) -> Option<EffectsData> {
        self.effects.as_ref().map(|effects| EffectsData {
            opacity: effects.opacity,
            blend_mode: effects.blend_mode,
            filter: effects.filter.clone(),
        })
    }

    pub(crate) fn gather(
        layout_arena: &impl PaintableRowsRead,
        callbacks: &FfiVisualContextHostCallbacks,
        slot: NodeSlotId,
        pixel_ratio: f64,
        may_have_default_scroll_shift_anchor: bool,
    ) -> Self {
        let mut facts = Self {
            transform: None,
            transform_is_invertible: false,
            perspective: None,
            effects: None,
            overflow_clip: None,
            css_clip: None,
            clip_path: None,
            mask_layers: Vec::new(),
            establishes_absolute_containing_block: false,
            establishes_fixed_containing_block: false,
            backface_hidden: false,
            establishes_or_extends_3d_rendering_context: false,
            may_have_clip: false,
            default_scroll_shift_anchor: if may_have_default_scroll_shift_anchor {
                let node = slot;
                callbacks.default_scroll_shift_anchor(layout_arena.shell_if_live(node))
            } else {
                NodeSlotId::INVALID
            },
        };
        if let Some((transform, transform_is_invertible)) =
            super::node_values::compute_transform(layout_arena, callbacks, slot, pixel_ratio)
        {
            facts.transform = Some(transform);
            facts.transform_is_invertible = transform_is_invertible;
        }
        facts.perspective = super::node_values::compute_perspective_data(layout_arena, slot, pixel_ratio);
        facts.effects =
            super::node_values::compute_effects_data(layout_arena, callbacks, slot, pixel_ratio).map(std::rc::Rc::new);
        facts.backface_hidden = super::node_values::backface_hidden(layout_arena, slot);
        let node = slot;
        facts.establishes_or_extends_3d_rendering_context =
            crate::painting::style_queries::establishes_or_extends_a_3d_rendering_context(layout_arena, node);
        let (establishes_absolute, establishes_fixed) =
            crate::painting::style_queries::establishes_positioning_containing_blocks(layout_arena, node);
        facts.establishes_absolute_containing_block = establishes_absolute;
        facts.establishes_fixed_containing_block = establishes_fixed;
        facts.clip_path = super::basic_shapes::compute_basic_shape_clip_path_data(layout_arena, slot, pixel_ratio)
            .map(|(path, bounding_rect, fill_rule)| (std::rc::Rc::new(path), bounding_rect, fill_rule));
        let converter = crate::painting::display_list::device_pixels::DevicePixelConverter::new(pixel_ratio);
        facts.mask_layers = super::node_values::mask_layer_presence(layout_arena, callbacks, slot, true)
            .into_iter()
            .map(|layer| MaskData {
                rect: converter.enclosing_device_rect(layer.area),
                kind: layer.kind,
                origin: layer.origin,
            })
            .collect();
        facts.css_clip = super::node_values::compute_css_clip_data(layout_arena, slot, pixel_ratio);
        facts.may_have_clip = super::node_values::may_have_clip(layout_arena, slot);
        facts.overflow_clip = if facts.may_have_clip {
            super::node_values::compute_clip_data(layout_arena, slot, pixel_ratio)
        } else {
            None
        };
        facts
    }

    pub(crate) fn clip_path_data(&self) -> Option<ClipPathData> {
        self.clip_path
            .as_ref()
            .map(|(path, bounding_rect, fill_rule)| ClipPathData {
                path: path.clone(),
                bounding_rect: *bounding_rect,
                fill_rule: *fill_rule,
            })
    }
}

pub(crate) struct FreshTree {
    pub tree: VisualContextTree,
    pub viewport_assignment: PaintableVisualContextAssignment,
}

pub(crate) fn create_fresh_tree_with_viewport_nodes(
    layout_arena: &impl PaintableRowsRead,
    viewport: NodeSlotId,
    inputs: &FfiVisualContextTreeInputs,
) -> FreshTree {
    let mut tree = VisualContextTree::create(super::node_values::visual_viewport_transform_data(inputs));
    let root_isolation_frame = tree.append_frame(
        FrameData::layer_blending_with(CompositingAndBlendingOperator::Normal),
        FrameNodeIndex::NONE,
        VISUAL_VIEWPORT_NODE_INDEX,
    );
    tree.root_isolation_frame = Some(root_isolation_frame);
    let root_context = ContextRef {
        spatial: VISUAL_VIEWPORT_NODE_INDEX,
        frame: root_isolation_frame,
    };

    let viewport_scroll_node = tree.append_spatial(
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: viewport,
            registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
        }),
        VISUAL_VIEWPORT_NODE_INDEX,
    );
    let viewport_state_for_descendants = ContextRef {
        spatial: viewport_scroll_node,
        frame: root_isolation_frame,
    };

    let viewport_nearest_scroll_nodes = NearestScrollNodeIndices {
        stopping_at_fixed_position_ancestors: viewport_scroll_node,
        continuing_through_fixed_position_ancestors: viewport_scroll_node,
    };
    let viewport_contexts = DescendantVisualContexts {
        normal: viewport_state_for_descendants,
        absolute_position: viewport_state_for_descendants,
        fixed_position: root_context,
        normal_nearest_scroll_nodes: viewport_nearest_scroll_nodes,
        absolute_position_nearest_scroll_nodes: viewport_nearest_scroll_nodes,
        fixed_position_nearest_scroll_nodes: viewport_nearest_scroll_nodes,
        normal_plane_root: viewport_scroll_node,
        absolute_position_plane_root: viewport_scroll_node,
        fixed_position_plane_root: VISUAL_VIEWPORT_NODE_INDEX,
        flattens_inherited_transform: true,
        sorting_context_root: None,
        enclosing_stacking_context: viewport,
    };
    let mut viewport_assignment = PaintableVisualContextAssignment::from_data(
        viewport,
        layout_arena.paintable_data(viewport),
        PaintableVisualContextRecord {
            inherited_input: viewport_contexts,
            output_for_descendants: viewport_contexts,
            node_handles: BoxVisualContextNodeHandles::default(),
            has_mask_nodes: false,
            may_be_root_element: false,
            owns_geometry_dependent_nodes: false,
            subtree_may_own_geometry_dependent_nodes: false,
            stacking_context: crate::painting::stacking_context::StackingContextFacts::for_viewport(),
        },
    );
    viewport_assignment.enclosing_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
    viewport_assignment.own_scroll_node_index = viewport_scroll_node;
    viewport_assignment.has_accumulated_visual_context = true;
    viewport_assignment.accumulated_visual_context = root_context;
    viewport_assignment.accumulated_visual_context_for_descendants = viewport_state_for_descendants;
    FreshTree {
        tree,
        viewport_assignment,
    }
}
