/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What WebContent last sent a context to draw, and the visual context tree it is drawn with: the one WebContent sent, with the visual viewport this context has pinch-zoomed or panned since, and with its visual animations sampled.

use libcompositing_rust::display_list::commands::SpatialNodeIndex;
use libcompositing_rust::display_list::layers::{LayerStack, UnvalidatedLayerPlan};
use libcompositing_rust::display_list::placement::spatial_nodes_whose_scroll_offset_placement_leaves_out;
use libcompositing_rust::scrolling::{
    AccumulatedVisualContextTree, DisplayList, ScrollStateSnapshot, TransformWithOrigin,
    animated_content_may_affect_viewport,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatMatrix4x4, IntRect, IntSize};
use std::io::Write;
use std::rc::Rc;

/// A plan of the layers of the display list a scene holds under the tree WebContent published, as WebContent sent it,
/// once it passed the checks against the two.
#[derive(Clone)]
pub struct ReceivedLayerPlan {
    /// The viewport, in device pixels, the plan was made within.
    pub viewport_rect: IntRect,
    pub stack: Rc<LayerStack>,
    /// The spatial nodes whose scroll offset the plan reads. It holds wherever the compositor scrolls or moves the
    /// other nodes to, which include the anchors of its spaces. Empty for a plan of one layer.
    pub nodes_whose_scroll_offset_plan_reads: Rc<[u32]>,
    /// What the overlay of layers tells of the stack past its layers, as WebContent wrote it.
    pub note_lines: Rc<[String]>,
}

#[derive(Default)]
pub struct ContextScene {
    display_list: Option<Rc<DisplayList>>,
    visual_context_tree: Option<AccumulatedVisualContextTree>,
    layer_plan: Option<ReceivedLayerPlan>,
    /// Whether the scene holds the display list and the tree of the update WebContent sent last, which the plans it
    /// sends with scroll states are of.
    holds_list_and_tree_sent_last: bool,
    async_visual_viewport_transform: Option<TransformWithOrigin>,
    visual_context_tree_for_compositing: Option<AccumulatedVisualContextTree>,
    sampled_visual_context_tree: Option<AccumulatedVisualContextTree>,
    visual_animation_sample_time_ns: Option<i64>,
    has_active_visual_animations: bool,
    animated_content_may_affect_viewport: Option<bool>,
}

fn update_visual_animation_sampling_state(
    visual_context_tree: &AccumulatedVisualContextTree,
    sample_time_ns: &mut Option<i64>,
    has_active_animations: &mut bool,
    now: MonotonicTime,
) {
    *has_active_animations = visual_context_tree.has_active_visual_animation_at(now.nanoseconds());
    // A replacement tree can be presented before the next vsync. Keep the most recent sample so that presentation
    // remains continuous, but sample dormant and completed descriptors at the current boundary when necessary.
    if !visual_context_tree.has_visual_animations() {
        *sample_time_ns = None;
    } else if !*has_active_animations {
        *sample_time_ns = Some(now.nanoseconds());
    }
}

impl ContextScene {
    pub fn install(
        &mut self,
        display_list: Rc<DisplayList>,
        visual_context_tree: AccumulatedVisualContextTree,
        now: MonotonicTime,
    ) {
        self.display_list = Some(display_list);
        self.layer_plan = None;
        self.animated_content_may_affect_viewport = None;
        self.take_visual_context_tree_unless_content_is_unchanged(visual_context_tree);
        update_visual_animation_sampling_state(
            self.visual_context_tree.as_ref().unwrap(),
            &mut self.visual_animation_sample_time_ns,
            &mut self.has_active_visual_animations,
            now,
        );
    }

    // Keeping the held tree keeps what frames cached under the number of its copy, and the main thread publishes most
    // lists with the content of the tree before.
    fn take_visual_context_tree_unless_content_is_unchanged(
        &mut self,
        visual_context_tree: AccumulatedVisualContextTree,
    ) {
        if self
            .visual_context_tree
            .as_ref()
            .is_some_and(|held| held.as_arc().has_same_content_as(visual_context_tree.as_arc()))
        {
            return;
        }
        self.invalidate_visual_context_tree_for_compositing();
        self.visual_context_tree = Some(visual_context_tree);
    }

