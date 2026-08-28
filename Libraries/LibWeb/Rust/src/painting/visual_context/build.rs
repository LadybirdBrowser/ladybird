/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::box_build::{
    AnchorScrollShiftResolver, BoxBuildEnvironment, PaintableVisualContextAssignment, build_box_visual_context_nodes,
};
use super::refresh::compute_sticky_data;
use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState};
use super::*;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::host::{FfiVisualContextHostCallbacks, FfiVisualContextTreeInputs};
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use libgfx_rust::{
    AffineTransform, CompositingAndBlendingOperator, FloatPoint, IntRect, WindingRule, affine_to_matrix,
    scale_matrix_for_device_pixels, translated_then_multiplied,
};
use std::collections::HashSet;

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

struct FullBuildAnchorScrollShiftResolver<'a, Arena> {
    layout_arena: &'a Arena,
    callbacks: &'a FfiVisualContextHostCallbacks,
    scroll_state: &'a ScrollState,
    assignments: &'a [Option<PaintableVisualContextAssignment>],
    may_have_default_scroll_shift_anchor: bool,
}

impl<Arena: PaintableRowsRead> AnchorScrollShiftResolver for FullBuildAnchorScrollShiftResolver<'_, Arena> {
    fn default_scroll_shift_anchor(&self, slot: NodeSlotId) -> NodeSlotId {
        if !self.may_have_default_scroll_shift_anchor {
            return NodeSlotId::INVALID;
        }
        self.callbacks
            .default_scroll_shift_anchor(self.layout_arena.shell_if_live(slot))
    }

    fn enclosing_scroll_node_index(&self, slot: NodeSlotId) -> SpatialNodeIndex {
        self.assignments[slot.slot_index() as usize].as_ref().map_or_else(
            || self.layout_arena.paintable_data(slot).enclosing_scroll_node_index,
            |assignment| assignment.enclosing_scroll_node_index,
        )
    }

    fn scroll_state(&self) -> &ScrollState {
        self.scroll_state
    }
}

struct Builder<'a, Arena> {
    layout_arena: &'a Arena,
    callbacks: &'a FfiVisualContextHostCallbacks,
    tree: VisualContextTree,
    scroll_state: ScrollState,
    paintables_with_mask_nodes: Vec<NodeSlotId>,
    assignments: Vec<Option<PaintableVisualContextAssignment>>,
    pixel_ratio: f64,
    tree_inputs: FfiVisualContextTreeInputs,
    root_background_source: crate::painting::host::FfiRootBackgroundSource,
    may_have_default_scroll_shift_anchor: bool,
}

