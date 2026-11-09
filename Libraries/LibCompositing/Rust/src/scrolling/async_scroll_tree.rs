/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Mutable compositor-side copy of AsyncScrollingState. Current scroll offsets live in ScrollStateSnapshot; this tree owns scroll node geometry and derived hit-test targets.

use super::*;
use crate::css_pixels::{CssPixelPoint, CssPixels};
use libgfx_rust::{CornerRadii, FloatPoint, FloatRect, FloatSize, IntPoint, IntRect};

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum WheelHitTestOutcome {
    FoundScrollTarget,
    #[default]
    NoScrollTarget,
    BlockedByMainThreadRegion,
    BlockedByWheelEventRegion,
    HitTestTargetsOfAnotherVisualContextTree,
}

#[derive(Clone, Copy, Debug, Default)]
pub struct WheelHitTestResult {
    pub outcome: WheelHitTestOutcome,
    // Set exactly when the outcome is FoundScrollTarget.
    pub scroll_node_index: Option<SpatialNodeIndex>,
    // The scroller the wheel is over, which is where the search for `scroll_node_index` started. It is set even if
    // neither it nor any scroller around it can scroll by the delta.
    pub innermost_scroll_node_index: Option<SpatialNodeIndex>,
    // The scroller that keeps the delta from the scrollers around it, when the outcome is NoScrollTarget for that
    // reason rather than because no scroller is left to offer it to.
    pub scroll_node_index_keeping_the_scroll: Option<SpatialNodeIndex>,
}

impl WheelHitTestResult {
    pub fn is_blocked_by_a_region(&self) -> bool {
        self.outcome == WheelHitTestOutcome::BlockedByMainThreadRegion
            || self.outcome == WheelHitTestOutcome::BlockedByWheelEventRegion
    }
}

// Where the first step of a wheel gesture over a position goes, as far as the admission and the hit test decide it.
pub struct FirstWheelStepRoute {
    pub decision: InputRoutingDecision,
    pub scroller: Option<AsyncScrollNodeStableID>,
}

pub struct FirstWheelStepRouteForTesting {
    pub route: FirstWheelStepRoute,
    // The document whose scrolling state decided the route.
    pub document_id: Option<UniqueNodeId>,
}

// What the wheel hit test does where a nested context's content is painted.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum NestedContextRegions {
    // The step is left to the page, which hands it to the nested navigable.
    LeaveToMainThread,
    // The step is handed to the nested context under it, which only gets here if there is none.
    HandToNestedContext,
    // The nested context passed the step on, so it goes to whatever is under the nested context.
    Ignore,
}

// Where a wheel or the pointer over the content of a nested context lands in it.
pub struct NestedContextHit {
    pub child_context_id: CompositorContextId,
    // Relative to the origin of the nested context's content box, in the device pixels of the containing context.
    pub position_in_content_box: FloatPoint,
    pub content_box_size: FloatSize,
    // Where the nested context's content is painted among the other things in the containing context that take
    // pointer input.
    pub paint_order_index: u32,
}

pub struct WheelScrollTarget {
    pub stable_node_id: AsyncScrollNodeStableID,
    pub current_offset: FloatPoint,
    pub start_offset: FloatPoint,
    pub destination_offset: FloatPoint,
}

pub struct CachedWheelHitTestTarget {
    pub target_scroll_node_index: Option<SpatialNodeIndex>,
    pub context: ContextRef,
    pub rect: FloatRect,
    pub corner_radii: CornerRadii,
    pub viewport_rect: Option<FloatRect>,
    pub paint_order_index: u32,
}

pub struct CachedMainThreadWheelEventTarget {
    pub context: ContextRef,
    pub rect: FloatRect,
    pub viewport_rect: Option<FloatRect>,
}

pub struct CachedNestedContextWheelRegion {
    pub region: NestedContextWheelRegion,
    pub viewport_rect: Option<FloatRect>,
}

pub struct CachedBlockingWheelEventTarget {
    pub context: ContextRef,
    pub rect: FloatRect,
    pub viewport_rect: Option<FloatRect>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ScrollChaining {
    ToScrollableAncestors,
    None,
}

#[derive(Default)]
pub struct AsyncScrollTree {
    state: AsyncScrollingState,
    cached_wheel_hit_test_targets: Vec<CachedWheelHitTestTarget>,
    cached_main_thread_wheel_event_targets: Vec<CachedMainThreadWheelEventTarget>,
    cached_nested_context_wheel_regions: Vec<CachedNestedContextWheelRegion>,
    cached_blocking_wheel_event_targets: Vec<CachedBlockingWheelEventTarget>,
    visual_context_tree_structural_epoch: Option<u64>,
    scroll_state_snapshot: ScrollStateSnapshot,
}

struct NestedContextWheelRegionHit<'a> {
    region: &'a NestedContextWheelRegion,
    position_in_context: FloatPoint,
}

impl AsyncScrollTree {
    pub fn document_id(&self) -> Option<UniqueNodeId> {
        self.state.document_id
    }
    pub fn scroll_nodes(&self) -> &[AsyncScrollNode] {
        &self.state.scroll_nodes
    }

