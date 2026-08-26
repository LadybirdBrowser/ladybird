/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::refresh::compute_sticky_data;
use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState, ScrollStateSlot};
use super::*;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::painting::host::FfiVisualContextHostCallbacks;
use crate::painting::paintable_data::*;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::{PaintableRowsRead, PaintableRowsWrite};
use libgfx_rust::{
    AffineTransform, FloatPoint, IntRect, WindingRule, affine_to_matrix, scale_matrix_for_device_pixels,
    translated_then_multiplied,
};
use std::collections::HashSet;

#[derive(Clone, Copy)]
pub(crate) struct PaintableVisualContextAssignment {
    slot: NodeSlotId,
    enclosing_scroll_node_index: SpatialNodeIndex,
    own_scroll_node_index: SpatialNodeIndex,
    has_accumulated_visual_context: bool,
    accumulated_visual_context: ContextRef,
    accumulated_visual_context_for_descendants: ContextRef,
    fixed_background_visual_context: ContextRef,
    has_fixed_background_visual_context: bool,
    has_scroll_offset_dependent_background: bool,
    spatial_nodes_begin: u32,
    spatial_nodes_end: u32,
    frame_nodes_begin: u32,
    frame_nodes_end: u32,
    has_non_invertible_css_transform: bool,
}

impl PaintableVisualContextAssignment {
    fn from_data(slot: NodeSlotId, data: &PaintableData) -> Self {
        Self {
            slot,
            enclosing_scroll_node_index: data.enclosing_scroll_node_index,
            own_scroll_node_index: data.own_scroll_node_index,
            has_accumulated_visual_context: data.has_accumulated_visual_context,
            accumulated_visual_context: data.accumulated_visual_context,
            accumulated_visual_context_for_descendants: data.accumulated_visual_context_for_descendants,
            fixed_background_visual_context: data.fixed_background_visual_context,
            has_fixed_background_visual_context: data.has_fixed_background_visual_context,
            has_scroll_offset_dependent_background: data.has_scroll_offset_dependent_background,
            spatial_nodes_begin: data.spatial_nodes_begin,
            spatial_nodes_end: data.spatial_nodes_end,
            frame_nodes_begin: data.frame_nodes_begin,
            frame_nodes_end: data.frame_nodes_end,
            has_non_invertible_css_transform: data.has_flag(PaintableFlag::HasNonInvertibleCssTransform),
        }
    }

    pub(crate) fn apply(self, layout_arena: &mut impl PaintableRowsWrite) {
        let data = layout_arena.paintable_data_mut(self.slot);
        data.enclosing_scroll_node_index = self.enclosing_scroll_node_index;
        data.own_scroll_node_index = self.own_scroll_node_index;
        data.has_accumulated_visual_context = self.has_accumulated_visual_context;
        data.accumulated_visual_context = self.accumulated_visual_context;
        data.accumulated_visual_context_for_descendants = self.accumulated_visual_context_for_descendants;
        data.fixed_background_visual_context = self.fixed_background_visual_context;
        data.has_fixed_background_visual_context = self.has_fixed_background_visual_context;
        data.has_scroll_offset_dependent_background = self.has_scroll_offset_dependent_background;
        data.spatial_nodes_begin = self.spatial_nodes_begin;
        data.spatial_nodes_end = self.spatial_nodes_end;
        data.frame_nodes_begin = self.frame_nodes_begin;
        data.frame_nodes_end = self.frame_nodes_end;
        data.set_flag(
            PaintableFlag::HasNonInvertibleCssTransform,
            self.has_non_invertible_css_transform,
        );
    }
}

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
    pub clip_path: Option<(std::rc::Rc<libgfx_rust::path::OwnedPath>, IntRect, WindingRule, bool)>,
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
        facts.clip_path = super::basic_shapes::compute_basic_shape_clip_path_data(layout_arena, slot, pixel_ratio).map(
            |(path, bounding_rect, fill_rule, bounds_are_empty)| {
                (std::rc::Rc::new(path), bounding_rect, fill_rule, bounds_are_empty)
            },
        );
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

    fn clip_path_data(&self) -> Option<ClipPathData> {
        self.clip_path
            .as_ref()
            .map(|(path, bounding_rect, fill_rule, empty)| ClipPathData {
                path: path.clone(),
                bounding_rect: *bounding_rect,
                fill_rule: *fill_rule,
                path_bounds_are_empty: *empty,
            })
    }
}