impl<Arena: PaintableRowsRead> Builder<'_, Arena> {
    fn default_scroll_shift_anchor(&self, slot: NodeSlotId) -> NodeSlotId {
        if !self.may_have_default_scroll_shift_anchor {
            return NodeSlotId::INVALID;
        }
        self.callbacks
            .default_scroll_shift_anchor(self.layout_arena.shell_if_live(slot))
    }

    fn register_scroll_node(
        &mut self,
        node_index: SpatialNodeIndex,
        paintable: NodeSlotId,
        parent_index: SpatialNodeIndex,
    ) {
        let parent_slot = self.tree.scroll_state_slot_for_node(parent_index);
        let slot = self
            .scroll_state
            .register_scroll_node(node_index, paintable, parent_slot);
        if let SpatialData::Scroll(scroll) = &mut self.tree.spatial_nodes[node_index.0 as usize].data {
            scroll.state_slot = slot;
        }
        if self.layout_arena.node_kind_if_live(paintable) != Some(NodeKind::Viewport)
            && let Some(style) = self.layout_arena.node_style_if_live(paintable)
        {
            use crate::css::css_enums::overflow;
            let box_values = style.box_values();
            if matches!(box_values.overflow_x, overflow::AUTO | overflow::SCROLL)
                || matches!(box_values.overflow_y, overflow::AUTO | overflow::SCROLL)
            {
                self.scroll_state.has_non_viewport_wheel_scroll_target_candidate = true;
            }
        }
    }

    fn register_sticky_node(
        &mut self,
        node_index: SpatialNodeIndex,
        paintable: NodeSlotId,
        parent_index: SpatialNodeIndex,
    ) {
        let parent_slot = self.tree.scroll_state_slot_for_node(parent_index);
        let slot = self
            .scroll_state
            .register_sticky_node(node_index, paintable, parent_slot);
        self.tree.spatial_nodes[node_index.0 as usize].data = SpatialData::Sticky(compute_sticky_data(
            self.layout_arena,
            &self.scroll_state,
            slot,
            &self.tree_inputs,
        ));
    }

    fn register_scroll_like_nodes(&mut self, spatial_handles: &[SpatialNodeIndex]) {
        for node_index in spatial_handles.iter().copied() {
            match &self.tree.spatial_nodes[node_index.0 as usize].data {
                SpatialData::Scroll(scroll) => {
                    let (owner_paintable, registry_parent_node) = (scroll.owner_paintable, scroll.registry_parent_node);
                    self.register_scroll_node(node_index, owner_paintable, registry_parent_node);
                }
                SpatialData::Sticky(sticky) => {
                    let (owner_paintable, registry_parent_node) = (sticky.owner_paintable, sticky.registry_parent_node);
                    self.register_sticky_node(node_index, owner_paintable, registry_parent_node);
                }
                _ => {}
            }
        }
    }

    fn build_paintable_box(
        &mut self,
        slot: NodeSlotId,
        inherited: DescendantVisualContexts,
        may_be_root_element: bool,
    ) -> DescendantVisualContexts {
        let environment = BoxBuildEnvironment {
            layout_arena: self.layout_arena,
            callbacks: self.callbacks,
            pixel_ratio: self.pixel_ratio,
            tree_inputs: self.tree_inputs,
            root_background_source: self.root_background_source,
        };
        let output = {
            let resolver = FullBuildAnchorScrollShiftResolver {
                layout_arena: self.layout_arena,
                callbacks: self.callbacks,
                scroll_state: &self.scroll_state,
                assignments: &self.assignments,
                may_have_default_scroll_shift_anchor: self.may_have_default_scroll_shift_anchor,
            };
            build_box_visual_context_nodes(
                &environment,
                &mut self.tree,
                slot,
                inherited,
                may_be_root_element,
                Some(&resolver),
            )
        };
        self.register_scroll_like_nodes(&output.assignment.record.node_handles.spatial);
        if output.assignment.record.has_mask_nodes {
            self.paintables_with_mask_nodes.push(slot);
        }
        self.assignments[slot.slot_index() as usize] = Some(output.assignment);
        output.descendant_contexts
    }

    fn has_default_scroll_shift_anchor(&self, slot: NodeSlotId) -> bool {
        !self.default_scroll_shift_anchor(slot).is_invalid()
    }
}

#[derive(Clone, Copy)]
struct PendingPaintable {
    paintable: NodeSlotId,
    inherited: DescendantVisualContexts,
    may_be_root_element: bool,
}