    /// Returns false, leaving everything as it was, for a tree that does not fit the installed display list.
    pub fn replace_visual_context_tree(
        &mut self,
        visual_context_tree: AccumulatedVisualContextTree,
        now: MonotonicTime,
    ) -> bool {
        if self.display_list.as_ref().is_none_or(|list| {
            list.compatible_visual_context_tree_structural_epoch() != visual_context_tree.structural_epoch()
        }) {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping stale visual context tree update (tree epoch {}, display list epoch {})",
                visual_context_tree.structural_epoch(),
                self.display_list
                    .as_ref()
                    .map_or(0, |list| list.compatible_visual_context_tree_structural_epoch())
            );
            return false;
        }
        if self
            .display_list
            .as_ref()
            .is_some_and(|list| !visual_context_tree.has_every_node_the_runs_of_display_list_name(list))
        {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping visual context tree update without a node the display list names"
            );
            return false;
        }
        if self.display_list.as_ref().is_some_and(|list| {
            !list
                .storage()
                .effect_clip_plan_holds_under(visual_context_tree.as_arc())
        }) {
            let _ = writeln!(
                std::io::stderr(),
                "Compositor: Dropping visual context tree update that nests the effects of the display list otherwise"
            );
            return false;
        }
        self.layer_plan = None;
        self.take_visual_context_tree_unless_content_is_unchanged(visual_context_tree);
        update_visual_animation_sampling_state(
            self.visual_context_tree.as_ref().unwrap(),
            &mut self.visual_animation_sample_time_ns,
            &mut self.has_active_visual_animations,
            now,
        );
        true
    }

    // NB: WebContent sends a plan with each display list and tree it can plan for, and with a scroll state only where
    //     the viewport rect or a scroll offset that the plan it sent last reads changed. So a list or a tree that comes
    //     without a plan leaves the scene without one, and a scroll state that comes without one keeps the plan the
    //     scene holds. After a list or a tree that the compositor refuses, WebContent plans for what the scene does not
    //     hold: a plan that comes with a scroll state is then dropped, until a list or a tree is installed.

    /// Forgets the plan of layers, ahead of a display list or a tree that WebContent sent, which the compositor may
    /// refuse.
    pub fn forget_layer_plan(&mut self) {
        self.layer_plan = None;
        self.holds_list_and_tree_sent_last = false;
    }

    /// Takes the plan of layers that came with the display list or the tree the scene just installed. Fails for a plan
    /// that does not pass the checks against them.
    pub fn take_layer_plan_of_installed_update(
        &mut self,
        layer_plan: Option<UnvalidatedLayerPlan>,
    ) -> Result<(), &'static str> {
        self.holds_list_and_tree_sent_last = true;
        self.take_layer_plan(layer_plan)
    }

    /// Takes the plan of layers that came with a scroll state, if any. Fails for a plan that does not pass the checks
    /// against the display list and the tree it is of.
    pub fn take_layer_plan_of_scroll_state(
        &mut self,
        layer_plan: Option<UnvalidatedLayerPlan>,
    ) -> Result<(), &'static str> {
        match layer_plan {
            Some(layer_plan) if self.holds_list_and_tree_sent_last => self.take_layer_plan(Some(layer_plan)),
            _ => Ok(()),
        }
    }

    fn take_layer_plan(&mut self, layer_plan: Option<UnvalidatedLayerPlan>) -> Result<(), &'static str> {
        self.layer_plan = None;
        let (Some(layer_plan), Some(display_list), Some(tree)) =
            (layer_plan, &self.display_list, &self.visual_context_tree)
        else {
            return Ok(());
        };
        // A plan of another list is of one the compositor refused for a reason WebContent does not learn.
        if layer_plan.display_list_id != display_list.id() {
            return Ok(());
        }
        let layer_plan = layer_plan.validated(display_list.storage(), tree.as_arc())?;
        // The spaces of a plan of more than one layer are at the anchors that plans take for the tree. A plan of one
        // layer replays the whole display list, and no frame reads which scroll offsets it leaves out.
        let nodes_whose_scroll_offset_plan_reads = if layer_plan.stack.replays_whole_list() {
            Rc::default()
        } else {
            let anchors_of_spaces: Vec<SpatialNodeIndex> = layer_plan
                .stack
                .spaces
                .iter()
                .skip(1)
                .map(|space| space.anchor)
                .collect();
            spatial_nodes_whose_scroll_offset_placement_leaves_out(tree.as_arc(), &anchors_of_spaces)
                .into_iter()
                .enumerate()
                .filter(|&(_, leaves_out)| !leaves_out)
                .map(|(node_index, _)| node_index as u32)
                .collect()
        };
        self.layer_plan = Some(ReceivedLayerPlan {
            viewport_rect: layer_plan.viewport_rect,
            stack: Rc::new(layer_plan.stack),
            nodes_whose_scroll_offset_plan_reads,
            note_lines: layer_plan.note_lines.into(),
        });
        Ok(())
    }

    pub fn display_list(&self) -> &Option<Rc<DisplayList>> {
        &self.display_list
    }
    pub fn visual_context_tree(&self) -> &Option<AccumulatedVisualContextTree> {
        &self.visual_context_tree
    }

    pub fn current_visual_context_tree(&self) -> &AccumulatedVisualContextTree {
        assert!(self.display_list.is_some());
        assert!(self.visual_context_tree.is_some());
        self.visual_context_tree.as_ref().unwrap()
    }

    pub fn visual_context_tree_for_compositing(&mut self) -> &AccumulatedVisualContextTree {
        if let Some(transform) = self.async_visual_viewport_transform
            && self.visual_context_tree_for_compositing.is_none()
        {
            self.sampled_visual_context_tree = None;
            self.visual_context_tree_for_compositing = Some(
                self.current_visual_context_tree()
                    .with_visual_viewport_transform(&transform),
            );
        }
        let unsampled_tree = self
            .visual_context_tree_for_compositing
            .as_ref()
            .unwrap_or_else(|| self.visual_context_tree.as_ref().unwrap());
        let Some(sample_time_ns) = self.visual_animation_sample_time_ns else {
            return unsampled_tree;
        };
        if self.sampled_visual_context_tree.is_none() {
            self.sampled_visual_context_tree = Some(unsampled_tree.with_visual_animation_samples(sample_time_ns));
        }
        self.sampled_visual_context_tree.as_ref().unwrap()
    }

    /// The plan of layers of the display list and the tree as WebContent published it, unless frames draw the tree
    /// through a visual viewport transform other than its own.
    pub fn layer_plan_unless_visual_viewport_differs(&self) -> Option<&ReceivedLayerPlan> {
        let layer_plan = self.layer_plan.as_ref()?;
        let tree = self.visual_context_tree.as_ref()?;
        if let Some(transform) = self.async_visual_viewport_transform {
            let published_transform = tree.visual_viewport_transform();
            if transform.matrix != published_transform.matrix || transform.origin != published_transform.origin {
                return None;
            }
        }
        Some(layer_plan)
    }

    pub fn visual_viewport_transform(&self) -> TransformWithOrigin {
        if let Some(transform) = self.async_visual_viewport_transform {
            return transform;
        }
        if let Some(tree) = &self.visual_context_tree {
            return tree.visual_viewport_transform();
        }
        TransformWithOrigin {
            matrix: FloatMatrix4x4::identity(),
            origin: Default::default(),
        }
    }

    pub fn visual_viewport_scale(&self) -> Option<f32> {
        let tree = self.visual_context_tree.as_ref()?;
        if let Some(transform) = self.async_visual_viewport_transform {
            return Some(transform.matrix.elements[0][0]);
        }
        Some(tree.visual_viewport_transform().matrix.elements[0][0])
    }

    /// How many screen pixels a zoomed visual viewport shows one pixel of the layout viewport with. Scrollers scroll
    /// and stretch in the pixels of the layout viewport.
    pub fn visual_viewport_magnification(&self) -> f32 {
        let scale = self.visual_viewport_scale().unwrap_or(1.0);
        if scale < 1.0 { 1.0 } else { scale }
    }

    pub fn has_async_visual_viewport_transform(&self) -> bool {
        self.async_visual_viewport_transform.is_some()
    }

    pub fn set_async_visual_viewport_transform(&mut self, transform: TransformWithOrigin) {
        self.invalidate_visual_context_tree_for_compositing();
        self.async_visual_viewport_transform = Some(transform);
    }

    pub fn drop_async_visual_viewport_transform(&mut self) {
        self.invalidate_visual_context_tree_for_compositing();
        self.async_visual_viewport_transform = None;
    }

    pub fn advance_visual_animations(&mut self, now: MonotonicTime) -> bool {
        if !self.visual_context_tree.as_ref().unwrap().has_visual_animations() {
            return false;
        }
        self.sampled_visual_context_tree = None;
        self.visual_animation_sample_time_ns = Some(now.nanoseconds());
        self.visual_context_tree_for_compositing();
        self.has_active_visual_animations = self
            .visual_context_tree
            .as_ref()
            .unwrap()
            .has_active_visual_animation_at(now.nanoseconds());
        self.has_active_visual_animations
    }

    pub fn visual_animations_need_frame(
        &mut self,
        scroll_state_snapshot: &ScrollStateSnapshot,
        viewport_size: IntSize,
        now: MonotonicTime,
    ) -> bool {
        if !self.has_active_visual_animations {
            return false;
        }
        if let Some(affects_viewport) = self.animated_content_may_affect_viewport {
            return affects_viewport;
        }
        let sample_time_ns = self.visual_animation_sample_time_ns.unwrap_or(now.nanoseconds());
        let transformed_tree;
        let tree = if let Some(transform) = self.async_visual_viewport_transform {
            transformed_tree = self
                .current_visual_context_tree()
                .with_visual_viewport_transform(&transform);
            &transformed_tree
        } else {
            self.current_visual_context_tree()
        };
        let effect = animated_content_may_affect_viewport(
            self.display_list.as_ref().unwrap(),
            tree,
            scroll_state_snapshot,
            IntRect::new(0, 0, viewport_size.width, viewport_size.height),
            sample_time_ns,
        );
        if effect.stable_until_scene_changes {
            self.animated_content_may_affect_viewport = Some(effect.may_affect_viewport);
        }
        effect.may_affect_viewport
    }

    pub fn forget_whether_animated_content_may_affect_viewport(&mut self) {
        self.animated_content_may_affect_viewport = None;
    }

    fn invalidate_visual_context_tree_for_compositing(&mut self) {
        self.animated_content_may_affect_viewport = None;
        self.sampled_visual_context_tree = None;
        self.visual_context_tree_for_compositing = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::display_list::builder::DisplayListBuilder;
    use libcompositing_rust::display_list::commands::{ContextRef, EffectNodeIndex, FillRect};
    use libcompositing_rust::display_list::layers::LayerPlan;
    use libcompositing_rust::display_list::layers::planning::InputsOfLayerPlan;
    use libcompositing_rust::display_list::storage::{ChunkIndexInList, DisplayListStorage, ListPosition, ListSpan};
    use libcompositing_rust::ipc_types::LayerPlanOnWire;
    use libcompositing_rust::visual_context::VisualContextTree;
    use libgfx_rust::{Color, CompositingAndBlendingOperator};
    use libipc_rust::wire::{Decoder, Encoder};
    use std::sync::Arc;

    const VIEWPORT: IntRect = IntRect::new(0, 0, 800, 600);
    const DISPLAY_LIST_ID: u64 = 7;

    fn scene_with_one_rect() -> (ContextScene, Arc<DisplayListStorage>, Arc<VisualContextTree>) {
        let tree = Arc::new(VisualContextTree::create_identity());
        let mut builder = DisplayListBuilder::new();
        builder.append(
            &FillRect {
                rect: IntRect::new(10, 10, 100, 100),
                color: Color::from_rgb(0, 128, 0),
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            ContextRef::default(),
        );
        let list = DisplayListStorage::recorded(&builder.finish(), tree.clone());
        let display_list = DisplayList::new(
            list.clone(),
            DISPLAY_LIST_ID,
            tree.structural_epoch,
            None,
            None,
            Default::default(),
        );
        let mut scene = ContextScene::default();
        scene.install(
            Rc::new(display_list),
            AccumulatedVisualContextTree::from_arc(tree.clone()),
            MonotonicTime::default(),
        );
        (scene, list, tree)
    }

    // A plan as the compositor receives it from WebContent.
    fn received(display_list_id: u64, stack: LayerStack) -> UnvalidatedLayerPlan {
        let mut encoder = Encoder::new();
        encoder
            .encode(&LayerPlanOnWire::Planned(Arc::new(LayerPlan {
                display_list_id,
                viewport_rect: VIEWPORT,
                stack,
                note_lines: Vec::new(),
            })))
            .unwrap();
        let (bytes, _) = encoder.finish();
        Decoder::new(&bytes, [])
            .decode::<LayerPlanOnWire>()
            .unwrap()
            .into_received()
    }

    fn stack_of_plan(scene: &ContextScene) -> Option<LayerStack> {
        scene
            .layer_plan_unless_visual_viewport_differs()
            .map(|layer_plan| (*layer_plan.stack).clone())
    }

    #[test]
    fn a_plan_is_taken_with_the_list_it_is_of_and_one_that_does_not_fit_it_is_refused() {
        let (mut scene, list, tree) = scene_with_one_rect();
        let planned = InputsOfLayerPlan::new(list.clone(), tree, &[], VIEWPORT).plan().stack;
        assert_eq!(planned.why_list_is_one_layer, None);
        let span_past_end_of_list = ListSpan {
            begin: ListPosition::default(),
            end: ListPosition::new(ChunkIndexInList(0), list.whole_span().end.offset + 8),
        };
        let stack_not_fitting_list = || {
            let mut stack = planned.clone();
            stack.bands[0].record_spans = vec![span_past_end_of_list];
            stack
        };

        scene.forget_layer_plan();
        assert_eq!(
            scene.take_layer_plan_of_installed_update(Some(received(DISPLAY_LIST_ID, planned.clone()))),
            Ok(())
        );
        assert_eq!(stack_of_plan(&scene), Some(planned.clone()));
        // A scroll state without a plan keeps the plan.
        assert_eq!(scene.take_layer_plan_of_scroll_state(None), Ok(()));
        assert_eq!(stack_of_plan(&scene), Some(planned.clone()));

        // The stub of the message answers WebContent with misbehavior for a plan that does not pass the checks.
        assert!(
            scene
                .take_layer_plan_of_scroll_state(Some(received(DISPLAY_LIST_ID, stack_not_fitting_list())))
                .is_err()
        );
        assert_eq!(stack_of_plan(&scene), None);
        assert!(
            scene
                .take_layer_plan_of_installed_update(Some(received(DISPLAY_LIST_ID, stack_not_fitting_list())))
                .is_err()
        );

        // A plan of another list is of one the compositor refused, and goes unchecked.
        assert_eq!(
            scene.take_layer_plan_of_installed_update(Some(received(DISPLAY_LIST_ID + 1, stack_not_fitting_list()))),
            Ok(())
        );
        assert_eq!(stack_of_plan(&scene), None);

        // After a list or a tree the compositor may refuse, a plan with a scroll state is not of what the scene holds
        // until a plan comes with a list or a tree the scene installs.
        scene.forget_layer_plan();
        assert_eq!(
            scene.take_layer_plan_of_scroll_state(Some(received(DISPLAY_LIST_ID, stack_not_fitting_list()))),
            Ok(())
        );
        assert_eq!(stack_of_plan(&scene), None);
        assert_eq!(scene.take_layer_plan_of_installed_update(None), Ok(()));
        assert_eq!(
            scene.take_layer_plan_of_scroll_state(Some(received(DISPLAY_LIST_ID, planned.clone()))),
            Ok(())
        );
        assert_eq!(stack_of_plan(&scene), Some(planned));
    }
}