fn common_ancestor_slot_along_scroll_parent_chain(
    scroll_state: &ScrollState,
    a_slot: ScrollStateSlot,
    b_slot: ScrollStateSlot,
) -> ScrollStateSlot {
    let mut a_slot_and_ancestors = Vec::new();
    let mut slot = a_slot;
    loop {
        a_slot_and_ancestors.push(slot);
        if slot == NO_SCROLL_STATE_SLOT {
            break;
        }
        slot = scroll_state.state_at_slot(slot).parent_slot;
    }
    let mut slot = b_slot;
    loop {
        if a_slot_and_ancestors.contains(&slot) {
            return slot;
        }
        if slot == NO_SCROLL_STATE_SLOT {
            break;
        }
        slot = scroll_state.state_at_slot(slot).parent_slot;
    }
    NO_SCROLL_STATE_SLOT
}

// Nearest ancestor scroll node resolved along the containing block chain, drilled down alongside
// the visual context indices. A fixed-position ancestor decouples its subtree from all outer
// scrollers, but sticky boxes must still reference a scrollport through fixed-position ancestors
// for their sticky offset computation, so both resolutions are carried.
#[derive(Clone, Copy)]
struct NearestScrollNodeIndices {
    stopping_at_fixed_position_ancestors: SpatialNodeIndex,
    continuing_through_fixed_position_ancestors: SpatialNodeIndex,
}

#[derive(Clone, Copy)]
struct DescendantVisualContexts {
    normal: ContextRef,
    absolute_position: ContextRef,
    fixed_position: ContextRef,
    normal_nearest_scroll_nodes: NearestScrollNodeIndices,
    absolute_position_nearest_scroll_nodes: NearestScrollNodeIndices,
    fixed_position_nearest_scroll_nodes: NearestScrollNodeIndices,
    normal_plane_root: SpatialNodeIndex,
    absolute_position_plane_root: SpatialNodeIndex,
    fixed_position_plane_root: SpatialNodeIndex,
    flattens_inherited_transform: bool,
    sorting_context_root: Option<SpatialNodeIndex>,
}

struct Builder<'a, Arena> {
    layout_arena: &'a Arena,
    callbacks: &'a FfiVisualContextHostCallbacks,
    tree: VisualContextTree,
    scroll_state: ScrollState,
    paintables_with_mask_nodes: Vec<NodeSlotId>,
    assignments: Vec<Option<PaintableVisualContextAssignment>>,
    pixel_ratio: f64,
    root_background_source: crate::painting::host::FfiRootBackgroundSource,
    may_have_default_scroll_shift_anchor: bool,
}