    // Scroll offsets are held in the device pixels of the display list; snap geometry is in the CSS pixels it was
    // recorded from, at the scale the display list carries.
    pub fn device_pixels_per_css_pixel(&self) -> f64 {
        self.state.device_pixels_per_css_pixel
    }
    pub fn has_nested_contexts(&self) -> bool {
        !self.state.nested_context_wheel_regions.is_empty()
    }
    pub fn has_wheel_hit_test_targets_for(&self, visual_context_tree: &AccumulatedVisualContextTree) -> bool {
        self.visual_context_tree_structural_epoch == Some(visual_context_tree.structural_epoch())
    }

    pub fn set_state(&mut self, state: AsyncScrollingState) {
        self.state = state;
        self.cached_wheel_hit_test_targets.clear();
        self.cached_main_thread_wheel_event_targets.clear();
        self.cached_nested_context_wheel_regions.clear();
        self.cached_blocking_wheel_event_targets.clear();
        self.visual_context_tree_structural_epoch = None;
    }

    pub fn state(&self) -> &AsyncScrollingState {
        &self.state
    }
}

// A first step of a wheel gesture is latched to the scroller the hit test found, and is otherwise left to the page for
// the outcome's reason.
pub fn input_routing_decision_for_first_wheel_step(outcome: WheelHitTestOutcome) -> InputRoutingDecision {
    match outcome {
        WheelHitTestOutcome::FoundScrollTarget => InputRoutingDecision::Latched,
        WheelHitTestOutcome::NoScrollTarget => InputRoutingDecision::NoScrollTarget,
        WheelHitTestOutcome::BlockedByMainThreadRegion => InputRoutingDecision::BlockedByMainThreadRegion,
        WheelHitTestOutcome::BlockedByWheelEventRegion => InputRoutingDecision::BlockedByWheelEventRegion,
        WheelHitTestOutcome::HitTestTargetsOfAnotherVisualContextTree => {
            InputRoutingDecision::HitTestTargetsOfAnotherVisualContextTree
        }
    }
}

impl AsyncScrollTree {
    pub fn scroll_node_for_index(&self, scroll_node_index: SpatialNodeIndex) -> Option<&AsyncScrollNode> {
        self.state
            .scroll_nodes
            .iter()
            .find(|node| node.scroll_node_index == scroll_node_index)
    }
}

fn delta_is_mainly_vertical(delta: FloatPoint) -> bool {
    delta.y.abs() >= delta.x.abs()
}

// https://drafts.csswg.org/css-overscroll-1/#overscroll-behavior-properties
// A step is taken by one scroller, and a touchpad rarely moves along one axis exactly, so the main axis of the step
// decides whether a scroller that cannot take it passes it on.
fn node_chains_scroll_to_ancestors(node: &AsyncScrollNode, delta: FloatPoint) -> bool {
    if delta_is_mainly_vertical(delta) {
        node.chains_scroll_to_ancestors_vertically
    } else {
        node.chains_scroll_to_ancestors_horizontally
    }
}

impl AsyncScrollTree {
    // The scroller, of the given one and those it passes a scroll by the delta on to, that takes a wheel step by it, as
    // the hit test finds one for a step over the given scroller.
    pub fn hit_test_result_for_scroll_node(
        &self,
        scroll_node_index: SpatialNodeIndex,
        delta: FloatPoint,
        in_flight_destination_for: &dyn Fn(AsyncScrollNodeStableID) -> Option<FloatPoint>,
    ) -> WheelHitTestResult {
        let can_take_step = |node: &AsyncScrollNode| {
            let in_flight_destination = in_flight_destination_for(node.stable_node_id);
            Self::can_scroll_node_from_offset_by_delta(
                node,
                in_flight_destination.unwrap_or(Self::scroll_offset_for_scroll_node(node, &self.scroll_state_snapshot)),
                delta,
            )
        };
        let mut node = self.scroll_node_for_index(scroll_node_index);
        while let Some(current) = node {
            if can_take_step(current) {
                return WheelHitTestResult {
                    outcome: WheelHitTestOutcome::FoundScrollTarget,
                    scroll_node_index: Some(current.scroll_node_index),
                    innermost_scroll_node_index: Some(scroll_node_index),
                    ..Default::default()
                };
            }
            // https://drafts.csswg.org/css-overscroll-1/#scroll-chaining-and-boundary-default-actions
            // If a scroll container has no potential to scroll, because it does not overflow in the direction of the
            // scroll, the element is always considered to be at the scroll boundary.
            // NB: A scroll container that keeps its scrolls has a scroll node even when it has nothing to scroll.
            if !node_chains_scroll_to_ancestors(current, delta) {
                return WheelHitTestResult {
                    outcome: WheelHitTestOutcome::NoScrollTarget,
                    innermost_scroll_node_index: Some(scroll_node_index),
                    scroll_node_index_keeping_the_scroll: Some(current.scroll_node_index),
                    ..Default::default()
                };
            }
            let Some(parent) = current.parent_scroll_node_index else {
                break;
            };
            node = self.scroll_node_for_index(parent);
        }
        WheelHitTestResult {
            outcome: WheelHitTestOutcome::NoScrollTarget,
            innermost_scroll_node_index: Some(scroll_node_index),
            ..Default::default()
        }
    }