pub(crate) fn build_visual_context_tree(
    layout_arena: &impl PaintableRowsRead,
    callbacks: &FfiVisualContextHostCallbacks,
    viewport: NodeSlotId,
) -> (
    VisualContextTree,
    ScrollState,
    Vec<NodeSlotId>,
    Vec<PaintableVisualContextAssignment>,
) {
    let inputs = callbacks.tree_inputs();
    let mut builder = Builder {
        layout_arena,
        callbacks,
        tree: VisualContextTree::create(super::node_values::visual_viewport_transform_data(&inputs)),
        scroll_state: ScrollState::default(),
        paintables_with_mask_nodes: Vec::new(),
        assignments: vec![None; layout_arena.paintable_row_count()],
        pixel_ratio: inputs.device_pixels_per_css_pixel,
        tree_inputs: inputs,
        root_background_source: callbacks.root_background_source(),
        may_have_default_scroll_shift_anchor: inputs.may_have_default_scroll_shift_anchor,
    };

    let root_isolation_frame = builder.tree.append_frame_with_role(
        FrameData::layer_blending_with(CompositingAndBlendingOperator::Normal),
        FrameNodeIndex::NONE,
        VISUAL_VIEWPORT_NODE_INDEX,
        FrameRole::RootIsolation,
    );
    builder.tree.root_isolation_frame = Some(root_isolation_frame);
    let root_context = ContextRef {
        spatial: VISUAL_VIEWPORT_NODE_INDEX,
        frame: root_isolation_frame,
    };

    let viewport_scroll_node = builder.tree.append_spatial(
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
            owner_paintable: viewport,
            registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
        }),
        VISUAL_VIEWPORT_NODE_INDEX,
    );
    builder.register_scroll_node(viewport_scroll_node, viewport, VISUAL_VIEWPORT_NODE_INDEX);
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
    };
    {
        let mut viewport_assignment = PaintableVisualContextAssignment::from_data(
            viewport,
            layout_arena.paintable_data(viewport),
            PaintableVisualContextRecord {
                inherited_input: viewport_contexts,
                output_for_descendants: viewport_contexts,
                node_handles: BoxVisualContextNodeHandles::default(),
                has_mask_nodes: false,
                may_be_root_element: false,
            },
        );
        viewport_assignment.enclosing_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
        viewport_assignment.own_scroll_node_index = viewport_scroll_node;
        viewport_assignment.has_accumulated_visual_context = true;
        viewport_assignment.accumulated_visual_context = root_context;
        viewport_assignment.accumulated_visual_context_for_descendants = viewport_state_for_descendants;
        builder.assignments[viewport.slot_index() as usize] = Some(viewport_assignment);
    }

    // Anchor-positioned boxes emit AnchorScrollShift nodes by reading the enclosing scroll nodes of
    // their anchors, and an acceptable anchor may come later in tree order than the positioned box.
    // Building such boxes' subtrees is deferred until their anchors have been built.
    let mut deferred_anchor_positioned: Vec<PendingPaintable> = Vec::new();
    let mut deferred_awaiting_build: HashSet<NodeSlotId> = HashSet::new();

    fn build_deferring_anchor_positioned(
        builder: &mut Builder<'_, impl PaintableRowsRead>,
        stack: &mut Vec<PendingPaintable>,
        exempt: Option<NodeSlotId>,
        deferred: &mut Vec<PendingPaintable>,
        awaiting: &mut HashSet<NodeSlotId>,
    ) {
        while let Some(pending) = stack.pop() {
            if Some(pending.paintable) != exempt && builder.has_default_scroll_shift_anchor(pending.paintable) {
                deferred.push(pending);
                awaiting.insert(pending.paintable);
                continue;
            }
            let child_contexts =
                builder.build_paintable_box(pending.paintable, pending.inherited, pending.may_be_root_element);
            let mut children = Vec::new();
            crate::painting::paint_order::for_each_paint_child(builder.layout_arena, pending.paintable, |child| {
                children.push(child);
            });
            for child in children.into_iter().rev() {
                stack.push(PendingPaintable {
                    paintable: child,
                    inherited: child_contexts,
                    may_be_root_element: false,
                });
            }
        }
    }

    let mut pending: Vec<PendingPaintable> = Vec::new();
    let mut viewport_children = Vec::new();
    crate::painting::paint_order::for_each_paint_child(builder.layout_arena, viewport, |child| {
        viewport_children.push(child);
    });
    for child in viewport_children.into_iter().rev() {
        pending.push(PendingPaintable {
            paintable: child,
            inherited: viewport_contexts,
            may_be_root_element: true,
        });
    }
    build_deferring_anchor_positioned(
        &mut builder,
        &mut pending,
        None,
        &mut deferred_anchor_positioned,
        &mut deferred_awaiting_build,
    );

    fn anchor_is_awaiting_build(
        builder: &Builder<'_, impl PaintableRowsRead>,
        slot: NodeSlotId,
        awaiting: &HashSet<NodeSlotId>,
    ) -> bool {
        let anchor_node = builder.default_scroll_shift_anchor(slot);
        let mut paintable = builder
            .layout_arena
            .paintable_row_is_populated(anchor_node)
            .then_some(anchor_node);
        while let Some(current) = paintable {
            if awaiting.contains(&current) {
                return true;
            }
            paintable = crate::painting::paint_order::paint_parent(builder.layout_arena, current);
        }
        false
    }

    while !deferred_anchor_positioned.is_empty() {
        let entries = std::mem::take(&mut deferred_anchor_positioned);
        let mut still_deferred = Vec::new();
        for entry in &entries {
            if anchor_is_awaiting_build(&builder, entry.paintable, &deferred_awaiting_build) {
                still_deferred.push(*entry);
            } else {
                deferred_awaiting_build.remove(&entry.paintable);
                pending.clear();
                pending.push(*entry);
                build_deferring_anchor_positioned(
                    &mut builder,
                    &mut pending,
                    Some(entry.paintable),
                    &mut deferred_anchor_positioned,
                    &mut deferred_awaiting_build,
                );
            }
        }
        let no_entry_was_ready = still_deferred.len() == entries.len();
        if no_entry_was_ready {
            // Cyclic or otherwise malformed anchor chains can leave every remaining entry waiting
            // on another; build them in queue order then.
            for entry in still_deferred {
                deferred_awaiting_build.remove(&entry.paintable);
                pending.clear();
                pending.push(entry);
                build_deferring_anchor_positioned(
                    &mut builder,
                    &mut pending,
                    Some(entry.paintable),
                    &mut deferred_anchor_positioned,
                    &mut deferred_awaiting_build,
                );
            }
        } else {
            deferred_anchor_positioned.extend(still_deferred);
        }
    }

    (
        builder.tree,
        builder.scroll_state,
        builder.paintables_with_mask_nodes,
        builder.assignments.into_iter().flatten().collect(),
    )
}

