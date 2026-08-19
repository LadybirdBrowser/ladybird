/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;

use crate::layout::LayoutNodeArena;
use crate::painting::host::FfiVisualContextHostCallbacks;
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::PaintableSlotId;
use crate::painting::visual_context::build::{
    BoxFacts, compute_svg_viewport_transform_data, svg_viewport_transform_of,
};
use crate::painting::visual_context::{MaskLayerOrigin, TransformData, VisualContextData, VisualContextTree};

#[derive(Default)]
pub struct NestedAssignments {
    pub paintable_indices: HashMap<u32, (usize, usize)>,
    pub mask_node_indices: HashMap<u32, Vec<usize>>,
}

struct NestedBuilder<'a> {
    layout_arena: &'a LayoutNodeArena,
    paintables: &'a PaintableArena,
    callbacks: &'a FfiVisualContextHostCallbacks,
    tree: VisualContextTree,
    assignments: NestedAssignments,
    pixel_ratio: f64,
}

impl NestedBuilder<'_> {
    fn build_subtree(&mut self, slot: PaintableSlotId, inherited_state: usize, include_element_transform: bool) {
        let data = self.paintables.data_ref(slot);
        let facts = BoxFacts::gather(
            self.layout_arena,
            self.paintables,
            self.callbacks,
            slot,
            self.pixel_ratio,
        );
        let mut own_state = inherited_state;
        if let Some(effects) = facts.effects_data() {
            own_state = self.tree.append(VisualContextData::Effects(effects), inherited_state);
        }

        if include_element_transform && let Some(transform) = facts.transform {
            own_state = self.tree.append(VisualContextData::Transform(transform), own_state);
        }

        for mask_layer in facts
            .mask_layers
            .iter()
            .filter(|layer| layer.origin != MaskLayerOrigin::CssMaskLayers)
        {
            own_state = self.tree.append(VisualContextData::Mask(*mask_layer), own_state);
            self.assignments
                .mask_node_indices
                .entry(slot.index)
                .or_default()
                .push(own_state);
        }

        let mut state_for_descendants = own_state;

        if facts.may_have_clip
            && let Some(clip) = facts.overflow_clip
        {
            state_for_descendants = self.tree.append(VisualContextData::Clip(clip), state_for_descendants);
        }

        if let Some(svg_viewport_transform) = svg_viewport_transform_of(&data) {
            let viewport_transform_data =
                compute_svg_viewport_transform_data(self.paintables, slot, svg_viewport_transform, self.pixel_ratio);
            state_for_descendants = self.tree.append(
                VisualContextData::Transform(viewport_transform_data),
                state_for_descendants,
            );
        }

        self.assignments
            .paintable_indices
            .insert(slot.index, (own_state, state_for_descendants));

        let mut child = self.paintables.first_child(slot);
        while let Some(current) = child {
            self.build_subtree(current, state_for_descendants, true);
            child = self.paintables.next_sibling(current);
        }
    }
}

pub(crate) fn build_nested_svg_visual_context_tree(
    layout_arena: &LayoutNodeArena,
    paintables: &PaintableArena,
    callbacks: &FfiVisualContextHostCallbacks,
    root: PaintableSlotId,
    root_transform: TransformData,
    include_root_element_transform: bool,
    pixel_ratio: f64,
) -> (VisualContextTree, NestedAssignments) {
    let mut builder = NestedBuilder {
        layout_arena,
        paintables,
        callbacks,
        tree: VisualContextTree::create_with_content_root(root_transform),
        assignments: NestedAssignments::default(),
        pixel_ratio,
    };
    builder.build_subtree(root, 0, include_root_element_transform);
    (builder.tree, builder.assignments)
}
