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
};
use libcompositing_rust::time::MonotonicTime;
use libcompositing_rust::visual_context::VisualContextTree;
use libgfx_rust::{
    AffineTransform, FloatMatrix4x4, FloatPoint, FloatRect, IntRect, IntSize, enclosing_int_rect, scale_matrix,
};
use std::io::Write;
use std::rc::Rc;
use std::sync::Arc;

/// A plan of the layers of the display list a scene holds under the tree WebContent published with its visual viewport
/// at identity, as WebContent sent it, once it passed the checks against the two. It holds wherever the visual viewport
/// is.
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

/// A plan of layers the scene holds, with the list and the tree of layer plans it was taken under.
struct HeldLayerPlan {
    plan: ReceivedLayerPlan,
    display_list: Rc<DisplayList>,
    tree_of_layer_plans: Arc<VisualContextTree>,
}

#[derive(Default)]
pub struct ContextScene {
    display_list: Option<Rc<DisplayList>>,
    visual_context_tree: Option<AccumulatedVisualContextTree>,
    layer_plan: Option<HeldLayerPlan>,
    /// Whether the scene holds the display list and the tree of the update WebContent sent last, which the plans it
    /// sends with scroll states are of.
    holds_list_and_tree_sent_last: bool,
    async_visual_viewport_transform: Option<TransformWithOrigin>,
    visual_context_tree_for_compositing: Option<AccumulatedVisualContextTree>,
    sampled_visual_context_tree: Option<AccumulatedVisualContextTree>,
    visual_animation_sample_time_ns: Option<i64>,
    has_active_visual_animations: bool,
    /// The published tree with its visual viewport at identity, which plans of layers are made under and checked
    /// against. It stays the same copy while the trees WebContent publishes differ from it only in the visual viewport.
    tree_of_layer_plans: Option<Arc<VisualContextTree>>,
    /// The plan the scene held before WebContent sent the list or the tree it holds. The scene keeps it where WebContent
    /// sends that plan again under the same list and tree of layer plans, as with each tree that only moves the visual
    /// viewport, so that frames keep what they worked out of it. Frames bind what they work out of a plan to its list.
    layer_plan_sent_before: Option<HeldLayerPlan>,
    /// The tree of layer plans with its visual viewport at a raster scale other than 1, by that scale and the number of
    /// the copy of the tree of layer plans.
    tree_at_raster_scale: Option<(i32, u64, AccumulatedVisualContextTree)>,
    /// The tree frames raster with, with the visual animations sampled, by the number of its copy and the time of the
    /// sample.
    sampled_raster_tree: Option<(u64, i64, AccumulatedVisualContextTree)>,
    /// The raster scale frames hold while the compositor zooms the page, or none at rest.
    raster_scale_while_zooming: Option<i32>,
}

#[derive(PartialEq)]
enum KeepsPlanSentAgain {
    Yes,
    No,
}

/// What a frame rasters the page with, how it shows that raster, and the plan of the layers it rasters.
pub struct RasterOfFrame {
    pub visual_context_tree: AccumulatedVisualContextTree,
    pub raster_to_frame: Option<AffineTransform>,
    pub layer_plan: Option<ReceivedLayerPlan>,
    /// How many raster pixels the frame rasters each device pixel of the layout viewport at: a power of two, at which
    /// the rects of a plan stay on whole pixels.
    pub raster_scale: i32,
}

// A scale this close to another shows the raster at that scale.
const SCALE_ROUNDING_OF_RASTER_TO_FRAME: f32 = 1e-4;
const LARGEST_RASTER_SCALE: i32 = 8;

/// The raster scale of frames that show the page zoomed by `zoom` at rest: the smallest power of two the zoom does not
/// pass, so that frames scale their raster down, by at most half.
fn raster_scale_at_rest_for(zoom: f32) -> i32 {
    let mut raster_scale = 1;
    while raster_scale < LARGEST_RASTER_SCALE
        && (raster_scale as f32) < zoom * (1.0 - SCALE_ROUNDING_OF_RASTER_TO_FRAME)
    {
        raster_scale *= 2;
    }
    raster_scale
}

