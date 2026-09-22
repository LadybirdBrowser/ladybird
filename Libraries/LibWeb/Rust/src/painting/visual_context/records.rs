/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the main thread keeps alongside a visual context tree while it builds and updates it: the
//! document's state, and the records the builder keeps per box.

use std::rc::Rc;

use super::{ClipNodeIndex, ContextRef, EffectNodeIndex, SpatialNodeIndex, VisualContextTree, dirty, scroll_state};
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiCompositorAnimationPublishOutcome;
use crate::painting::visual_animation::VisualAnimation;

#[derive(Default)]
pub struct VisualContextState {
    pub tree: Option<Rc<VisualContextTree>>,
    pub paintables_with_mask_nodes: Vec<crate::layout::node_data::NodeSlotId>,
    pub scroll_state: scroll_state::ScrollState,
    pub needs_to_refresh_scroll_state: bool,
    pub build_count: u64,
    pub dirty_boxes: dirty::VisualContextDirtySet,
    pub incremental_update_count: u64,
    pub last_tree_inputs: Option<crate::painting::host::FfiVisualContextTreeInputs>,
    pub last_full_build_reason: dirty::VisualContextGlobalRebuildReason,
    pub quarantined_slots_are_releasable: bool,
    // The compositor animations the effects of the current update pass have published so far.
    pub pending_compositor_animations: Vec<VisualAnimation>,
    // The list the tree was last given, which the next pass compares its own against.
    pub published_compositor_animations: Vec<VisualAnimation>,
}

impl VisualContextState {
    pub fn structural_epoch(&self) -> u64 {
        self.tree.as_ref().map_or(0, |tree| tree.structural_epoch)
    }

    pub fn clear_scroll_state(&mut self) {
        self.scroll_state.clear();
        self.needs_to_refresh_scroll_state = true;
    }

    pub fn release_quarantined_slots_while_no_handle_is_retained(&mut self) {
        if !self.quarantined_slots_are_releasable {
            return;
        }
        self.quarantined_slots_are_releasable = false;
        if let Some(tree) = self.tree.as_mut() {
            Rc::make_mut(tree).release_quarantined_slots_after_recording();
        }
    }
}

/// Gives the tree the animations the update pass published, or none, the way the main thread
/// publishes them: a list the tree already carries changes nothing, and the outcome tells what
/// changed for the counters the document keeps.
pub fn publish_compositor_animations(
    visual_context: &mut VisualContextState,
    publish_pending: bool,
) -> FfiCompositorAnimationPublishOutcome {
    let animations = if publish_pending {
        std::mem::take(&mut visual_context.pending_compositor_animations)
    } else {
        visual_context.pending_compositor_animations.clear();
        Vec::new()
    };
    let Some(tree) = visual_context.tree.as_mut() else {
        return FfiCompositorAnimationPublishOutcome::default();
    };
    // A tree rebuilt under a new structural epoch dropped the animations it carried, so the ones this
    // pass produced are new to it even when the last pass produced the same.
    let published = &mut visual_context.published_compositor_animations;
    if tree.visual_animations() == animations.as_slice() {
        return FfiCompositorAnimationPublishOutcome::default();
    }
    let mut parameters_changed = published.len() != animations.len();
    let mut timing_anchors_changed = false;
    if !parameters_changed {
        for (before, after) in published.iter().zip(&animations) {
            if before.monotonic_time_at_anchor_ns != after.monotonic_time_at_anchor_ns
                || before.local_time_at_anchor_ms != after.local_time_at_anchor_ms
            {
                timing_anchors_changed = true;
            }
            if !before.has_same_animation_parameters(after) {
                parameters_changed = true;
                break;
            }
        }
    }
    *published = animations.clone();
    Rc::make_mut(tree).set_visual_animations(animations);
    FfiCompositorAnimationPublishOutcome {
        published: true,
        parameters_changed,
        timing_anchors_changed,
    }
}

// The nodes a box appended, by kind. The chain units end at the box's own accumulated
// context; the descendant units hold what only descendants record under (the overflow clip).
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct BoxVisualContextNodeHandles {
    pub spatial: Vec<SpatialNodeIndex>,
    pub chain_clips: Vec<ClipNodeIndex>,
    pub descendant_clips: Vec<ClipNodeIndex>,
    pub effects: Vec<EffectNodeIndex>,
}

pub static EMPTY_BOX_VISUAL_CONTEXT_NODE_HANDLES: BoxVisualContextNodeHandles = BoxVisualContextNodeHandles {
    spatial: Vec::new(),
    chain_clips: Vec::new(),
    descendant_clips: Vec::new(),
    effects: Vec::new(),
};

impl BoxVisualContextNodeHandles {
    pub fn clip_handles(&self) -> impl Iterator<Item = ClipNodeIndex> + '_ {
        self.chain_clips.iter().chain(&self.descendant_clips).copied()
    }
}

// Nearest ancestor scroll node resolved along the containing block chain, drilled down alongside
// the visual context indices. A fixed-position ancestor decouples its subtree from all outer
// scrollers, but sticky boxes must still reference a scrollport through fixed-position ancestors
// for their sticky offset computation, so both resolutions are carried.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct NearestScrollNodeIndices {
    pub stopping_at_fixed_position_ancestors: SpatialNodeIndex,
    pub continuing_through_fixed_position_ancestors: SpatialNodeIndex,
}

// One positioning chain; effects are shared by all three chains.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PositioningContext {
    pub spatial: SpatialNodeIndex,
    pub clip: ClipNodeIndex,
    pub nearest_scroll_nodes: NearestScrollNodeIndices,
    pub plane_root: SpatialNodeIndex,
}

impl PositioningContext {
    pub fn with_effect(self, effect: EffectNodeIndex) -> ContextRef {
        ContextRef {
            spatial: self.spatial,
            clip: self.clip,
            effect,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct DescendantVisualContexts {
    pub effect: EffectNodeIndex,
    pub normal: PositioningContext,
    pub absolute_position: PositioningContext,
    pub fixed_position: PositioningContext,
    pub flattens_inherited_transform: bool,
    pub sorting_context_root: Option<SpatialNodeIndex>,
    pub enclosing_stacking_context: NodeSlotId,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct PaintableVisualContextRecord {
    pub inherited_input: DescendantVisualContexts,
    pub output_for_descendants: DescendantVisualContexts,
    pub node_handles: BoxVisualContextNodeHandles,
    pub has_mask_nodes: bool,
    pub may_be_root_element: bool,
    pub owns_geometry_dependent_nodes: bool,
    pub subtree_may_own_geometry_dependent_nodes: bool,
    pub stacking_context: crate::painting::stacking_context::StackingContextFacts,
}
