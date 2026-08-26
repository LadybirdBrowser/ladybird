/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;

use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiVisualContextHostCallbacks;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::visual_context::build::{
    BoxFacts, compute_svg_viewport_transform_data, svg_viewport_transform_of,
};
use crate::painting::visual_context::{
    ContextRef, FrameData, FrameNodeIndex, MaskLayerOrigin, SpatialData, TransformData, VisualContextTree,
};

#[derive(Default)]
pub struct NestedAssignments {
    pub paintable_contexts: HashMap<u32, (ContextRef, ContextRef)>,
    pub mask_frames: HashMap<u32, Vec<FrameNodeIndex>>,
}

struct NestedBuilder<'a, Arena> {
    layout_arena: &'a Arena,
    callbacks: &'a FfiVisualContextHostCallbacks,
    tree: VisualContextTree,
    assignments: NestedAssignments,
    pixel_ratio: f64,
}

impl<Arena: PaintableRowsRead> NestedBuilder<'_, Arena> {
    fn build_subtree(&mut self, slot: NodeSlotId, inherited_state: ContextRef, include_element_transform: bool) {
        let facts = BoxFacts::gather(self.layout_arena, self.callbacks, slot, self.pixel_ratio, false);
        let mut own_state = inherited_state;
        if let Some(effects) = facts.effects_data() {
            own_state = self.tree.append_frame_under(own_state, FrameData::Effects(effects));
        }

        if include_element_transform && let Some(transform) = facts.transform {
            own_state = self
                .tree
                .append_spatial_under(own_state, SpatialData::Transform(transform));
        }

        for mask_layer in facts
            .mask_layers
            .iter()
            .filter(|layer| layer.origin != MaskLayerOrigin::CssMaskLayers)
        {
            own_state = self.tree.append_frame_under(own_state, FrameData::Mask(*mask_layer));
            self.assignments
                .mask_frames
                .entry(slot.index)
                .or_default()
                .push(own_state.frame);
        }

        let mut state_for_descendants = own_state;

        if facts.may_have_clip
            && let Some(clip) = facts.overflow_clip
        {
            state_for_descendants = self
                .tree
                .append_frame_under(state_for_descendants, FrameData::Clip(clip));
        }

        if let Some(svg_viewport_transform) = svg_viewport_transform_of(self.layout_arena, slot) {
            let viewport_transform_data =
                compute_svg_viewport_transform_data(self.layout_arena, slot, svg_viewport_transform, self.pixel_ratio);
            state_for_descendants = self
                .tree
                .append_spatial_under(state_for_descendants, SpatialData::Transform(viewport_transform_data));
        }

        self.assignments
            .paintable_contexts
            .insert(slot.index, (own_state, state_for_descendants));

        let mut child = crate::painting::paint_order::first_paint_child(self.layout_arena, slot);
        while let Some(current) = child {
            self.build_subtree(current, state_for_descendants, true);
            child = crate::painting::paint_order::next_paint_sibling(self.layout_arena, current);
        }
    }
}

pub(crate) fn build_nested_svg_visual_context_tree(
    layout_arena: &impl PaintableRowsRead,
    callbacks: &FfiVisualContextHostCallbacks,
    root: NodeSlotId,
    root_transform: TransformData,
    include_root_element_transform: bool,
    pixel_ratio: f64,
) -> (VisualContextTree, NestedAssignments) {
    let mut builder = NestedBuilder {
        layout_arena,
        callbacks,
        tree: VisualContextTree::create_with_content_root(root_transform),
        assignments: NestedAssignments::default(),
        pixel_ratio,
    };
    builder.build_subtree(root, ContextRef::default(), include_root_element_transform);
    (builder.tree, builder.assignments)
}