/// The largest power of two, from 1, that `zoom` reaches.
fn largest_raster_scale_reached_by(zoom: f32) -> i32 {
    let mut raster_scale = 1;
    while raster_scale < LARGEST_RASTER_SCALE
        && (raster_scale * 2) as f32 <= zoom * (1.0 + SCALE_ROUNDING_OF_RASTER_TO_FRAME)
    {
        raster_scale *= 2;
    }
    raster_scale
}

/// The raster scale of frames that show the page zoomed by `zoom` while the compositor zooms it, which keeps `held` for
/// as long as frames scale it by between half and twice: past twice, the largest power of two the zoom reaches, and
/// below half, the scale at rest.
fn raster_scale_while_zooming_to(held: i32, zoom: f32) -> i32 {
    let frame_scales_held_by = zoom / held as f32;
    if frame_scales_held_by > 2.0 {
        largest_raster_scale_reached_by(zoom)
    } else if frame_scales_held_by < 0.5 {
        raster_scale_at_rest_for(zoom)
    } else {
        held
    }
}

/// How a frame within `viewport_rect` maps a raster of the tree of layer plans at `raster_scale`, which covers the
/// viewport at that scale, to its pixels through the visual viewport `transform`: none where it shows the raster as it
/// is, a translation alone rounded to whole pixels, or a scale and a translation. Fails for a visual viewport that
/// rotates, skews or shrinks the page, or that shows past the raster.
fn raster_to_frame_through_visual_viewport(
    transform: &TransformWithOrigin,
    raster_scale: i32,
    viewport_rect: IntRect,
) -> Option<Option<AffineTransform>> {
    let matrix = transform.matrix_including_origin();
    let visual_viewport = matrix.is_2d_affine().then(|| matrix.extract_2d_affine())?;
    let zooms_in_or_not = |scale: f32| scale >= 1.0 - SCALE_ROUNDING_OF_RASTER_TO_FRAME;
    if visual_viewport.b() != 0.0
        || visual_viewport.c() != 0.0
        || !zooms_in_or_not(visual_viewport.a())
        || !zooms_in_or_not(visual_viewport.d())
        || !visual_viewport.values.iter().all(|value| value.is_finite())
    {
        return None;
    }
    let raster_to_visual_viewport = 1.0 / raster_scale as f32;
    let raster_to_frame = visual_viewport.scaled(raster_to_visual_viewport, raster_to_visual_viewport);
    let keeps_size = |scale: f32| (scale - 1.0).abs() <= SCALE_ROUNDING_OF_RASTER_TO_FRAME;
    let raster_to_frame = if keeps_size(raster_to_frame.a()) && keeps_size(raster_to_frame.d()) {
        let (x, y) = (raster_to_frame.e().round(), raster_to_frame.f().round());
        if x == 0.0 && y == 0.0 {
            return Some(None);
        }
        AffineTransform::new(1.0, 0.0, 0.0, 1.0, x, y)
    } else {
        raster_to_frame
    };
    let viewport = viewport_rect.to_float();
    let raster_scale = raster_scale as f32;
    let raster = FloatRect::new(
        viewport.x * raster_scale,
        viewport.y * raster_scale,
        viewport.width * raster_scale,
        viewport.height * raster_scale,
    );
    let shown = raster_to_frame.inverse()?.map_rect(viewport);
    // A rounding of the transform may move an edge of what the frame shows by this much.
    let rounding = 0.01;
    raster
        .inflated(2.0 * rounding, 2.0 * rounding)
        .contains_rect(shown)
        .then_some(Some(raster_to_frame))
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
        self.set_aside_layer_plan();
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
        self.tree_of_layer_plans = Some(VisualContextTree::tree_of_layer_plans_for(
            visual_context_tree.as_arc(),
            self.tree_of_layer_plans.as_ref(),
        ));
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
        self.set_aside_layer_plan();
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
        self.set_aside_layer_plan();
        self.holds_list_and_tree_sent_last = false;
    }

    fn set_aside_layer_plan(&mut self) {
        if let Some(layer_plan) = self.layer_plan.take() {
            self.layer_plan_sent_before = Some(layer_plan);
        }
    }

    /// Takes the plan of layers that came with the display list or the tree the scene just installed. Fails for a plan
    /// that does not pass the checks against them.
    pub fn take_layer_plan_of_installed_update(
        &mut self,
        layer_plan: Option<UnvalidatedLayerPlan>,
    ) -> Result<(), &'static str> {
        self.holds_list_and_tree_sent_last = true;
        self.take_layer_plan(layer_plan, KeepsPlanSentAgain::Yes)
    }

    /// Takes the plan of layers that came with a scroll state, if any. Fails for a plan that does not pass the checks
    /// against the display list and the tree it is of.
    pub fn take_layer_plan_of_scroll_state(
        &mut self,
        layer_plan: Option<UnvalidatedLayerPlan>,
    ) -> Result<(), &'static str> {
        match layer_plan {
            // A plan that comes with a scroll state is taken anew, as frames that drew the one before may have drawn it
            // as one layer for the scroll offsets they showed.
            Some(layer_plan) if self.holds_list_and_tree_sent_last => {
                self.take_layer_plan(Some(layer_plan), KeepsPlanSentAgain::No)
            }
            _ => Ok(()),
        }
    }

    fn take_layer_plan(
        &mut self,
        layer_plan: Option<UnvalidatedLayerPlan>,
        keeps_plan_sent_again: KeepsPlanSentAgain,
    ) -> Result<(), &'static str> {
        self.set_aside_layer_plan();
        let layer_plan_sent_before = self.layer_plan_sent_before.take();
        let (Some(layer_plan), Some(display_list), Some(tree)) =
            (layer_plan, &self.display_list, &self.tree_of_layer_plans)
        else {
            return Ok(());
        };
        // A plan of another list is of one the compositor refused for a reason WebContent does not learn.
        if layer_plan.display_list_id != display_list.id() {
            return Ok(());
        }
        let layer_plan = layer_plan.validated(display_list.storage(), tree)?;
        if keeps_plan_sent_again == KeepsPlanSentAgain::Yes
            && let Some(layer_plan_sent_before) = layer_plan_sent_before
            && Rc::ptr_eq(&layer_plan_sent_before.display_list, display_list)
            && Arc::ptr_eq(&layer_plan_sent_before.tree_of_layer_plans, tree)
            && layer_plan_sent_before.plan.viewport_rect == layer_plan.viewport_rect
            && *layer_plan_sent_before.plan.stack == layer_plan.stack
        {
            self.layer_plan = Some(layer_plan_sent_before);
            return Ok(());
        }
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
            spatial_nodes_whose_scroll_offset_placement_leaves_out(tree, &anchors_of_spaces)
                .into_iter()
                .enumerate()
                .filter(|&(_, leaves_out)| !leaves_out)
                .map(|(node_index, _)| node_index as u32)
                .collect()
        };
        self.layer_plan = Some(HeldLayerPlan {
            plan: ReceivedLayerPlan {
                viewport_rect: layer_plan.viewport_rect,
                stack: Rc::new(layer_plan.stack),
                nodes_whose_scroll_offset_plan_reads,
                note_lines: layer_plan.note_lines.into(),
            },
            display_list: display_list.clone(),
            tree_of_layer_plans: tree.clone(),
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

    /// What a frame within `viewport_rect` rasters the page with, how it shows that raster, and the plan of the layers
    /// it rasters. As plans hold wherever the visual viewport is, a frame that shows the page zoomed or panned rasters
    /// the tree of layer plans at a power of two of the layout viewport's device pixels and maps the raster to its
    /// pixels through the visual viewport, with the plan. A visual viewport that rotates, skews or shrinks the page is
    /// drawn as the tree places it, without the plan.
    pub fn raster_of_frame(&mut self, viewport_rect: IntRect) -> RasterOfFrame {
        let raster_scale = self.raster_scale_of_frames();
        match raster_to_frame_through_visual_viewport(&self.visual_viewport_transform(), raster_scale, viewport_rect) {
            Some(raster_to_frame) => RasterOfFrame {
                visual_context_tree: self.sampled_tree_at_raster_scale(raster_scale),
                raster_to_frame,
                layer_plan: self.layer_plan.as_ref().map(|held| held.plan.clone()),
                raster_scale,
            },
            None => RasterOfFrame {
                visual_context_tree: self.visual_context_tree_for_compositing().clone(),
                raster_to_frame: None,
                layer_plan: None,
                raster_scale: 1,
            },
        }
    }

    /// The raster scale of frames: the one they hold while the compositor zooms the page, and the one for the zoom at
    /// rest.
    pub fn raster_scale_of_frames(&self) -> i32 {
        self.raster_scale_while_zooming
            .unwrap_or_else(|| raster_scale_at_rest_for(self.visual_viewport_magnification()))
    }

    /// Zooms the visual viewport to `transform` for a step of a pinch, which frames show from a raster they hold the
    /// scale of until the zoom leaves the range they scale it by.
    pub fn zoom_visual_viewport(&mut self, transform: TransformWithOrigin) {
        let held = self.raster_scale_of_frames();
        self.set_async_visual_viewport_transform(transform);
        self.raster_scale_while_zooming = Some(raster_scale_while_zooming_to(
            held,
            self.visual_viewport_magnification(),
        ));
    }

    /// Ends the zoom of the compositor, after which frames raster at the scale for the zoom at rest. Answers whether
    /// that is another scale than the one they held.
    pub fn settle_raster_scale(&mut self) -> bool {
        let Some(held) = self.raster_scale_while_zooming.take() else {
            return false;
        };
        held != self.raster_scale_of_frames()
    }

    fn sampled_tree_at_raster_scale(&mut self, raster_scale: i32) -> AccumulatedVisualContextTree {
        let tree_of_layer_plans = AccumulatedVisualContextTree::from_arc(
            self.tree_of_layer_plans
                .clone()
                .expect("a frame draws a scene that holds a tree"),
        );
        let tree = if raster_scale == 1 {
            self.tree_at_raster_scale = None;
            tree_of_layer_plans
        } else {
            let copy_of_tree_of_layer_plans = tree_of_layer_plans.as_arc().copy.number();
            match &self.tree_at_raster_scale {
                Some((scale, copy, tree)) if *scale == raster_scale && *copy == copy_of_tree_of_layer_plans => {
                    tree.clone()
                }
                _ => {
                    let tree = tree_of_layer_plans.with_visual_viewport_transform(&TransformWithOrigin {
                        matrix: scale_matrix(raster_scale as f32, raster_scale as f32, 1.0),
                        origin: FloatPoint::default(),
                    });
                    self.tree_at_raster_scale = Some((raster_scale, copy_of_tree_of_layer_plans, tree.clone()));
                    tree
                }
            }
        };
        let Some(sample_time_ns) = self.visual_animation_sample_time_ns else {
            return tree;
        };
        let copy = tree.as_arc().copy.number();
        if let Some((sampled_copy, sampled_time_ns, sampled)) = &self.sampled_raster_tree
            && *sampled_copy == copy
            && *sampled_time_ns == sample_time_ns
        {
            return sampled.clone();
        }
        let sampled = tree.with_visual_animation_samples(sample_time_ns);
        self.sampled_raster_tree = Some((copy, sample_time_ns, sampled.clone()));
        sampled
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
        self.raster_scale_while_zooming = None;
    }

    pub fn advance_visual_animations(&mut self, now: MonotonicTime) -> bool {
        if !self.visual_context_tree.as_ref().unwrap().has_visual_animations() {
            return false;
        }
        self.sampled_visual_context_tree = None;
        self.visual_animation_sample_time_ns = Some(now.nanoseconds());
        self.has_active_visual_animations = self
            .visual_context_tree
            .as_ref()
            .unwrap()
            .has_active_visual_animation_at(now.nanoseconds());
        self.has_active_visual_animations
    }

    /// Whether the visual animations need a frame in a viewport of `viewport_size` scrolled as `scroll_state_snapshot`
    /// tells: whether an animation still running at the time they were last sampled changes pixels that frames show,
    /// as the plan of layers the scene holds tells. Without a plan made for that viewport, or where frames draw without
    /// the plan, every active animation needs one.
    pub fn visual_animations_need_frame(
        &self,
        scroll_state_snapshot: &ScrollStateSnapshot,
        viewport_size: IntSize,
        now: MonotonicTime,
    ) -> bool {
        let viewport_rect = IntRect::new(0, 0, viewport_size.width, viewport_size.height);
        if !self.has_active_visual_animations || viewport_rect.is_empty() {
            return false;
        }
        let Some(held) = &self.layer_plan else {
            return true;
        };
        if held.plan.viewport_rect != viewport_rect {
            return true;
        }
        let Some(part_of_viewport_frames_show) = self.part_of_layout_viewport_frames_show(viewport_rect) else {
            return true;
        };
        let sample_time_ns = self.visual_animation_sample_time_ns.unwrap_or(now.nanoseconds());
        let animations = held.tree_of_layer_plans.visual_animations();
        let stack = &held.plan.stack;
        stack.running_animations_can_show_within(
            |animation_index| {
                animations
                    .get(animation_index as usize)
                    .is_none_or(|animation| !animation.has_finished_at(sample_time_ns))
            },
            &stack.composite_offsets(scroll_state_snapshot.device_offsets()),
            part_of_viewport_frames_show,
        )
    }

    /// The part of the layout viewport, in its device pixels, that frames within `viewport_rect` show through the
    /// visual viewport, or none for frames drawn without the plan of layers, as through a visual viewport that rotates,
    /// skews or shrinks the page.
    fn part_of_layout_viewport_frames_show(&self, viewport_rect: IntRect) -> Option<IntRect> {
        let raster_scale = self.raster_scale_of_frames();
        let shown_in_raster = match raster_to_frame_through_visual_viewport(
            &self.visual_viewport_transform(),
            raster_scale,
            viewport_rect,
        )? {
            Some(raster_to_frame) => raster_to_frame.inverse()?.map_rect(viewport_rect.to_float()),
            None => viewport_rect.to_float(),
        };
        let raster_to_layout_viewport = 1.0 / raster_scale as f32;
        let shown = FloatRect::new(
            shown_in_raster.x * raster_to_layout_viewport,
            shown_in_raster.y * raster_to_layout_viewport,
            shown_in_raster.width * raster_to_layout_viewport,
            shown_in_raster.height * raster_to_layout_viewport,
        );
        // A pixel more on each side covers the rounding of the transform.
        Some(
            enclosing_int_rect(shown)
                .inflated_edges(1, 1, 1, 1)
                .intersected(viewport_rect),
        )
    }

    fn invalidate_visual_context_tree_for_compositing(&mut self) {
        self.sampled_visual_context_tree = None;
        self.visual_context_tree_for_compositing = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::animation_timing::{AnimationEffectTiming, TimeValue};
    use libcompositing_rust::display_list::builder::DisplayListBuilder;
    use libcompositing_rust::display_list::commands::{ContextRef, EffectNodeIndex, FillRect};
    use libcompositing_rust::display_list::layers::planning::InputsOfLayerPlan;
    use libcompositing_rust::display_list::layers::{LayerPlan, StackOnWire};
    use libcompositing_rust::display_list::storage::DisplayListStorage;
    use libcompositing_rust::easing::Easing;
    use libcompositing_rust::host::FfiVisualAnimationTargetKind;
    use libcompositing_rust::ipc_types::LayerPlanOnWire;
    use libcompositing_rust::node_slot_id::NodeSlotId;
    use libcompositing_rust::visual_animation::{VisualAnimation, VisualAnimationKeyframe, VisualAnimationValue};
    use libcompositing_rust::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use libcompositing_rust::visual_context::{
        ClipNodeIndex, CompositorScrollNodeRecord, CompositorScrollTable, EffectNodeData, EffectsData, ScrollData,
        SpatialData, VISUAL_VIEWPORT_NODE_INDEX, VisualContextTree,
    };
    use libgfx_rust::{Color, CompositingAndBlendingOperator};
    use libipc_rust::wire::{Decoder, Encoder};
    use std::sync::Arc;

    const VIEWPORT: IntRect = IntRect::new(0, 0, 800, 600);
    const DISPLAY_LIST_ID: u64 = 7;

    fn list_of_rects(tree: &Arc<VisualContextTree>, rect_count: usize) -> Arc<DisplayListStorage> {
        let mut builder = DisplayListBuilder::new();
        for _ in 0..rect_count {
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
        }
        DisplayListStorage::recorded(&builder.finish(), tree.clone())
    }

    fn scene_with_one_rect() -> (ContextScene, Arc<DisplayListStorage>, Arc<VisualContextTree>) {
        let tree = Arc::new(VisualContextTree::create_identity());
        let list = list_of_rects(&tree, 1);
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
    fn received(display_list_id: u64, stack: StackOnWire) -> UnvalidatedLayerPlan {
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
        scene.layer_plan.as_ref().map(|held| (*held.plan.stack).clone())
    }

    #[test]
    fn a_plan_is_taken_with_the_list_it_is_of_and_one_that_does_not_fit_it_is_refused() {
        let (mut scene, list, tree) = scene_with_one_rect();
        let planned = InputsOfLayerPlan::new(list, tree.clone(), &[], VIEWPORT).plan();
        assert_eq!(planned.stack.why_list_is_one_layer, None);
        // The plan of a longer list holds a span past the end of the list of the scene.
        let planned_for_longer_list = InputsOfLayerPlan::new(list_of_rects(&tree, 2), tree, &[], VIEWPORT).plan();
        assert_eq!(planned_for_longer_list.stack.why_list_is_one_layer, None);
        let stack_not_fitting_list = || planned_for_longer_list.stack_on_wire.clone();

        scene.forget_layer_plan();
        assert_eq!(
            scene.take_layer_plan_of_installed_update(Some(received(DISPLAY_LIST_ID, planned.stack_on_wire.clone()))),
            Ok(())
        );
        assert_eq!(stack_of_plan(&scene), Some(planned.stack.clone()));
        // A scroll state without a plan keeps the plan.
        assert_eq!(scene.take_layer_plan_of_scroll_state(None), Ok(()));
        assert_eq!(stack_of_plan(&scene), Some(planned.stack.clone()));

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
            scene.take_layer_plan_of_scroll_state(Some(received(DISPLAY_LIST_ID, planned.stack_on_wire))),
            Ok(())
        );
        assert_eq!(stack_of_plan(&scene), Some(planned.stack));
    }

    const VIEWPORT_SIZE: IntSize = IntSize {
        width: 800,
        height: 600,
    };
    const DOCUMENT_SCROLL_NODE: SpatialNodeIndex = SpatialNodeIndex(1);
    const BOX_ON_SCREEN: IntRect = IntRect::new(600, 400, 50, 50);
    const BOX_BELOW_SCREEN: IntRect = IntRect::new(100, 1500, 50, 50);
    const RUNS_FOREVER: f64 = f64::INFINITY;

    fn fill_of(rect: IntRect) -> FillRect {
        FillRect {
            rect,
            color: Color::from_rgb(0, 128, 0),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    // A box of the document that an animation of one-second iterations, from time zero, fades out.
    struct FadingBox {
        rect: IntRect,
        iteration_count: f64,
    }

    // A scene of a document that scrolls by up to 2000 px and holds `boxes`, with the plan of its layers where
    // `takes_plan` says so.
    fn scene_with_fading_boxes(boxes: &[FadingBox], takes_plan: bool) -> ContextScene {
        let mut tree = VisualContextTree::create_identity();
        let document = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        assert_eq!(document, DOCUMENT_SCROLL_NODE);
        assert!(
            tree.set_compositor_scroll_table(CompositorScrollTable {
                nodes: vec![CompositorScrollNodeRecord {
                    scroll_node_index: document,
                    parent_scroll_node_index: VISUAL_VIEWPORT_NODE_INDEX,
                    scrollport_rect: VIEWPORT,
                    max_scroll_offset: FloatPoint { x: 0.0, y: 2000.0 },
                    is_viewport: true,
                    can_be_wheel_scrolled_vertically: true,
                    ..CompositorScrollNodeRecord::default()
                }]
                .into(),
                ..CompositorScrollTable::default()
            })
        );
        let document_context = ContextRef {
            spatial: document,
            ..ContextRef::default()
        };
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_of(IntRect::new(0, 0, 800, 2600)), &[], document_context);
        let keyframe = |offset: f64, opacity: f32| VisualAnimationKeyframe {
            offset,
            easing: Easing::default(),
            value: VisualAnimationValue::Opacity(opacity),
        };
        let mut animations = Vec::new();
        for fading_box in boxes {
            let effect = tree.append_effect(
                EffectNodeData::Effects(EffectsData {
                    isolation: false,
                    opacity: 1.0,
                    blend_mode: CompositingAndBlendingOperator::Normal,
                    filter: None,
                    backdrop_filter: None,
                }),
                EffectNodeIndex::NONE,
                document,
                ClipNodeIndex::NONE,
            );
            builder.append(
                &fill_of(fading_box.rect),
                &[],
                ContextRef {
                    effect,
                    ..document_context
                },
            );
            animations.push(VisualAnimation {
                target_kind: FfiVisualAnimationTargetKind::Opacity,
                node_indices: vec![effect.0],
                timing: AnimationEffectTiming {
                    iteration_duration: TimeValue::milliseconds(1000.0),
                    iteration_count: fading_box.iteration_count,
                    ..VisualAnimation::default().timing
                },
                keyframes: vec![keyframe(0.0, 1.0), keyframe(1.0, 0.0)],
                ..VisualAnimation::default()
            });
        }
        tree.set_visual_animations(animations);
        let tree = Arc::new(tree);
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
        if takes_plan {
            let planned = InputsOfLayerPlan::new(list, tree, &[], VIEWPORT).plan();
            assert_eq!(planned.stack.why_list_is_one_layer, None);
            assert_eq!(
                scene.take_layer_plan_of_installed_update(Some(received(DISPLAY_LIST_ID, planned.stack_on_wire))),
                Ok(())
            );
        }
        scene
    }

    fn needs_frame(scene: &ContextScene, document_scrolled_by: f32, viewport_size: IntSize, now_ns: i64) -> bool {
        let mut device_offsets = vec![FloatPoint::default(); 2];
        device_offsets[DOCUMENT_SCROLL_NODE.0 as usize] = FloatPoint {
            x: 0.0,
            y: -document_scrolled_by,
        };
        scene.visual_animations_need_frame(
            &ScrollStateSnapshot::from_device_offsets(device_offsets),
            viewport_size,
            MonotonicTime::from_nanoseconds(now_ns),
        )
    }

    #[test]
    fn every_active_animation_needs_frames_without_a_plan_made_for_the_viewport() {
        let box_below_screen = [FadingBox {
            rect: BOX_BELOW_SCREEN,
            iteration_count: RUNS_FOREVER,
        }];
        assert!(needs_frame(
            &scene_with_fading_boxes(&box_below_screen, false),
            0.0,
            VIEWPORT_SIZE,
            0
        ));
        let scene = scene_with_fading_boxes(&box_below_screen, true);
        assert!(!needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
        let wider_viewport = IntSize {
            width: 1000,
            height: 600,
        };
        assert!(needs_frame(&scene, 0.0, wider_viewport, 0));
        let empty_viewport = IntSize { width: 0, height: 0 };
        assert!(!needs_frame(&scene, 0.0, empty_viewport, 0));
    }

    #[test]
    fn an_animation_below_the_viewport_needs_frames_once_scrolled_into_view() {
        let scene = scene_with_fading_boxes(
            &[FadingBox {
                rect: BOX_BELOW_SCREEN,
                iteration_count: RUNS_FOREVER,
            }],
            true,
        );
        assert!(!needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
        assert!(needs_frame(&scene, 1200.0, VIEWPORT_SIZE, 0));
    }

    #[test]
    fn a_zoomed_in_visual_viewport_needs_frames_only_for_animations_it_shows() {
        let mut scene = scene_with_fading_boxes(
            &[FadingBox {
                rect: BOX_ON_SCREEN,
                iteration_count: RUNS_FOREVER,
            }],
            true,
        );
        assert!(needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
        // Zoomed in on the top left quarter of the layout viewport, away from the box.
        scene.set_async_visual_viewport_transform(TransformWithOrigin {
            matrix: scale_matrix(2.0, 2.0, 1.0),
            origin: FloatPoint::default(),
        });
        assert!(!needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
        // Panned to the bottom right quarter, which shows the box.
        let mut panned = scale_matrix(2.0, 2.0, 1.0);
        panned.elements[0][3] = -800.0;
        panned.elements[1][3] = -600.0;
        scene.set_async_visual_viewport_transform(TransformWithOrigin {
            matrix: panned,
            origin: FloatPoint::default(),
        });
        assert!(needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
    }

    #[test]
    fn a_zoomed_out_visual_viewport_needs_frames_for_every_active_animation() {
        let mut scene = scene_with_fading_boxes(
            &[FadingBox {
                rect: BOX_BELOW_SCREEN,
                iteration_count: RUNS_FOREVER,
            }],
            true,
        );
        assert!(!needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
        scene.set_async_visual_viewport_transform(TransformWithOrigin {
            matrix: scale_matrix(0.5, 0.5, 1.0),
            origin: FloatPoint::default(),
        });
        assert!(needs_frame(&scene, 0.0, VIEWPORT_SIZE, 0));
    }

    #[test]
    fn a_finite_animation_needs_one_more_frame_after_it_ends() {
        let mut scene = scene_with_fading_boxes(
            &[
                FadingBox {
                    rect: BOX_ON_SCREEN,
                    iteration_count: 1.0,
                },
                FadingBox {
                    rect: BOX_BELOW_SCREEN,
                    iteration_count: RUNS_FOREVER,
                },
            ],
            true,
        );
        let (before_end_ns, past_end_ns) = (900_000_000, 1_100_000_000);
        assert!(scene.advance_visual_animations(MonotonicTime::from_nanoseconds(before_end_ns)));
        // Sampled before it ended, the animation of the box on screen needs the frame that shows its end.
        assert!(needs_frame(&scene, 0.0, VIEWPORT_SIZE, past_end_ns));
        assert!(scene.advance_visual_animations(MonotonicTime::from_nanoseconds(past_end_ns)));
        // The animation below the viewport runs on, and needs no frame.
        assert!(!needs_frame(&scene, 0.0, VIEWPORT_SIZE, past_end_ns));
    }
}