impl<Arena: PaintableRowsRead> Builder<'_, Arena> {
    fn assignment_mut(&mut self, slot: NodeSlotId) -> &mut PaintableVisualContextAssignment {
        let index = slot.slot_index() as usize;
        if self.assignments[index].is_none() {
            let assignment = PaintableVisualContextAssignment::from_data(slot, self.layout_arena.paintable_data(slot));
            self.assignments[index] = Some(assignment);
        }
        self.assignments[index].as_mut().unwrap()
    }

    fn enclosing_scroll_node_index(&self, slot: NodeSlotId) -> SpatialNodeIndex {
        self.assignments[slot.slot_index() as usize].map_or_else(
            || self.layout_arena.paintable_data(slot).enclosing_scroll_node_index,
            |assignment| assignment.enclosing_scroll_node_index,
        )
    }

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
        let data = self.layout_arena.paintable_data(paintable);
        if data.kind != crate::painting::paintable_data::PaintableKind::ViewportPaintable
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
            self.pixel_ratio,
        ));
    }

    fn build_paintable_box(
        &mut self,
        slot: NodeSlotId,
        inherited: DescendantVisualContexts,
        may_be_root_element: bool,
    ) -> DescendantVisualContexts {
        let first_spatial_node_index = self.tree.spatial_nodes.len() as u32;
        let first_frame_node_index = self.tree.frame_nodes.len() as u32;
        let facts = BoxFacts::gather(
            self.layout_arena,
            self.callbacks,
            slot,
            self.pixel_ratio,
            self.may_have_default_scroll_shift_anchor,
        );
        let position = crate::painting::style_queries::position(self.layout_arena, slot);
        let is_fixed = position == crate::css::css_enums::positioning::FIXED;
        let is_absolute = position == crate::css::css_enums::positioning::ABSOLUTE;
        let is_sticky = position == crate::css::css_enums::positioning::STICKY;
        let has_sticky_insets = self.layout_arena.paintable_data(slot).has_sticky_insets;
        let layout_node = slot;
        {
            let assignment = self.assignment_mut(slot);
            assignment.enclosing_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
            assignment.own_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
            assignment.fixed_background_visual_context = ContextRef::default();
            assignment.has_fixed_background_visual_context = false;
            assignment.has_scroll_offset_dependent_background = false;
        }

        let mut nearest_scroll_nodes_for_descendants = if is_fixed {
            NearestScrollNodeIndices {
                stopping_at_fixed_position_ancestors: VISUAL_VIEWPORT_NODE_INDEX,
                continuing_through_fixed_position_ancestors: inherited
                    .fixed_position_nearest_scroll_nodes
                    .continuing_through_fixed_position_ancestors,
            }
        } else if is_absolute {
            inherited.absolute_position_nearest_scroll_nodes
        } else {
            inherited.normal_nearest_scroll_nodes
        };
        let nearest_ancestor_scroll_node_index = if is_sticky {
            nearest_scroll_nodes_for_descendants.continuing_through_fixed_position_ancestors
        } else {
            nearest_scroll_nodes_for_descendants.stopping_at_fixed_position_ancestors
        };
        if !is_fixed && !is_sticky {
            self.assignment_mut(slot).enclosing_scroll_node_index = nearest_ancestor_scroll_node_index;
        }

        let creates_sticky_scroll_node = is_sticky && has_sticky_insets;

        let inherited_state = if is_fixed {
            inherited.fixed_position
        } else if is_absolute {
            inherited.absolute_position
        } else {
            // In-flow and relatively positioned boxes inherit the normal descendant context from
            // their visual parent.
            inherited.normal
        };
        // Build this element's own state from inherited state.
        let mut own_state = inherited_state;

        {
            // https://drafts.csswg.org/css-anchor-position-1/#default-scroll-shift
            // After layout has been performed for abspos, it is additionally shifted by the default scroll shift, as if
            // affected by a transform (before any other transforms).
            // NB: The shift is the scroll movement of the frames between the box's containing block and its default
            //     anchor box. When the anchor is itself an anchor-positioned box, its layout position does not include
            //     its own paint-time shift, so each chained anchor's shift is emitted as well, masked to the axes that
            //     every link below it compensates in. The visited set and depth cap guard against malformed anchor chains.
            let mut box_node = layout_node;
            let mut compensate_horizontal_scroll = true;
            let mut compensate_vertical_scroll = true;
            let mut visited: Vec<NodeSlotId> = Vec::new();
            const MAX_ANCHOR_CHAIN_DEPTH: usize = 32;
            let mut anchor_node = facts.default_scroll_shift_anchor;
            while !box_node.is_invalid() && !visited.contains(&box_node) && visited.len() < MAX_ANCHOR_CHAIN_DEPTH {
                if anchor_node.is_invalid() {
                    break;
                }
                if !self.layout_arena.paintable_row_is_populated(box_node)
                    || !self.layout_arena.paintable_row_is_populated(anchor_node)
                {
                    break;
                }
                visited.push(box_node);
                let box_flags = self.layout_arena.node_flags_if_live(box_node);
                compensate_horizontal_scroll =
                    compensate_horizontal_scroll && box_flags & NodeFlag::CompensatesForHorizontalScroll as u32 != 0;
                compensate_vertical_scroll =
                    compensate_vertical_scroll && box_flags & NodeFlag::CompensatesForVerticalScroll as u32 != 0;
                let anchor_scroll_slot = self
                    .tree
                    .scroll_state_slot_for_node(self.enclosing_scroll_node_index(anchor_node));
                let base_scroll_slot = self
                    .tree
                    .scroll_state_slot_for_node(self.enclosing_scroll_node_index(box_node));
                let shared_scroll_slot = common_ancestor_slot_along_scroll_parent_chain(
                    &self.scroll_state,
                    anchor_scroll_slot,
                    base_scroll_slot,
                );
                let mut s = anchor_scroll_slot;
                while s != NO_SCROLL_STATE_SLOT && s != shared_scroll_slot {
                    own_state = self.tree.append_spatial_under(
                        own_state,
                        SpatialData::AnchorScrollShift(AnchorScrollShift {
                            scroll_node_index: self.scroll_state.node_index_for_slot(s),
                            negate: false,
                            compensate_horizontal_scroll,
                            compensate_vertical_scroll,
                        }),
                    );
                    s = self.scroll_state.state_at_slot(s).parent_slot;
                }
                let mut s = base_scroll_slot;
                while s != NO_SCROLL_STATE_SLOT && s != shared_scroll_slot {
                    own_state = self.tree.append_spatial_under(
                        own_state,
                        SpatialData::AnchorScrollShift(AnchorScrollShift {
                            scroll_node_index: self.scroll_state.node_index_for_slot(s),
                            negate: true,
                            compensate_horizontal_scroll,
                            compensate_vertical_scroll,
                        }),
                    );
                    s = self.scroll_state.state_at_slot(s).parent_slot;
                }
                box_node = anchor_node;
                anchor_node = self.default_scroll_shift_anchor(anchor_node);
            }
        }

        // Out-of-flow descendants can skip overflow and scroll clips from intermediate ancestors.
        let establishes_absolute_cb = facts.establishes_absolute_containing_block;
        let establishes_fixed_cb = facts.establishes_fixed_containing_block;
        let mut state_for_absolute_position_descendants = inherited.absolute_position;
        let mut state_for_fixed_position_descendants = inherited.fixed_position;

        macro_rules! append_frame_to_own_and_positioned_descendant_contexts {
            ($make:expr) => {{
                own_state = self.tree.append_frame_under(own_state, $make);
                if !establishes_absolute_cb {
                    state_for_absolute_position_descendants = self
                        .tree
                        .append_frame_under(state_for_absolute_position_descendants, $make);
                }
                if !establishes_fixed_cb {
                    state_for_fixed_position_descendants = self
                        .tree
                        .append_frame_under(state_for_fixed_position_descendants, $make);
                }
            }};
        }

        let mut sticky_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
        if creates_sticky_scroll_node {
            own_state = self.tree.append_spatial_under(
                own_state,
                SpatialData::Sticky(StickyData::unconstrained(
                    nearest_ancestor_scroll_node_index,
                    None,
                    NO_SCROLL_STATE_SLOT,
                )),
            );
            sticky_scroll_node_index = own_state.spatial;
            self.register_sticky_node(sticky_scroll_node_index, slot, nearest_ancestor_scroll_node_index);
            {
                self.assignment_mut(slot).enclosing_scroll_node_index = sticky_scroll_node_index;
            }
            nearest_scroll_nodes_for_descendants = NearestScrollNodeIndices {
                stopping_at_fixed_position_ancestors: sticky_scroll_node_index,
                continuing_through_fixed_position_ancestors: sticky_scroll_node_index,
            };
        }

        let transform_data = facts.transform;

        if facts.effects.is_some() {
            append_frame_to_own_and_positioned_descendant_contexts!(FrameData::Effects(facts.effects_data().unwrap()));
        }

        let flattens_inherited_transform = inherited.flattens_inherited_transform;

        let mut appended_transform_node = false;
        if let Some(mut transform) = transform_data {
            transform.flattens_inherited_transform = flattens_inherited_transform;
            transform.sorting_context_root_index = inherited.sorting_context_root;
            self.assignment_mut(slot).has_non_invertible_css_transform = !facts.transform_is_invertible;
            own_state = self
                .tree
                .append_spatial_under(own_state, SpatialData::Transform(transform));
            appended_transform_node = true;
        } else {
            self.assignment_mut(slot).has_non_invertible_css_transform = false;
        }

        let inherited_plane_root = if is_fixed {
            inherited.fixed_position_plane_root
        } else if is_absolute {
            inherited.absolute_position_plane_root
        } else {
            inherited.normal_plane_root
        };
        let mut appended_backface_marker = false;
        // https://drafts.csswg.org/css-transforms-2/#backface-visibility-property
        // NB: Whether the element's backface is visible depends on its accumulated 3D transformation matrix, which
        //     is only known at replay time once scroll offsets have been applied. The node recorded below marks the
        //     content to skip and stores the plane root from which that matrix is accumulated. The plane root bounds
        //     the accumulation to the element's 3D rendering context.
        // AD-HOC: The spec determines visibility from the sign of m33 in the accumulated matrix. That is wrong for
        //         matrices with a perspective component, so we test the z-component of the transformed plane normal
        //         instead. See: https://github.com/w3c/csswg-drafts/issues/917.
        if facts.backface_hidden {
            own_state = self.tree.append_spatial_under(
                own_state,
                SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                    plane_root_index: inherited_plane_root,
                    flattens_inherited_transform: !appended_transform_node && flattens_inherited_transform,
                }),
            );
            appended_backface_marker = true;
        }

        // https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
        // An element participates in a 3D rendering context if its parent establishes or extends a 3D rendering
        // context. The position of each element in that three-dimensional space is determined by accumulating the
        // transformation matrices up from the given element to the element that establishes the 3D rendering context.
        // NB: Children of an element that neither establishes nor extends a 3D rendering context start a new plane
        //     below the element's own transform. Anonymous boxes carry no element style and are invisible to 3D
        //     rendering contexts, so they pass the inherited plane through unchanged. Pseudo-element boxes carry
        //     their own style and participate normally. An inherited flatten is only materialized by an appended
        //     node, so an element that appends none keeps it pending for its descendants.
        let establishes_or_extends_3d_rendering_context = facts.establishes_or_extends_3d_rendering_context;
        let node_data_flags = self.layout_arena.node_flags_if_live(layout_node);
        let invisible_to_3d_rendering_contexts = node_data_flags & NodeFlag::Anonymous as u32 != 0
            && !self.layout_arena.node_is_generated_for_pseudo_element(layout_node);
        let plane_root_for_descendants =
            if establishes_or_extends_3d_rendering_context || invisible_to_3d_rendering_contexts {
                inherited_plane_root
            } else {
                own_state.spatial
            };
        let inherited_flatten_still_pending =
            flattens_inherited_transform && !appended_transform_node && !appended_backface_marker;
        let mut descendants_flatten_inherited_transform = if invisible_to_3d_rendering_contexts {
            flattens_inherited_transform
        } else {
            !establishes_or_extends_3d_rendering_context || inherited_flatten_still_pending
        };

        // https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
        // A 3D rendering context is established by a transformable element whose used value for transform-style
        // is preserve-3d and which itself is not part of a 3D rendering context. An element that establishes a
        // 3D rendering context also participates in that context.
        // NB: Every preserve-3d element renders into its own plane, so one without a transform of its own
        //     appends an identity transform node to provide that plane. The establishing element's own state
        //     serves as the context's root; replay sorts the content recorded under it as the context's z=0
        //     plane alongside the planes of the participants, whose transform nodes reference the root.
        let mut sorting_context_root_for_descendants = inherited.sorting_context_root;
        if !invisible_to_3d_rendering_contexts {
            if !establishes_or_extends_3d_rendering_context {
                sorting_context_root_for_descendants = None;
            } else {
                if !appended_transform_node {
                    own_state = self.tree.append_spatial_under(
                        own_state,
                        SpatialData::Transform(TransformData {
                            matrix: libgfx_rust::FloatMatrix4x4::identity(),
                            origin: FloatPoint::default(),
                            sorting_context_root_index: sorting_context_root_for_descendants,
                            flattens_inherited_transform,
                            role: TransformDataRole::CssTransform,
                            synthetic_plane: true,
                        }),
                    );
                }
                if sorting_context_root_for_descendants.is_none() {
                    sorting_context_root_for_descendants = Some(own_state.spatial);
                }
            }
        }

        if let Some(css_clip) = facts.css_clip {
            append_frame_to_own_and_positioned_descendant_contexts!(FrameData::Clip(css_clip));
        }

        if facts.clip_path.is_some() {
            append_frame_to_own_and_positioned_descendant_contexts!(FrameData::ClipPath(
                facts.clip_path_data().unwrap()
            ));
        }

        if !facts.mask_layers.is_empty() {
            self.paintables_with_mask_nodes.push(slot);
        }
        for mask_layer in &facts.mask_layers {
            append_frame_to_own_and_positioned_descendant_contexts!(FrameData::Mask(*mask_layer));
        }

        {
            let assignment = self.assignment_mut(slot);
            assignment.has_accumulated_visual_context = true;
            assignment.accumulated_visual_context = own_state;
        }

        if super::node_values::wants_fixed_background_visual_context(
            self.layout_arena,
            self.root_background_source,
            slot,
            may_be_root_element,
        ) {
            // Rooted above every scroll-like node on the box's root path, the background stays put
            // under scrolling while the box's own frames keep clipping it in their scrolled spaces.
            let mut fixed_background_spatial = own_state.spatial;
            let mut index = own_state.spatial;
            while index != VISUAL_VIEWPORT_NODE_INDEX {
                let node = &self.tree.spatial_nodes[index.0 as usize];
                if node.data.is_scroll_like() {
                    fixed_background_spatial = node.parent;
                }
                index = node.parent;
            }
            {
                let assignment = self.assignment_mut(slot);
                assignment.fixed_background_visual_context = ContextRef {
                    spatial: fixed_background_spatial,
                    frame: own_state.frame,
                };
                assignment.has_fixed_background_visual_context = true;
            }
        }

        if super::node_values::background_depends_on_live_scroll_offset(
            self.layout_arena,
            self.root_background_source,
            slot,
            may_be_root_element,
        ) {
            self.assignment_mut(slot).has_scroll_offset_dependent_background = true;
        }

        // Build state for descendants: own state + perspective + clip + scroll.
        let mut state_for_descendants = own_state;

        if let Some(mut perspective) = facts.perspective {
            perspective.flattens_inherited_transform = descendants_flatten_inherited_transform;
            descendants_flatten_inherited_transform = false;
            state_for_descendants = self
                .tree
                .append_spatial_under(state_for_descendants, SpatialData::Perspective(perspective));
        }

        if facts.may_have_clip
            && let Some(overflow_clip) = facts.overflow_clip
        {
            state_for_descendants = self
                .tree
                .append_frame_under(state_for_descendants, FrameData::Clip(overflow_clip));
        }

        if paintable_geometry::has_scrollable_overflow(self.layout_arena.paintable_data(slot)) {
            let parent_index = if creates_sticky_scroll_node {
                sticky_scroll_node_index
            } else {
                nearest_ancestor_scroll_node_index
            };
            state_for_descendants = self.tree.append_spatial_under(
                state_for_descendants,
                SpatialData::Scroll(ScrollData {
                    state_slot: NO_SCROLL_STATE_SLOT,
                }),
            );
            let scroll_node_index = state_for_descendants.spatial;
            self.register_scroll_node(scroll_node_index, slot, parent_index);
            self.assignment_mut(slot).own_scroll_node_index = scroll_node_index;
            nearest_scroll_nodes_for_descendants = NearestScrollNodeIndices {
                stopping_at_fixed_position_ancestors: scroll_node_index,
                continuing_through_fixed_position_ancestors: scroll_node_index,
            };
        }

        // Positioned descendants that escape into a viewport-establishing containing block lay
        // out in the box's own coordinate space, not the viewport's user units, so they hang
        // above the viewport transform node.
        let state_for_positioned_descendants = state_for_descendants;
        if let Some(svg_viewport_transform) = svg_viewport_transform_of(self.layout_arena, slot) {
            let mut viewport_transform_data =
                compute_svg_viewport_transform_data(self.layout_arena, slot, svg_viewport_transform, self.pixel_ratio);
            viewport_transform_data.flattens_inherited_transform = descendants_flatten_inherited_transform;
            descendants_flatten_inherited_transform = false;
            state_for_descendants = self
                .tree
                .append_spatial_under(state_for_descendants, SpatialData::Transform(viewport_transform_data));
        }

        {
            let spatial_nodes_end = self.tree.spatial_nodes.len() as u32;
            let frame_nodes_end = self.tree.frame_nodes.len() as u32;
            let assignment = self.assignment_mut(slot);
            assignment.accumulated_visual_context_for_descendants = state_for_descendants;
            assignment.spatial_nodes_begin = first_spatial_node_index;
            assignment.spatial_nodes_end = spatial_nodes_end;
            assignment.frame_nodes_begin = first_frame_node_index;
            assignment.frame_nodes_end = frame_nodes_end;
        }
        let mut absolute_position_nearest_scroll_nodes = inherited.absolute_position_nearest_scroll_nodes;
        let mut fixed_position_nearest_scroll_nodes = inherited.fixed_position_nearest_scroll_nodes;
        let mut absolute_position_plane_root = inherited.absolute_position_plane_root;
        let mut fixed_position_plane_root = inherited.fixed_position_plane_root;
        if establishes_absolute_cb {
            state_for_absolute_position_descendants = state_for_positioned_descendants;
            absolute_position_nearest_scroll_nodes = nearest_scroll_nodes_for_descendants;
            absolute_position_plane_root = plane_root_for_descendants;
        }
        if establishes_fixed_cb {
            state_for_fixed_position_descendants = state_for_positioned_descendants;
            fixed_position_nearest_scroll_nodes = nearest_scroll_nodes_for_descendants;
            fixed_position_plane_root = plane_root_for_descendants;
        }

        DescendantVisualContexts {
            normal: state_for_descendants,
            absolute_position: state_for_absolute_position_descendants,
            fixed_position: state_for_fixed_position_descendants,
            normal_nearest_scroll_nodes: nearest_scroll_nodes_for_descendants,
            absolute_position_nearest_scroll_nodes,
            fixed_position_nearest_scroll_nodes,
            normal_plane_root: plane_root_for_descendants,
            absolute_position_plane_root,
            fixed_position_plane_root,
            flattens_inherited_transform: descendants_flatten_inherited_transform,
            sorting_context_root: sorting_context_root_for_descendants,
        }
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
        root_background_source: callbacks.root_background_source(),
        may_have_default_scroll_shift_anchor: inputs.may_have_default_scroll_shift_anchor,
    };

    builder.assignment_mut(viewport).enclosing_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
    let viewport_scroll_node = builder.tree.append_spatial(
        SpatialData::Scroll(ScrollData {
            state_slot: NO_SCROLL_STATE_SLOT,
        }),
        VISUAL_VIEWPORT_NODE_INDEX,
    );
    builder.register_scroll_node(viewport_scroll_node, viewport, VISUAL_VIEWPORT_NODE_INDEX);
    let viewport_state_for_descendants = ContextRef::spatial_only(viewport_scroll_node);
    {
        let assignment = builder.assignment_mut(viewport);
        assignment.own_scroll_node_index = viewport_scroll_node;
        assignment.has_accumulated_visual_context = true;
        assignment.accumulated_visual_context = ContextRef::default();
        assignment.accumulated_visual_context_for_descendants = viewport_state_for_descendants;
    }

    let viewport_nearest_scroll_nodes = NearestScrollNodeIndices {
        stopping_at_fixed_position_ancestors: viewport_scroll_node,
        continuing_through_fixed_position_ancestors: viewport_scroll_node,
    };
    let viewport_contexts = DescendantVisualContexts {
        normal: viewport_state_for_descendants,
        absolute_position: viewport_state_for_descendants,
        fixed_position: ContextRef::default(),
        normal_nearest_scroll_nodes: viewport_nearest_scroll_nodes,
        absolute_position_nearest_scroll_nodes: viewport_nearest_scroll_nodes,
        fixed_position_nearest_scroll_nodes: viewport_nearest_scroll_nodes,
        normal_plane_root: viewport_scroll_node,
        absolute_position_plane_root: viewport_scroll_node,
        fixed_position_plane_root: VISUAL_VIEWPORT_NODE_INDEX,
        flattens_inherited_transform: true,
        sorting_context_root: None,
    };

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
    let (spatial_range, frame_range) = {
        let data = layout_arena.paintable_data(slot);
        (
            data.spatial_nodes_begin as usize..data.spatial_nodes_end as usize,
            data.frame_nodes_begin as usize..data.frame_nodes_end as usize,
        )
    };
    if spatial_range.end > tree.spatial_nodes.len() || frame_range.end > tree.frame_nodes.len() {
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
        for index in spatial_range {
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
        for index in frame_range {
            // The builder duplicates a box's EffectsData into the positioned-descendant chains, so every
            // node of a kind is patched with the same recomputed payload.
            if let FrameData::Effects(effects_data) = &mut tree.frame_nodes[index].data {
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