// Patches the transform/effects/perspective values of the box's existing visual context nodes in place.
// Returns false if the box's node structure no longer matches; the caller must then do a full rebuild.
pub(crate) fn update_visual_context_values(
    layout_arena: &impl PaintableRowsRead,
    callbacks: &FfiVisualContextHostCallbacks,
    tree: &mut VisualContextTree,
    slot: NodeSlotId,
    pixel_ratio: f64,
) -> (bool, Option<bool>) {
    let Some(node_handles) = layout_arena
        .paintable_visual_context_record(slot)
        .map(|record| record.node_handles.clone())
    else {
        return (false, None);
    };
    if node_handles
        .spatial
        .iter()
        .any(|index| index.0 as usize >= tree.spatial_nodes.len())
        || node_handles
            .frame_handles()
            .any(|index| index.0 as usize >= tree.frame_nodes.len())
    {
        return (false, None);
    }
    let transform_with_invertibility =
        super::node_values::compute_transform(layout_arena, callbacks, slot, pixel_ratio);
    let transform = transform_with_invertibility.map(|(transform, _)| transform);
    let transform_is_invertible = transform_with_invertibility.is_some_and(|(_, invertible)| invertible);
    let effects = super::node_values::compute_effects_data(layout_arena, callbacks, slot, pixel_ratio);
    let perspective = super::node_values::compute_perspective_data(layout_arena, slot, pixel_ratio);
    let svg_viewport_transform_data = svg_viewport_transform_of(layout_arena, slot)
        .map(|transform| compute_svg_viewport_transform_data(layout_arena, slot, transform, pixel_ratio));

    let has_non_invertible_css_transform = transform.is_some() && !transform_is_invertible;

    let compatible = (|| {
        let mut found_css_transform = false;
        let mut found_svg_viewport_transform = false;
        let mut found_effects = false;
        let mut found_perspective = false;
        for index in node_handles.spatial.iter().map(|index| index.0 as usize) {
            match &mut tree.spatial_nodes[index].data {
                SpatialData::Transform(transform_data) => {
                    if transform_data.role == TransformDataRole::SvgViewportTransform {
                        let Some(mut new_data) = svg_viewport_transform_data else {
                            return false;
                        };
                        new_data.flattens_inherited_transform = transform_data.flattens_inherited_transform;
                        *transform_data = new_data;
                        found_svg_viewport_transform = true;
                        continue;
                    }
                    // A synthetic plane node has no computed transform behind it. It stays as-is unless the element
                    // gained a real transform, which changes the structure the node was built for.
                    if transform_data.synthetic_plane {
                        if transform.is_some() {
                            return false;
                        }
                        continue;
                    }
                    let Some(mut new_data) = transform else {
                        return false;
                    };
                    new_data.flattens_inherited_transform = transform_data.flattens_inherited_transform;
                    new_data.sorting_context_root_index = transform_data.sorting_context_root_index;
                    new_data.establishes_sorting_context = transform_data.establishes_sorting_context;
                    *transform_data = new_data;
                    found_css_transform = true;
                }
                SpatialData::Perspective(perspective_data) => {
                    let Some(mut new_data) = perspective else {
                        return false;
                    };
                    new_data.flattens_inherited_transform = perspective_data.flattens_inherited_transform;
                    *perspective_data = new_data;
                    found_perspective = true;
                }
                _ => {}
            }
        }
        for index in node_handles.frame_handles().map(|index| index.0 as usize) {
            let node = &mut tree.frame_nodes[index];
            if node.role != FrameRole::Structural {
                continue;
            }
            // The builder duplicates a box's EffectsData into the positioned-descendant chains, so every
            // node of a kind is patched with the same recomputed payload.
            if let FrameData::Effects(effects_data) = &mut node.data {
                let Some(new_effects) = &effects else {
                    return false;
                };
                *effects_data = EffectsData {
                    opacity: new_effects.opacity,
                    blend_mode: new_effects.blend_mode,
                    filter: new_effects.filter.clone(),
                };
                found_effects = true;
            }
        }
        transform.is_some() == found_css_transform
            && effects.is_some() == found_effects
            && perspective.is_some() == found_perspective
            && svg_viewport_transform_data.is_some() == found_svg_viewport_transform
    })();
    (compatible, Some(has_non_invertible_css_transform))
}
