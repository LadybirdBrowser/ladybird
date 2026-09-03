/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::scroll_state::{NO_SCROLL_STATE_SLOT, ScrollState, ScrollStateSlot};
use super::*;
use crate::layout::node_data::{NodeFlag, NodeSlotId};
use crate::painting::host::{FfiRootBackgroundSource, FfiVisualContextHostCallbacks};
use crate::painting::paintable_data::*;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::{PaintableRowsRead, PaintableRowsWrite};
use libgfx_rust::FloatPoint;

pub(crate) struct BoxBuildEnvironment<'a, Arena> {
    pub layout_arena: &'a Arena,
    pub callbacks: &'a FfiVisualContextHostCallbacks,
    pub pixel_ratio: f64,
    pub root_background_source: FfiRootBackgroundSource,
}

pub(crate) trait AnchorScrollShiftResolver {
    fn default_scroll_shift_anchor(&self, slot: NodeSlotId) -> NodeSlotId;
    fn enclosing_scroll_node_index(&self, slot: NodeSlotId) -> SpatialNodeIndex;
    fn scroll_state(&self) -> &ScrollState;
}

#[derive(Clone)]
pub(crate) struct PaintableVisualContextAssignment {
    pub slot: NodeSlotId,
    pub enclosing_scroll_node_index: SpatialNodeIndex,
    pub own_scroll_node_index: SpatialNodeIndex,
    pub has_accumulated_visual_context: bool,
    pub accumulated_visual_context: ContextRef,
    pub accumulated_visual_context_for_descendants: ContextRef,
    pub fixed_background_visual_context: ContextRef,
    pub has_fixed_background_visual_context: bool,
    pub has_scroll_offset_dependent_background: bool,
    pub has_non_invertible_css_transform: bool,
    pub record: PaintableVisualContextRecord,
}

impl PaintableVisualContextAssignment {
    pub(crate) fn from_data(slot: NodeSlotId, data: &PaintableData, record: PaintableVisualContextRecord) -> Self {
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
            has_non_invertible_css_transform: data.has_flag(PaintableFlag::HasNonInvertibleCssTransform),
            record,
        }
    }

    pub(crate) fn apply(self, layout_arena: &mut impl PaintableRowsWrite) {
        {
            let data = layout_arena.paintable_data_mut(self.slot);
            data.establishes_stacking_context = self.record.stacking_context.establishes_stacking_context;
            data.enclosing_scroll_node_index = self.enclosing_scroll_node_index;
            data.own_scroll_node_index = self.own_scroll_node_index;
            data.has_accumulated_visual_context = self.has_accumulated_visual_context;
            data.accumulated_visual_context = self.accumulated_visual_context;
            data.accumulated_visual_context_for_descendants = self.accumulated_visual_context_for_descendants;
            data.fixed_background_visual_context = self.fixed_background_visual_context;
            data.has_fixed_background_visual_context = self.has_fixed_background_visual_context;
            data.has_scroll_offset_dependent_background = self.has_scroll_offset_dependent_background;
            data.set_flag(
                PaintableFlag::HasNonInvertibleCssTransform,
                self.has_non_invertible_css_transform,
            );
        }
        layout_arena.set_paintable_visual_context_record(self.slot, self.record);
    }
}

pub(crate) struct BoxVisualContextBuildOutput {
    pub assignment: PaintableVisualContextAssignment,
    pub descendant_contexts: DescendantVisualContexts,
}