    // Of the scroller and the scrollers around it that a scroll by the delta may be passed on to, the outermost one a
    // wheel can scroll along the main axis of the delta at all, wherever it is scrolled to.
    pub fn outermost_scroll_node_wheel_scrollable_along_main_axis_of(
        &self,
        innermost_scroll_node_index: SpatialNodeIndex,
        delta: FloatPoint,
    ) -> Option<SpatialNodeIndex> {
        let mut outermost_scroll_node_index = None;
        let mut scroll_node_index = innermost_scroll_node_index;
        for _ in 0..self.state.scroll_nodes.len() {
            let Some(node) = self.scroll_node_for_index(scroll_node_index) else {
                break;
            };
            if if delta_is_mainly_vertical(delta) {
                node.can_be_wheel_scrolled_vertically
            } else {
                node.can_be_wheel_scrolled_horizontally
            } {
                outermost_scroll_node_index = Some(node.scroll_node_index);
            }
            if !node_chains_scroll_to_ancestors(node, delta) {
                break;
            }
            let Some(parent) = node.parent_scroll_node_index else {
                break;
            };
            scroll_node_index = parent;
        }
        outermost_scroll_node_index
    }

    pub fn snap_container_for_node(&self, scroll_node_index: SpatialNodeIndex) -> Option<&AsyncSnapContainer> {
        self.state
            .snap_containers
            .iter()
            .find(|snap_container| snap_container.scroll_node_index == scroll_node_index)
    }

    fn scroll_node_for_stable_id(&self, stable_node_id: AsyncScrollNodeStableID) -> Option<&AsyncScrollNode> {
        self.state
            .scroll_nodes
            .iter()
            .find(|node| node.stable_node_id == stable_node_id)
    }

    fn clamp_scroll_offset_to_node(node: &AsyncScrollNode, mut scroll_offset: FloatPoint) -> FloatPoint {
        scroll_offset.x = max(node.min_scroll_offset.x, min(scroll_offset.x, node.max_scroll_offset.x));
        scroll_offset.y = max(node.min_scroll_offset.y, min(scroll_offset.y, node.max_scroll_offset.y));
        scroll_offset
    }

    pub fn scroll_offset_for_scroll_node(
        node: &AsyncScrollNode,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> FloatPoint {
        let device_offset = scroll_state_snapshot.device_offset_for_index(node.scroll_node_index);
        FloatPoint {
            x: -device_offset.x,
            y: -device_offset.y,
        }
    }

    fn can_scroll_node_by_delta(
        node: &AsyncScrollNode,
        scroll_state_snapshot: &ScrollStateSnapshot,
        delta: FloatPoint,
    ) -> bool {
        Self::can_scroll_node_from_offset_by_delta(
            node,
            Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot),
            delta,
        )
    }

    fn can_scroll_node_from_offset_by_delta(
        node: &AsyncScrollNode,
        scroll_offset: FloatPoint,
        delta: FloatPoint,
    ) -> bool {
        if node.can_be_wheel_scrolled_horizontally && delta.x < 0.0 && scroll_offset.x > node.min_scroll_offset.x {
            return true;
        }
        if node.can_be_wheel_scrolled_horizontally && delta.x > 0.0 && scroll_offset.x < node.max_scroll_offset.x {
            return true;
        }
        if node.can_be_wheel_scrolled_vertically && delta.y < 0.0 && scroll_offset.y > node.min_scroll_offset.y {
            return true;
        }
        if node.can_be_wheel_scrolled_vertically && delta.y > 0.0 && scroll_offset.y < node.max_scroll_offset.y {
            return true;
        }
        false
    }

    fn scrollable_ancestor_for_node(
        &self,
        scroll_node_index: SpatialNodeIndex,
        scroll_state_snapshot: &ScrollStateSnapshot,
        delta: FloatPoint,
    ) -> Option<SpatialNodeIndex> {
        let mut node = self.scroll_node_for_index(scroll_node_index)?;
        while node_chains_scroll_to_ancestors(node, delta) && node.parent_scroll_node_index.is_some() {
            node = self.scroll_node_for_index(node.parent_scroll_node_index?)?;
            if Self::can_scroll_node_by_delta(node, scroll_state_snapshot, delta) {
                return Some(node.scroll_node_index);
            }
        }
        None
    }

