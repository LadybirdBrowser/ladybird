/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What WebContent last sent a context to draw, and the visual context tree it is drawn with: the one WebContent sent, with the visual viewport this context has pinch-zoomed or panned since, and with its visual animations sampled.

use libcompositing_rust::scrolling::{
    AccumulatedVisualContextTree, DisplayList, ScrollStateSnapshot, TransformWithOrigin,
    animated_content_may_affect_viewport,
};
use libcompositing_rust::time::MonotonicTime;
use libgfx_rust::{FloatMatrix4x4, IntRect, IntSize};
use std::io::Write;
use std::rc::Rc;

#[derive(Default)]
pub struct ContextScene {
    display_list: Option<Rc<DisplayList>>,
    visual_context_tree: Option<AccumulatedVisualContextTree>,
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
        self.invalidate_visual_context_tree_for_compositing();
        self.display_list = Some(display_list);
        self.visual_context_tree = Some(visual_context_tree);
        update_visual_animation_sampling_state(
            self.visual_context_tree.as_ref().unwrap(),
            &mut self.visual_animation_sample_time_ns,
            &mut self.has_active_visual_animations,
            now,
        );
    }

    // Returns false, leaving everything as it was, for a tree that does not fit the installed display list.
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
        self.invalidate_visual_context_tree_for_compositing();
        self.visual_context_tree = Some(visual_context_tree);
        update_visual_animation_sampling_state(
            self.visual_context_tree.as_ref().unwrap(),
            &mut self.visual_animation_sample_time_ns,
            &mut self.has_active_visual_animations,
            now,
        );
        true
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

    // How many screen pixels a zoomed visual viewport shows one pixel of the layout viewport with. Scrollers scroll and
    // stretch in the pixels of the layout viewport.
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