fn scroll_state_slot_for_spatial_node(sink: &impl VisualContextNodeSink, index: SpatialNodeIndex) -> ScrollStateSlot {
    if index == VISUAL_VIEWPORT_NODE_INDEX {
        return NO_SCROLL_STATE_SLOT;
    }
    match &sink.spatial_node_at(index).data {
        SpatialData::Scroll(scroll) => scroll.state_slot,
        SpatialData::Sticky(sticky) => sticky.state_slot,
        _ => panic!("spatial node {} is not a scroll-like node", index.0),
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

// https://drafts.csswg.org/css-anchor-position-1/#default-scroll-shift
// After layout has been performed for abspos, it is additionally shifted by the default scroll shift, as if
// affected by a transform (before any other transforms).
// NB: The shift is the scroll movement of the frames between the box's containing block and its default
//     anchor box. When the anchor is itself an anchor-positioned box, its layout position does not include
//     its own paint-time shift, so each chained anchor's shift is emitted as well, masked to the axes that
//     every link below it compensates in. The visited set and depth cap guard against malformed anchor chains.
fn append_anchor_scroll_shift_nodes<Arena: PaintableRowsRead, Sink: VisualContextNodeSink>(
    env: &BoxBuildEnvironment<'_, Arena>,
    sink: &mut Sink,
    resolver: &dyn AnchorScrollShiftResolver,
    layout_node: NodeSlotId,
    own_enclosing_scroll_node_index: SpatialNodeIndex,
    first_anchor_node: NodeSlotId,
    mut own_state: ContextRef,
) -> ContextRef {
    let scroll_state = resolver.scroll_state();
    let enclosing_scroll_node_index = |node: NodeSlotId| {
        if node == layout_node {
            own_enclosing_scroll_node_index
        } else {
            resolver.enclosing_scroll_node_index(node)
        }
    };
    let mut box_node = layout_node;
    let mut compensate_horizontal_scroll = true;
    let mut compensate_vertical_scroll = true;
    let mut visited: Vec<NodeSlotId> = Vec::new();
    const MAX_ANCHOR_CHAIN_DEPTH: usize = 32;
    let mut anchor_node = first_anchor_node;
    while !box_node.is_invalid() && !visited.contains(&box_node) && visited.len() < MAX_ANCHOR_CHAIN_DEPTH {
        if anchor_node.is_invalid() {
            break;
        }
        if !env.layout_arena.paintable_row_is_populated(box_node)
            || !env.layout_arena.paintable_row_is_populated(anchor_node)
        {
            break;
        }
        visited.push(box_node);
        let box_flags = env.layout_arena.node_flags_if_live(box_node);
        compensate_horizontal_scroll =
            compensate_horizontal_scroll && box_flags & NodeFlag::CompensatesForHorizontalScroll as u32 != 0;
        compensate_vertical_scroll =
            compensate_vertical_scroll && box_flags & NodeFlag::CompensatesForVerticalScroll as u32 != 0;
        let anchor_scroll_slot = scroll_state_slot_for_spatial_node(sink, enclosing_scroll_node_index(anchor_node));
        let base_scroll_slot = scroll_state_slot_for_spatial_node(sink, enclosing_scroll_node_index(box_node));
        let shared_scroll_slot =
            common_ancestor_slot_along_scroll_parent_chain(scroll_state, anchor_scroll_slot, base_scroll_slot);
        let mut s = anchor_scroll_slot;
        while s != NO_SCROLL_STATE_SLOT && s != shared_scroll_slot {
            own_state = sink.append_spatial_node_under(
                own_state,
                SpatialData::AnchorScrollShift(AnchorScrollShift {
                    scroll_node_index: scroll_state.node_index_for_slot(s),
                    negate: false,
                    compensate_horizontal_scroll,
                    compensate_vertical_scroll,
                }),
            );
            s = scroll_state.state_at_slot(s).parent_slot;
        }
        let mut s = base_scroll_slot;
        while s != NO_SCROLL_STATE_SLOT && s != shared_scroll_slot {
            own_state = sink.append_spatial_node_under(
                own_state,
                SpatialData::AnchorScrollShift(AnchorScrollShift {
                    scroll_node_index: scroll_state.node_index_for_slot(s),
                    negate: true,
                    compensate_horizontal_scroll,
                    compensate_vertical_scroll,
                }),
            );
            s = scroll_state.state_at_slot(s).parent_slot;
        }
        box_node = anchor_node;
        anchor_node = resolver.default_scroll_shift_anchor(anchor_node);
    }
    own_state
}

pub(crate) fn build_box_visual_context_nodes<Arena: PaintableRowsRead, Sink: VisualContextNodeSink>(
    env: &BoxBuildEnvironment<'_, Arena>,
    sink: &mut Sink,
    slot: NodeSlotId,
    inherited: DescendantVisualContexts,
    may_be_root_element: bool,
    anchor_scroll_shift_resolver: Option<&dyn AnchorScrollShiftResolver>,
) -> BoxVisualContextBuildOutput {
    let layout_arena = env.layout_arena;
    let first_spatial_node_index = sink.next_spatial_node_index().0;
    let first_frame_node_index = sink.next_frame_node_index().0;
    let facts = super::build::BoxFacts::gather(layout_arena, env.callbacks, slot, env.pixel_ratio, true);
    let position = crate::painting::style_queries::position(layout_arena, slot);
    let is_fixed = position == crate::css::css_enums::positioning::FIXED;
    let is_absolute = position == crate::css::css_enums::positioning::ABSOLUTE;
    let is_sticky = position == crate::css::css_enums::positioning::STICKY;
    let layout_node = slot;
    let stacking_context_facts = crate::painting::stacking_context::StackingContextFacts::gather(
        layout_arena,
        slot,
        inherited.enclosing_stacking_context,
    );
    let mut assignment = PaintableVisualContextAssignment::from_data(
        slot,
        layout_arena.paintable_data(slot),
        PaintableVisualContextRecord {
            inherited_input: inherited,
            output_for_descendants: inherited,
            node_handles: BoxVisualContextNodeHandles::default(),
            has_mask_nodes: false,
            may_be_root_element,
            owns_geometry_dependent_nodes: false,
            subtree_may_own_geometry_dependent_nodes: false,
            stacking_context: stacking_context_facts,
        },
    );
    assignment.enclosing_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
    assignment.own_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
    assignment.fixed_background_visual_context = ContextRef::default();
    assignment.has_fixed_background_visual_context = false;
    assignment.has_scroll_offset_dependent_background = false;

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
        assignment.enclosing_scroll_node_index = nearest_ancestor_scroll_node_index;
    }

    let creates_sticky_scroll_node = is_sticky;

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

    match anchor_scroll_shift_resolver {
        Some(resolver) => {
            own_state = append_anchor_scroll_shift_nodes(
                env,
                sink,
                resolver,
                layout_node,
                assignment.enclosing_scroll_node_index,
                facts.default_scroll_shift_anchor,
                own_state,
            );
        }
        None => debug_assert!(
            facts.default_scroll_shift_anchor.is_invalid(),
            "anchor-positioned boxes are only built with a scroll shift resolver"
        ),
    }

    // Out-of-flow descendants can skip overflow and scroll clips from intermediate ancestors.
    let establishes_absolute_cb = facts.establishes_absolute_containing_block;
    let establishes_fixed_cb = facts.establishes_fixed_containing_block;
    let mut state_for_absolute_position_descendants = inherited.absolute_position;
    let mut state_for_fixed_position_descendants = inherited.fixed_position;

    macro_rules! append_frame_to_own_and_positioned_descendant_contexts {
        ($make:expr) => {{
            own_state = sink.append_frame_node_under(own_state, $make);
            if !establishes_absolute_cb {
                state_for_absolute_position_descendants =
                    sink.append_frame_node_under(state_for_absolute_position_descendants, $make);
            }
            if !establishes_fixed_cb {
                state_for_fixed_position_descendants =
                    sink.append_frame_node_under(state_for_fixed_position_descendants, $make);
            }
        }};
    }

    let mut sticky_scroll_node_index = VISUAL_VIEWPORT_NODE_INDEX;
    if creates_sticky_scroll_node {
        own_state = sink.append_spatial_node_under(
            own_state,
            SpatialData::Sticky(StickyData::unconstrained(
                nearest_ancestor_scroll_node_index,
                None,
                NO_SCROLL_STATE_SLOT,
                slot,
                nearest_ancestor_scroll_node_index,
            )),
        );
        sticky_scroll_node_index = own_state.spatial;
        assignment.enclosing_scroll_node_index = sticky_scroll_node_index;
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
        transform.establishes_sorting_context =
            facts.establishes_or_extends_3d_rendering_context && inherited.sorting_context_root.is_none();
        assignment.has_non_invertible_css_transform = !facts.transform_is_invertible;
        own_state = sink.append_spatial_node_under(own_state, SpatialData::Transform(transform));
        appended_transform_node = true;
    } else {
        assignment.has_non_invertible_css_transform = false;
    }

    let inherited_plane_root = if is_fixed {
        inherited.fixed_position_plane_root
    } else if is_absolute {
        inherited.absolute_position_plane_root
    } else {
        inherited.normal_plane_root
    };
    let establishes_or_extends_3d_rendering_context = facts.establishes_or_extends_3d_rendering_context;
    let node_data_flags = layout_arena.node_flags_if_live(layout_node);
    let invisible_to_3d_rendering_contexts = node_data_flags & NodeFlag::Anonymous as u32 != 0
        && !layout_arena.node_is_generated_for_pseudo_element(layout_node);

    let mut appended_synthetic_plane = false;
    if !invisible_to_3d_rendering_contexts && establishes_or_extends_3d_rendering_context && !appended_transform_node {
        own_state = sink.append_spatial_node_under(
            own_state,
            SpatialData::Transform(TransformData {
                matrix: libgfx_rust::FloatMatrix4x4::identity(),
                origin: FloatPoint::default(),
                sorting_context_root_index: inherited.sorting_context_root,
                flattens_inherited_transform,
                role: TransformDataRole::CssTransform,
                synthetic_plane: true,
                establishes_sorting_context: inherited.sorting_context_root.is_none(),
            }),
        );
        appended_synthetic_plane = true;
    }

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
        own_state = sink.append_spatial_node_under(
            own_state,
            SpatialData::BackfaceVisibility(BackfaceVisibilityData {
                plane_root_index: inherited_plane_root,
                flattens_inherited_transform: !appended_transform_node
                    && !appended_synthetic_plane
                    && flattens_inherited_transform,
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
    let plane_root_for_descendants =
        if establishes_or_extends_3d_rendering_context || invisible_to_3d_rendering_contexts {
            inherited_plane_root
        } else {
            own_state.spatial
        };
    let inherited_flatten_still_pending = flattens_inherited_transform
        && !appended_transform_node
        && !appended_synthetic_plane
        && !appended_backface_marker;
    let mut descendants_flatten_inherited_transform = if invisible_to_3d_rendering_contexts {
        flattens_inherited_transform
    } else {
        !establishes_or_extends_3d_rendering_context || inherited_flatten_still_pending
    };

    // https://drafts.csswg.org/css-transforms-2/#3d-rendering-contexts
    // A 3D rendering context is established by a transformable element whose used value for transform-style
    // is preserve-3d and which itself is not part of a 3D rendering context. An element that establishes a
    // 3D rendering context also participates in that context.
    // NB: The establishing element's own state serves as the context's root; replay sorts the content
    //     recorded under it as the context's z=0 plane alongside the planes of the participants, whose
    //     transform nodes reference the root.
    let mut sorting_context_root_for_descendants = inherited.sorting_context_root;
    if !invisible_to_3d_rendering_contexts {
        if !establishes_or_extends_3d_rendering_context {
            sorting_context_root_for_descendants = None;
        } else if sorting_context_root_for_descendants.is_none() {
            sorting_context_root_for_descendants = Some(own_state.spatial);
        }
    }

    if let Some(css_clip) = facts.css_clip {
        append_frame_to_own_and_positioned_descendant_contexts!(FrameData::Clip(css_clip));
    }

    if facts.clip_path.is_some() {
        append_frame_to_own_and_positioned_descendant_contexts!(FrameData::ClipPath(facts.clip_path_data().unwrap()));
    }

    if !facts.mask_layers.is_empty() {
        assignment.record.has_mask_nodes = true;
    }
    for mask_layer in &facts.mask_layers {
        append_frame_to_own_and_positioned_descendant_contexts!(FrameData::Mask(*mask_layer));
    }

    if facts.needs_compositor_background_color_frame {
        append_frame_to_own_and_positioned_descendant_contexts!(FrameData::BackgroundColorAnimation);
    }

    assignment.has_accumulated_visual_context = true;
    assignment.accumulated_visual_context = own_state;
    let chain_frames_end = sink.next_frame_node_index().0;

    if super::node_values::wants_fixed_background_visual_context(
        layout_arena,
        env.root_background_source,
        slot,
        may_be_root_element,
    ) {
        // Rooted above every scroll-like node on the box's root path, the background stays put
        // under scrolling while the box's own frames keep clipping it in their scrolled spaces.
        let mut fixed_background_spatial = own_state.spatial;
        let mut index = own_state.spatial;
        while index != VISUAL_VIEWPORT_NODE_INDEX {
            let node = sink.spatial_node_at(index);
            if node.data.is_scroll_like() {
                fixed_background_spatial = node.parent;
            }
            index = node.parent;
        }
        assignment.fixed_background_visual_context = ContextRef {
            spatial: fixed_background_spatial,
            frame: own_state.frame,
        };
        assignment.has_fixed_background_visual_context = true;
    }

    if super::node_values::background_depends_on_live_scroll_offset(
        layout_arena,
        env.root_background_source,
        slot,
        may_be_root_element,
    ) {
        assignment.has_scroll_offset_dependent_background = true;
    }

    // Build state for descendants: own state + perspective + clip + scroll.
    let mut state_for_descendants = own_state;

    if let Some(mut perspective) = facts.perspective {
        perspective.flattens_inherited_transform = descendants_flatten_inherited_transform;
        descendants_flatten_inherited_transform = false;
        state_for_descendants =
            sink.append_spatial_node_under(state_for_descendants, SpatialData::Perspective(perspective));
    }

    if facts.may_have_clip
        && let Some(overflow_clip) = facts.overflow_clip
    {
        state_for_descendants = sink.append_frame_node_under(state_for_descendants, FrameData::Clip(overflow_clip));
    }

    if paintable_geometry::has_scrollable_overflow(layout_arena.paintable_data(slot)) {
        let parent_index = if creates_sticky_scroll_node {
            sticky_scroll_node_index
        } else {
            nearest_ancestor_scroll_node_index
        };
        state_for_descendants = sink.append_spatial_node_under(
            state_for_descendants,
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: slot,
                registry_parent_node: parent_index,
            }),
        );
        let scroll_node_index = state_for_descendants.spatial;
        assignment.own_scroll_node_index = scroll_node_index;
        nearest_scroll_nodes_for_descendants = NearestScrollNodeIndices {
            stopping_at_fixed_position_ancestors: scroll_node_index,
            continuing_through_fixed_position_ancestors: scroll_node_index,
        };
    }

    // Positioned descendants that escape into a viewport-establishing containing block lay
    // out in the box's own coordinate space, not the viewport's user units, so they hang
    // above the viewport transform node.
    let state_for_positioned_descendants = state_for_descendants;
    if let Some(svg_viewport_transform) = super::build::svg_viewport_transform_of(layout_arena, slot) {
        let mut viewport_transform_data = super::build::compute_svg_viewport_transform_data(
            layout_arena,
            slot,
            svg_viewport_transform,
            env.pixel_ratio,
        );
        viewport_transform_data.flattens_inherited_transform = descendants_flatten_inherited_transform;
        descendants_flatten_inherited_transform = false;
        state_for_descendants =
            sink.append_spatial_node_under(state_for_descendants, SpatialData::Transform(viewport_transform_data));
    }

    assignment.accumulated_visual_context_for_descendants = state_for_descendants;
    let spatial_end = sink.next_spatial_node_index().0;
    let descendant_frames_end = sink.next_frame_node_index().0;
    assignment.record.node_handles = BoxVisualContextNodeHandles {
        spatial: (first_spatial_node_index..spatial_end).map(SpatialNodeIndex).collect(),
        chain_frames: (first_frame_node_index..chain_frames_end).map(FrameNodeIndex).collect(),
        descendant_frames: (chain_frames_end..descendant_frames_end).map(FrameNodeIndex).collect(),
    };
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

    let descendant_contexts = DescendantVisualContexts {
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
        enclosing_stacking_context: if stacking_context_facts.establishes_stacking_context {
            slot
        } else {
            inherited.enclosing_stacking_context
        },
    };
    assignment.record.output_for_descendants = descendant_contexts;
    BoxVisualContextBuildOutput {
        assignment,
        descendant_contexts,
    }
}