    fn apply_scroll_delta_to_node(
        &self,
        node: &AsyncScrollNode,
        delta: FloatPoint,
        scroll_state_snapshot: &mut ScrollStateSnapshot,
    ) -> FloatPoint {
        let old_scroll_offset = Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot);
        let wheel_scrollable_delta = FloatPoint {
            x: if node.can_be_wheel_scrolled_horizontally {
                delta.x
            } else {
                0.0
            },
            y: if node.can_be_wheel_scrolled_vertically {
                delta.y
            } else {
                0.0
            },
        };
        let new_scroll_offset = Self::clamp_scroll_offset_to_node(node, old_scroll_offset + wheel_scrollable_delta);
        if new_scroll_offset == old_scroll_offset {
            return delta;
        }
        scroll_state_snapshot.set_device_offset_for_index(
            node.scroll_node_index,
            FloatPoint {
                x: -new_scroll_offset.x,
                y: -new_scroll_offset.y,
            },
        );
        let consumed_delta = FloatPoint {
            x: new_scroll_offset.x - old_scroll_offset.x,
            y: new_scroll_offset.y - old_scroll_offset.y,
        };
        FloatPoint {
            x: delta.x - consumed_delta.x,
            y: delta.y - consumed_delta.y,
        }
    }

    pub fn apply_scroll_delta(
        &mut self,
        mut scroll_node_index: SpatialNodeIndex,
        delta: FloatPoint,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &mut ScrollStateSnapshot,
        scroll_chaining: ScrollChaining,
    ) -> Option<AsyncScrollOffset> {
        // The compositor can advance only the scroll offsets it owns in this snapshot. Hit testing already selects an
        // ancestor when the target cannot scroll in the wheel direction at all, so once a node moves it consumes the event.
        for _ in 0..self.state.scroll_nodes.len() {
            if delta.is_zero() {
                break;
            }
            let Some(node) = self.scroll_node_for_index(scroll_node_index) else {
                break;
            };
            let remaining_delta = self.apply_scroll_delta_to_node(node, delta, scroll_state_snapshot);
            if remaining_delta != delta {
                resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
                return Some(AsyncScrollOffset {
                    stable_node_id: node.stable_node_id,
                    compositor_scroll_offset: Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot),
                    unadopted_scroll_delta: delta - remaining_delta,
                    last_relative_scroll_delta: FloatPoint::default(),
                    visual_viewport_scale: None,
                    applied_scroll_request_id: 0,
                    user_input_scroll_count: 0,
                    context_user_input_scroll_count: 0,
                });
            }
            if scroll_chaining == ScrollChaining::None {
                break;
            }
            let Some(ancestor_scroll_node_index) =
                self.scrollable_ancestor_for_node(scroll_node_index, scroll_state_snapshot, delta)
            else {
                break;
            };
            scroll_node_index = ancestor_scroll_node_index;
        }
        None
    }

    pub fn select_wheel_scroll_target(
        &self,
        mut scroll_node_index: SpatialNodeIndex,
        delta: FloatPoint,
        scroll_state_snapshot: &ScrollStateSnapshot,
        in_flight_destination_for: &dyn Fn(AsyncScrollNodeStableID) -> Option<FloatPoint>,
        scroll_chaining: ScrollChaining,
    ) -> Option<WheelScrollTarget> {
        // Each box is measured from where its scroll in flight is headed, so a box already headed for its extent passes the
        // first step of a gesture on to an ancestor.
        for _ in 0..self.state.scroll_nodes.len() {
            let node = self.scroll_node_for_index(scroll_node_index)?;
            let start_offset = in_flight_destination_for(node.stable_node_id)
                .unwrap_or(Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot));
            let wheel_scrollable_delta = FloatPoint {
                x: if node.can_be_wheel_scrolled_horizontally {
                    delta.x
                } else {
                    0.0
                },
                y: if node.can_be_wheel_scrolled_vertically {
                    delta.y
                } else {
                    0.0
                },
            };
            let destination_offset = Self::clamp_scroll_offset_to_node(node, start_offset + wheel_scrollable_delta);
            if destination_offset != start_offset {
                return Some(WheelScrollTarget {
                    stable_node_id: node.stable_node_id,
                    current_offset: Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot),
                    start_offset,
                    destination_offset,
                });
            }
            if scroll_chaining == ScrollChaining::None || !node_chains_scroll_to_ancestors(node, delta) {
                return None;
            }
            scroll_node_index = node.parent_scroll_node_index?;
        }
        None
    }

    pub fn rebuild_wheel_hit_test_targets(
        &mut self,
        display_list: Option<&DisplayList>,
        visual_context_tree: Option<&AccumulatedVisualContextTree>,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) {
        self.cached_wheel_hit_test_targets.clear();
        self.cached_main_thread_wheel_event_targets.clear();
        self.cached_nested_context_wheel_regions.clear();
        self.cached_blocking_wheel_event_targets.clear();
        self.visual_context_tree_structural_epoch = None;
        self.scroll_state_snapshot = scroll_state_snapshot.clone();
        let (Some(display_list), Some(visual_context_tree)) = (display_list, visual_context_tree) else {
            return;
        };
        assert_eq!(
            display_list.compatible_visual_context_tree_structural_epoch(),
            visual_context_tree.structural_epoch()
        );
        self.visual_context_tree_structural_epoch = Some(visual_context_tree.structural_epoch());
        let context_is_valid = |context| visual_context_tree.context_is_valid(context);
        let spatial_context_has_visual_animation =
            visual_context_tree.spatial_nodes_in_subtrees_of_transform_animations();
        let viewport_rect_for_context = |context: ContextRef, rect| {
            if *spatial_context_has_visual_animation.get(context.spatial.0 as usize)? {
                return None;
            }
            Some(visual_context_tree.transform_rect_to_viewport(
                context.spatial,
                rect,
                scroll_state_snapshot,
                IncludeVisualViewportTransform::Yes,
            ))
        };
        self.cached_wheel_hit_test_targets
            .reserve(self.state.wheel_hit_test_targets.len());
        for target in &self.state.wheel_hit_test_targets {
            if !context_is_valid(target.context) {
                continue;
            }
            self.cached_wheel_hit_test_targets.push(CachedWheelHitTestTarget {
                target_scroll_node_index: target.target_scroll_node_index,
                context: target.context,
                rect: target.rect,
                corner_radii: target.corner_radii,
                viewport_rect: viewport_rect_for_context(target.context, target.rect),
                paint_order_index: target.paint_order_index,
            });
        }
        self.cached_main_thread_wheel_event_targets
            .reserve(self.state.main_thread_wheel_event_regions.len());
        for region in &self.state.main_thread_wheel_event_regions {
            if !context_is_valid(region.context) {
                continue;
            }
            self.cached_main_thread_wheel_event_targets
                .push(CachedMainThreadWheelEventTarget {
                    context: region.context,
                    rect: region.rect,
                    viewport_rect: viewport_rect_for_context(region.context, region.rect),
                });
        }
        self.cached_nested_context_wheel_regions
            .reserve(self.state.nested_context_wheel_regions.len());
        for region in &self.state.nested_context_wheel_regions {
            if !context_is_valid(region.context) {
                continue;
            }
            self.cached_nested_context_wheel_regions
                .push(CachedNestedContextWheelRegion {
                    region: *region,
                    viewport_rect: viewport_rect_for_context(region.context, region.rect),
                });
        }
        for region in &self.state.blocking_wheel_event_regions {
            // Keep invalid blockers so hit testing fails closed when their extent cannot be determined.
            self.cached_blocking_wheel_event_targets
                .push(CachedBlockingWheelEventTarget {
                    context: region.context,
                    rect: region.rect,
                    viewport_rect: if context_is_valid(region.context) {
                        viewport_rect_for_context(region.context, region.rect)
                    } else {
                        None
                    },
                });
        }
    }
}

fn rounded_rect_contains_point(rect: FloatRect, corner_radii: CornerRadii, position_in_context: FloatPoint) -> bool {
    if !rect.contains_point(position_in_context) {
        return false;
    }
    if !corner_radii.has_any_radius() {
        return true;
    }
    corner_radii.contains_int_point(
        IntPoint {
            x: position_in_context.x as i32,
            y: position_in_context.y as i32,
        },
        IntRect::new(rect.x as i32, rect.y as i32, rect.width as i32, rect.height as i32),
    )
}

fn wheel_hit_test_target_contains_point(target: &CachedWheelHitTestTarget, position_in_context: FloatPoint) -> bool {
    rounded_rect_contains_point(target.rect, target.corner_radii, position_in_context)
}

impl AsyncScrollTree {
    pub fn scroll_offset_for_node(
        &self,
        scroll_node_index: SpatialNodeIndex,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> Option<FloatPoint> {
        if let Some(node) = self.scroll_node_for_index(scroll_node_index) {
            return Some(Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot));
        }
        None
    }

    pub fn css_pixels_from_device_offset(&self, device_offset: FloatPoint) -> CssPixelPoint {
        CssPixelPoint {
            x: CssPixels::nearest_value_for(device_offset.x as f64 / self.state.device_pixels_per_css_pixel),
            y: CssPixels::nearest_value_for(device_offset.y as f64 / self.state.device_pixels_per_css_pixel),
        }
    }

    pub fn device_offset_from_css_pixels(&self, offset: CssPixelPoint) -> FloatPoint {
        FloatPoint {
            x: (offset.x.to_double() * self.state.device_pixels_per_css_pixel) as f32,
            y: (offset.y.to_double() * self.state.device_pixels_per_css_pixel) as f32,
        }
    }

    pub fn css_scroll_offset_for_node(
        &self,
        scroll_node_index: SpatialNodeIndex,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> Option<CssPixelPoint> {
        self.scroll_offset_for_node(scroll_node_index, scroll_state_snapshot)
            .map(|device_offset| self.css_pixels_from_device_offset(device_offset))
    }

    pub fn css_scroll_offset_for_scroll_node(
        &self,
        node: &AsyncScrollNode,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> CssPixelPoint {
        self.css_pixels_from_device_offset(Self::scroll_offset_for_scroll_node(node, scroll_state_snapshot))
    }

    pub fn viewport_scroll_node_index(&self) -> Option<SpatialNodeIndex> {
        for node in &self.state.scroll_nodes {
            if node.is_viewport {
                return Some(node.scroll_node_index);
            }
        }
        None
    }

    pub fn scroll_node_index_for_stable_id(&self, stable_node_id: AsyncScrollNodeStableID) -> Option<SpatialNodeIndex> {
        if let Some(node) = self.scroll_node_for_stable_id(stable_node_id) {
            return Some(node.scroll_node_index);
        }
        None
    }

    pub fn blocks_wheel_event_at_position(
        &self,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> bool {
        if self.state.has_blocking_wheel_event_region_covering_viewport
            || !self.has_wheel_hit_test_targets_for(visual_context_tree)
        {
            return true;
        }
        for target in &self.cached_blocking_wheel_event_targets {
            if !visual_context_tree.context_is_valid(target.context) {
                return true;
            }
            if target.viewport_rect.is_some_and(|rect| !rect.contains_point(position)) {
                continue;
            }
            let position_in_context = visual_context_tree.transform_point_for_hit_test(
                target.context,
                position,
                &self.scroll_state_snapshot,
                ClipBehavior::Respect,
            );
            if position_in_context.is_some_and(|point| target.rect.contains_point(point)) {
                return true;
            }
        }
        false
    }

    pub fn main_thread_wheel_event_region_contains(
        &self,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> bool {
        for target in &self.cached_main_thread_wheel_event_targets {
            if target.viewport_rect.is_some_and(|rect| !rect.contains_point(position)) {
                continue;
            }
            if !visual_context_tree.context_is_valid(target.context) {
                continue;
            }
            let position_in_context = visual_context_tree.transform_point_for_hit_test(
                target.context,
                position,
                &self.scroll_state_snapshot,
                ClipBehavior::Respect,
            );
            if position_in_context.is_some_and(|point| target.rect.contains_point(point)) {
                return true;
            }
        }
        false
    }

    fn topmost_nested_context_wheel_region_at(
        &self,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> Option<NestedContextWheelRegionHit<'_>> {
        for cached_region in self.cached_nested_context_wheel_regions.iter().rev() {
            let region = &cached_region.region;
            if cached_region
                .viewport_rect
                .is_some_and(|rect| !rect.contains_point(position))
            {
                continue;
            }
            if !visual_context_tree.context_is_valid(region.context) {
                continue;
            }
            let position_in_context = visual_context_tree.transform_point_for_hit_test(
                region.context,
                position,
                &self.scroll_state_snapshot,
                ClipBehavior::Respect,
            );
            if let Some(point) = position_in_context
                && rounded_rect_contains_point(region.rect, region.corner_radii, point)
            {
                return Some(NestedContextWheelRegionHit {
                    region,
                    position_in_context: point,
                });
            }
        }
        None
    }
}

fn nested_context_hit_in_region(
    region: &NestedContextWheelRegion,
    position_in_context: FloatPoint,
) -> NestedContextHit {
    NestedContextHit {
        child_context_id: region.child_context_id,
        position_in_content_box: position_in_context
            - FloatPoint {
                x: region.rect.x,
                y: region.rect.y,
            },
        content_box_size: FloatSize {
            width: region.rect.width,
            height: region.rect.height,
        },
        paint_order_index: region.paint_order_index,
    }
}

impl AsyncScrollTree {
    // The nested context whose content is the topmost thing taking pointer input at the position, if any.
    pub fn nested_context_at(
        &self,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> Option<NestedContextHit> {
        if self.visual_context_tree_structural_epoch != Some(visual_context_tree.structural_epoch()) {
            return None;
        }
        let hit = self.topmost_nested_context_wheel_region_at(visual_context_tree, position)?;
        if self.is_covered_by_hit_test_target_painted_after(hit.region.paint_order_index, visual_context_tree, position)
        {
            return None;
        }
        Some(nested_context_hit_in_region(hit.region, hit.position_in_context))
    }

    // As above, unless the main thread takes the wheel events at the position.
    pub fn nested_context_for_wheel_at(
        &self,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> Option<NestedContextHit> {
        if self.visual_context_tree_structural_epoch != Some(visual_context_tree.structural_epoch()) {
            return None;
        }
        if self.main_thread_wheel_event_region_contains(visual_context_tree, position) {
            return None;
        }
        self.nested_context_at(visual_context_tree, position)
    }

    // Where the position lands in the content of the nested context, even outside of that content and its clips, as
    // the pointer of a drag that began in there moves on.
    pub fn position_in_content_of_nested_context(
        &self,
        child_context_id: CompositorContextId,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> Option<NestedContextHit> {
        if self.visual_context_tree_structural_epoch != Some(visual_context_tree.structural_epoch()) {
            return None;
        }
        for cached_region in self.cached_nested_context_wheel_regions.iter().rev() {
            let region = &cached_region.region;
            if region.child_context_id != child_context_id {
                continue;
            }
            if !visual_context_tree.context_is_valid(region.context) {
                return None;
            }
            let position_in_context = visual_context_tree.transform_point_for_hit_test(
                region.context,
                position,
                &self.scroll_state_snapshot,
                ClipBehavior::Ignore,
            )?;
            return Some(nested_context_hit_in_region(region, position_in_context));
        }
        None
    }

    // Where a position in the viewport of the nested context lies in this context's viewport.
    pub fn position_of_nested_context_position(
        &self,
        child_context_id: CompositorContextId,
        position_in_nested_context: FloatPoint,
        nested_context_viewport_size: FloatSize,
        visual_context_tree: &AccumulatedVisualContextTree,
    ) -> Option<FloatPoint> {
        if self.visual_context_tree_structural_epoch != Some(visual_context_tree.structural_epoch())
            || nested_context_viewport_size.is_empty()
        {
            return None;
        }
        for cached_region in self.cached_nested_context_wheel_regions.iter().rev() {
            let region = &cached_region.region;
            if region.child_context_id != child_context_id {
                continue;
            }
            if !visual_context_tree.context_is_valid(region.context) {
                return None;
            }
            // The content box of a nested context is drawn scaled to the size of its viewport.
            let position_in_content_box = FloatPoint {
                x: position_in_nested_context.x * region.rect.width / nested_context_viewport_size.width,
                y: position_in_nested_context.y * region.rect.height / nested_context_viewport_size.height,
            };
            let location = FloatPoint {
                x: region.rect.x,
                y: region.rect.y,
            } + position_in_content_box;
            let point_in_context = FloatRect::new(location.x, location.y, 0.0, 0.0);
            let result = visual_context_tree.transform_rect_to_viewport(
                region.context.spatial,
                point_in_context,
                &self.scroll_state_snapshot,
                IncludeVisualViewportTransform::Yes,
            );
            return Some(FloatPoint {
                x: result.x,
                y: result.y,
            });
        }
        None
    }

    pub fn hit_test_scroll_node_for_wheel(
        &self,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
        delta: FloatPoint,
        in_flight_destination_for: &dyn Fn(AsyncScrollNodeStableID) -> Option<FloatPoint>,
        wheel_step_dispatch: WheelStepDispatch,
        nested_context_regions: NestedContextRegions,
    ) -> WheelHitTestResult {
        if self.visual_context_tree_structural_epoch != Some(visual_context_tree.structural_epoch()) {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::HitTestTargetsOfAnotherVisualContextTree,
                ..Default::default()
            };
        }
        let context_is_valid = |context| visual_context_tree.context_is_valid(context);
        // Neither the listeners nor the main thread have anything left to say about a step the page let through.
        let page_has_yet_to_see_the_step = wheel_step_dispatch == WheelStepDispatch::NotYetDispatched;
        if page_has_yet_to_see_the_step && self.state.has_blocking_wheel_event_region_covering_viewport {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::BlockedByWheelEventRegion,
                ..Default::default()
            };
        }
        if page_has_yet_to_see_the_step && self.main_thread_wheel_event_region_contains(visual_context_tree, position) {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::BlockedByMainThreadRegion,
                ..Default::default()
            };
        }
        if nested_context_regions == NestedContextRegions::LeaveToMainThread
            && self
                .topmost_nested_context_wheel_region_at(visual_context_tree, position)
                .is_some()
        {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::BlockedByMainThreadRegion,
                ..Default::default()
            };
        }
        if page_has_yet_to_see_the_step && self.blocks_wheel_event_at_position(visual_context_tree, position) {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::BlockedByWheelEventRegion,
                ..Default::default()
            };
        }
        for target in self.cached_wheel_hit_test_targets.iter().rev() {
            if target.viewport_rect.is_some_and(|rect| !rect.contains_point(position)) {
                continue;
            }
            if !context_is_valid(target.context) {
                continue;
            }
            let position_in_context = visual_context_tree.transform_point_for_hit_test(
                target.context,
                position,
                &self.scroll_state_snapshot,
                ClipBehavior::Respect,
            );
            if !position_in_context.is_some_and(|point| wheel_hit_test_target_contains_point(target, point)) {
                continue;
            }
            let Some(index) = target.target_scroll_node_index else {
                return WheelHitTestResult {
                    outcome: WheelHitTestOutcome::NoScrollTarget,
                    ..Default::default()
                };
            };
            return self.hit_test_result_for_scroll_node(index, delta, in_flight_destination_for);
        }
        let Some(index_of_viewport_scroll_node) = self.viewport_scroll_node_index() else {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::NoScrollTarget,
                ..Default::default()
            };
        };
        let viewport_node = self.scroll_node_for_index(index_of_viewport_scroll_node);
        if !viewport_node.is_some_and(|node| node.scrollport_rect.to_float().contains_point(position)) {
            return WheelHitTestResult {
                outcome: WheelHitTestOutcome::NoScrollTarget,
                ..Default::default()
            };
        }
        self.hit_test_result_for_scroll_node(index_of_viewport_scroll_node, delta, in_flight_destination_for)
    }

    // Leaves out what only a compositor holding a gesture decides: whether a step continues a latched gesture, which
    // scroller fingers at its edge stretch, and whether a zoomed page pans its visual viewport.
    pub fn route_first_wheel_step(
        &self,
        admission: WheelRoutingAdmission,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
        delta: FloatPoint,
        nested_context_regions: NestedContextRegions,
    ) -> FirstWheelStepRoute {
        if admission != WheelRoutingAdmission::Accepted {
            return FirstWheelStepRoute {
                decision: input_routing_decision_for_wheel_step_not_admitted_by(admission),
                scroller: None,
            };
        }
        let scroll_target = self.hit_test_scroll_node_for_wheel(
            visual_context_tree,
            position,
            delta,
            &|_| None,
            WheelStepDispatch::NotYetDispatched,
            nested_context_regions,
        );
        let mut route = FirstWheelStepRoute {
            decision: input_routing_decision_for_first_wheel_step(scroll_target.outcome),
            scroller: None,
        };
        if let Some(index) = scroll_target.scroll_node_index
            && let Some(node) = self.scroll_node_for_index(index)
        {
            route.scroller = Some(node.stable_node_id);
        }
        route
    }

    // Whether something painted above the given place in paint order takes pointer input at the position. It is taken
    // to be covered whenever that cannot be told.
    pub fn is_covered_by_hit_test_target_painted_after(
        &self,
        paint_order_index: u32,
        visual_context_tree: &AccumulatedVisualContextTree,
        position: FloatPoint,
    ) -> bool {
        if self.visual_context_tree_structural_epoch != Some(visual_context_tree.structural_epoch()) {
            return true;
        }
        for target in self.cached_wheel_hit_test_targets.iter().rev() {
            if target.paint_order_index < paint_order_index {
                break;
            }
            if target.viewport_rect.is_some_and(|rect| !rect.contains_point(position)) {
                continue;
            }
            if !visual_context_tree.context_is_valid(target.context) {
                continue;
            }
            let position_in_context = visual_context_tree.transform_point_for_hit_test(
                target.context,
                position,
                &self.scroll_state_snapshot,
                ClipBehavior::Respect,
            );
            if position_in_context.is_some_and(|point| wheel_hit_test_target_contains_point(target, point)) {
                return true;
            }
        }
        false
    }

    pub fn scroll_node_for_keyboard_scroll(
        &self,
        stable_node_id: AsyncScrollNodeStableID,
        delta: FloatPoint,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> Option<SpatialNodeIndex> {
        let node = self.scroll_node_for_stable_id(stable_node_id)?;
        // Keyboard routing does not depend on pointer or wheel-listener regions.
        if Self::can_scroll_node_by_delta(node, scroll_state_snapshot, delta) {
            return Some(node.scroll_node_index);
        }
        self.scrollable_ancestor_for_node(node.scroll_node_index, scroll_state_snapshot, delta)
    }

    pub fn clamped_scroll_offset_for_node(
        &self,
        scroll_node_index: SpatialNodeIndex,
        scroll_offset: FloatPoint,
    ) -> FloatPoint {
        let Some(node) = self.scroll_node_for_index(scroll_node_index) else {
            return scroll_offset;
        };
        Self::clamp_scroll_offset_to_node(node, scroll_offset)
    }

    pub fn scroll_node_is_viewport(&self, scroll_node_index: SpatialNodeIndex) -> bool {
        self.scroll_node_for_index(scroll_node_index)
            .is_some_and(|node| node.is_viewport)
    }

    // Whether the viewport keeps a scroll by the delta from the document it is nested in, whether or not it has a
    // scroll node.
    pub fn viewport_keeps_scrolls_along_main_axis_of(&self, delta: FloatPoint) -> bool {
        if delta_is_mainly_vertical(delta) {
            self.state.viewport_keeps_vertical_scrolls
        } else {
            self.state.viewport_keeps_horizontal_scrolls
        }
    }

    pub fn set_scroll_offset(
        &mut self,
        scroll_node_index: SpatialNodeIndex,
        scroll_offset: FloatPoint,
        visual_context_tree: &AccumulatedVisualContextTree,
        scroll_state_snapshot: &mut ScrollStateSnapshot,
    ) -> Option<FloatPoint> {
        let node = self.scroll_node_for_index(scroll_node_index)?;
        let new_scroll_offset =
            self.set_scroll_offset_leaving_sticky_offsets_unresolved(node, scroll_offset, scroll_state_snapshot);
        resolve_sticky_offsets(visual_context_tree, scroll_state_snapshot);
        Some(new_scroll_offset)
    }

    pub fn set_scroll_offset_leaving_sticky_offsets_unresolved(
        &self,
        node: &AsyncScrollNode,
        scroll_offset: FloatPoint,
        scroll_state_snapshot: &mut ScrollStateSnapshot,
    ) -> FloatPoint {
        let new_scroll_offset = Self::clamp_scroll_offset_to_node(node, scroll_offset);
        scroll_state_snapshot.set_device_offset_for_index(
            node.scroll_node_index,
            FloatPoint {
                x: -new_scroll_offset.x,
                y: -new_scroll_offset.y,
            },
        );
        new_scroll_offset
    }
}
