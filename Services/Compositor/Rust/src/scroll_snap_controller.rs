/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Selects the snap positions of the wheel scrolls the compositor performs on snap containers, from the geometry the display list carries, the way the main thread selects them for the scrolls it performs itself. The context owns the scroll animations; this controller decides where they go and remembers what a gesture has asked for so far.

use libcompositing_rust::css_pixels::{CssPixelPoint, CssPixels};
use libcompositing_rust::scrolling::scroll_snap_selection::{
    MomentumFlingEstimator, SnapAxisCandidates, SnapContainerGeometry, SnapSelectionStrategy,
    SnapSelectionStrategyType, axes_to_evaluate, build_snap_candidates, select_snap_destination_with_candidates,
};
use libcompositing_rust::scrolling::{
    AsyncScrollNodeStableID, AsyncScrollOperationID, AsyncScrollTree, AsyncSnapContainer, ScrollAnimationKind,
    ScrollGesturePhase, ScrollStateSnapshot, SnapDestination, SpatialNodeIndex, StartedUserScroll,
    USER_SCROLL_SETTLE_DELAY,
};
use libcompositing_rust::time::MonotonicTime;
use std::collections::{BTreeMap, HashMap};

#[derive(Default)]
pub struct ScrollSnapController {
    nodes: BTreeMap<AsyncScrollNodeStableID, NodeState>,
    candidates: HashMap<AsyncScrollNodeStableID, SnapAxisCandidates>,
    momentum_snap_position_selection: MomentumSnapPositionSelection,
    momentum_fling_estimator: MomentumFlingEstimator,
}

// A snap scroll to start for a wheel delta.
pub struct SnapScrollStart {
    pub stable_node_id: AsyncScrollNodeStableID,
    pub initial_scroll_offset: CssPixelPoint,
    pub unsnapped_scroll_destination: CssPixelPoint,
    pub relative_scroll_delta: CssPixelPoint,
    pub selection: SnapDestination,
    pub animation_kind: ScrollAnimationKind,
    pub is_absolute_scroll: bool,
}

impl Default for SnapScrollStart {
    fn default() -> Self {
        Self {
            stable_node_id: AsyncScrollNodeStableID {
                node_id: Default::default(),
                kind: libcompositing_rust::scrolling::AsyncScrollNodeKind::Element,
                pseudo_element_type: 0,
            },
            initial_scroll_offset: Default::default(),
            unsnapped_scroll_destination: Default::default(),
            relative_scroll_delta: Default::default(),
            selection: Default::default(),
            animation_kind: ScrollAnimationKind::SmoothScroll,
            is_absolute_scroll: false,
        }
    }
}

// A step that selected the snap position the scrolling box already rests at, or is already scrolling to, is
// consumed without disturbing where the box is going. Keys can still advance the input for that scroll.
#[derive(Default)]
pub struct StepConsumed {
    pub updated_scroll: Option<StartedUserScroll>,
}

// What a scroll step over a snap container does with the snap position it selected; a step that selects none
// scrolls the box by itself.
pub enum StepDecision {
    StepConsumed(StepConsumed),
    SnapScrollStart(SnapScrollStart),
}

#[derive(Default)]
pub struct GestureEndSnap {
    pub scroll_node_index: SpatialNodeIndex,
    pub snap_scroll: SnapScrollStart,
}

struct InFlightSnapScroll {
    operation_id: AsyncScrollOperationID,
    destination: CssPixelPoint,
}

// Where the scrolling box rested when the gesture's input started moving it, from which the end of the gesture
// snaps; momentum that traveled over the box must not pass a snap position that always stops.
#[derive(Clone, Copy)]
struct GestureStart {
    offset: CssPixelPoint,
    travels_under_momentum: bool,
}

#[derive(Default)]
struct NodeState {
    // The offset the gesture's steps have asked for without snapping, which its next step travels from, so that a
    // burst of steps advances by the distance they asked for rather than by one snap position each.
    unsnapped_scroll_destination: Option<CssPixelPoint>,
    gesture_input_deadline: Option<MonotonicTime>,
    // The offset this controller last left the scrolling box at; a box resting anywhere else has been scrolled by
    // something else since, which ends the gesture.
    expected_scroll_offset: Option<CssPixelPoint>,
    // The snap scroll of this controller's own that the box is scrolling under.
    snap_scroll: Option<InFlightSnapScroll>,
    gesture_start: Option<GestureStart>,
}

// Momentum that selects no snap position is scrolled by for the rest of the gesture rather than being asked again
// for each delta it produces.
#[derive(Default, PartialEq, Eq)]
enum MomentumSnapPositionSelection {
    #[default]
    NotSelectedYet,
    ScrollingToSelectedPosition,
    NoPositionSelected,
}

// A scroll node a snap position can be selected for: one with snap geometry and a current offset.
struct SnapTarget<'a> {
    stable_node_id: AsyncScrollNodeStableID,
    snap_container: &'a AsyncSnapContainer,
    current_scroll_offset: CssPixelPoint,
}

impl ScrollSnapController {
    pub fn did_install_scrolling_state(&mut self, scroll_tree: &AsyncScrollTree) {
        self.candidates.clear();
        self.nodes
            .retain(|stable_node_id, _| scroll_tree.scroll_node_index_for_stable_id(*stable_node_id).is_some());
    }

    // Input that scrolls with a gesture reports its phases; the momentum of a flick and the end of a gesture snap
    // from what the gesture has done so far.
    pub fn note_gesture_phase(&mut self, phase: ScrollGesturePhase) {
        match phase {
            ScrollGesturePhase::None => self.end_gesture(),
            ScrollGesturePhase::Ongoing => self.reset_momentum_fling_state(),
            ScrollGesturePhase::Momentum | ScrollGesturePhase::Ended => {}
        }
    }

    fn reset_momentum_fling_state(&mut self) {
        self.momentum_snap_position_selection = MomentumSnapPositionSelection::NotSelectedYet;
        self.momentum_fling_estimator.reset();
    }

    fn end_gesture(&mut self) {
        self.reset_momentum_fling_state();
        for state in self.nodes.values_mut() {
            state.gesture_start = None;
        }
    }

    // A gesture whose steps this controller chains awaits its next step until its input deadline passes, and ends
    // once that happens without one.
    pub fn boxes_with_gesture_awaiting_input(&self) -> Vec<AsyncScrollNodeStableID> {
        let mut boxes = Vec::new();
        for (stable_node_id, state) in &self.nodes {
            if state.gesture_input_deadline.is_some() {
                boxes.push(*stable_node_id);
            }
        }
        boxes
    }

    pub fn earliest_gesture_input_deadline(&self) -> Option<MonotonicTime> {
        let mut earliest = None;
        for state in self.nodes.values() {
            let Some(deadline) = state.gesture_input_deadline else {
                continue;
            };
            if earliest.is_none_or(|earliest| deadline < earliest) {
                earliest = Some(deadline);
            }
        }
        earliest
    }

    // Returns the boxes whose gesture ended.
    pub fn end_gestures_whose_input_ran_out(&mut self, now: MonotonicTime) -> Vec<AsyncScrollNodeStableID> {
        let mut ended_boxes = Vec::new();
        for (stable_node_id, state) in &mut self.nodes {
            if state.gesture_input_deadline.is_none_or(|deadline| now <= deadline) {
                continue;
            }
            state.gesture_input_deadline = None;
            state.unsnapped_scroll_destination = None;
            ended_boxes.push(*stable_node_id);
        }
        ended_boxes
    }

    pub fn did_scroll_node_plainly(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        phase: ScrollGesturePhase,
        scroll_offset_before_scroll: CssPixelPoint,
    ) {
        if phase != ScrollGesturePhase::Ongoing && phase != ScrollGesturePhase::Momentum {
            return;
        }
        let state = self.node_state(stable_node_id);
        let gesture_start = state.gesture_start.get_or_insert(GestureStart {
            offset: scroll_offset_before_scroll,
            travels_under_momentum: false,
        });
        if phase == ScrollGesturePhase::Momentum {
            gesture_start.travels_under_momentum = true;
        }
    }

    pub fn did_start_main_thread_scroll(&mut self, stable_node_id: AsyncScrollNodeStableID) {
        // A scroll started for any reason other than this controller's input is going somewhere the gesture never asked
        // for, so the gesture's steps then travel from the scrolling box itself.
        self.nodes.remove(&stable_node_id);
    }

    pub fn did_start_snap_scroll(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        operation_id: AsyncScrollOperationID,
        destination: CssPixelPoint,
    ) {
        let state = self.node_state(stable_node_id);
        state.snap_scroll = Some(InFlightSnapScroll {
            operation_id,
            destination,
        });
        state.expected_scroll_offset = Some(destination);
    }

    pub fn did_end_snap_scroll(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        operation_id: AsyncScrollOperationID,
        scroll_offset: Option<CssPixelPoint>,
    ) {
        if !self.is_snap_scroll(stable_node_id, operation_id) {
            return;
        }
        let state = self.node_state(stable_node_id);
        state.snap_scroll = None;
        state.expected_scroll_offset = scroll_offset;
    }

    pub fn did_retarget_scroll(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        operation_id: AsyncScrollOperationID,
    ) {
        // A snap scroll sent somewhere other than the snap position selected for it no longer goes where the gesture's
        // input asked, so the gesture's steps then travel from where the scroll is headed instead.
        if self.is_snap_scroll(stable_node_id, operation_id) {
            self.nodes.remove(&stable_node_id);
        }
    }

    pub fn is_snap_scroll(
        &self,
        stable_node_id: AsyncScrollNodeStableID,
        operation_id: AsyncScrollOperationID,
    ) -> bool {
        self.nodes.get(&stable_node_id).is_some_and(|state| {
            state
                .snap_scroll
                .as_ref()
                .is_some_and(|scroll| scroll.operation_id == operation_id)
        })
    }

    pub fn unsnapped_destination_for_snap_scroll(
        &self,
        stable_node_id: AsyncScrollNodeStableID,
        operation_id: AsyncScrollOperationID,
    ) -> Option<CssPixelPoint> {
        if !self.is_snap_scroll(stable_node_id, operation_id) {
            return None;
        }
        self.nodes.get(&stable_node_id)?.unsnapped_scroll_destination
    }

    fn node_state(&mut self, stable_node_id: AsyncScrollNodeStableID) -> &mut NodeState {
        self.nodes.entry(stable_node_id).or_default()
    }

    fn candidates_for(
        &mut self,
        stable_node_id: AsyncScrollNodeStableID,
        snap_container: &AsyncSnapContainer,
    ) -> &SnapAxisCandidates {
        self.candidates.entry(stable_node_id).or_insert_with(|| {
            build_snap_candidates(
                &snap_container.geometry,
                &snap_container.areas,
                snap_container.geometry.axes,
            )
        })
    }

    fn snap_target_for<'a>(
        scroll_tree: &'a AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        scroll_node_index: SpatialNodeIndex,
    ) -> Option<SnapTarget<'a>> {
        let node = scroll_tree.scroll_node_for_index(scroll_node_index);
        let snap_container = scroll_tree.snap_container_for_node(scroll_node_index);
        let current_scroll_offset = scroll_tree.css_scroll_offset_for_node(scroll_node_index, scroll_state_snapshot);
        let snap_container = snap_container?;
        if snap_container.geometry.min_scroll_offset.x > snap_container.geometry.max_scroll_offset.x
            || snap_container.geometry.min_scroll_offset.y > snap_container.geometry.max_scroll_offset.y
        {
            return None;
        }
        Some(SnapTarget {
            stable_node_id: node?.stable_node_id,
            snap_container,
            current_scroll_offset: current_scroll_offset?,
        })
    }
}

fn clamp_scroll_offset(offset: CssPixelPoint, geometry: &SnapContainerGeometry) -> CssPixelPoint {
    CssPixelPoint {
        x: offset
            .x
            .clamp(geometry.min_scroll_offset.x, geometry.max_scroll_offset.x),
        y: offset
            .y
            .clamp(geometry.min_scroll_offset.y, geometry.max_scroll_offset.y),
    }
}

// A scroll travels only along axes the container selects no snap position in is left to the ordinary relative scroll.
fn selected_snap_position_along_travel(selection: &SnapDestination, displacement: CssPixelPoint) -> bool {
    (selection.snapped_x && displacement.x != CssPixels::default())
        || (selection.snapped_y && displacement.y != CssPixels::default())
}

impl ScrollSnapController {
    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
    // A discrete wheel step is a relative scroll with only an intended direction.
    #[allow(clippy::too_many_arguments)]
    pub fn decide_discrete_step(
        &mut self,
        scroll_tree: &AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        scroll_node_index: SpatialNodeIndex,
        delta: CssPixelPoint,
        scroll_in_flight_destination: Option<CssPixelPoint>,
        now: MonotonicTime,
    ) -> Option<StepDecision> {
        let target = Self::snap_target_for(scroll_tree, scroll_state_snapshot, scroll_node_index)?;
        let geometry = &target.snap_container.geometry;
        // The step selects a snap position only along the axes it travels in that the container snaps in.
        if !(geometry.axes.x && delta.x != CssPixels::default())
            && !(geometry.axes.y && delta.y != CssPixels::default())
        {
            return None;
        }

        let current_scroll_offset = target.current_scroll_offset;
        let state = self.node_state(target.stable_node_id);
        // A gesture ends once its steps stop arriving, and once anything but this controller has moved the scrolling box.
        let gesture_ended = state.gesture_input_deadline.is_some_and(|deadline| now > deadline)
            || (state.snap_scroll.is_none() && state.expected_scroll_offset != Some(current_scroll_offset));
        if gesture_ended {
            state.unsnapped_scroll_destination = None;
        }
        // A step selects its snap position from the offset the gesture's input deltas have reached, which a wheel scroll in
        // flight is headed for, rather than from the snap position it is scrolling to.
        let step_start = state
            .unsnapped_scroll_destination
            .unwrap_or(scroll_in_flight_destination.unwrap_or(current_scroll_offset));
        let unsnapped_destination = clamp_scroll_offset(step_start.translated(delta.x, delta.y), geometry);

        // NB: A step with only an intended direction ignores every snap position up to the offset its input asked for.
        let strategy = SnapSelectionStrategy {
            r#type: SnapSelectionStrategyType::Direction,
            start_offset: Some(step_start),
            displacement: delta,
            starting_positions_boundary: Some(unsnapped_destination),
        };
        let evaluated_axes = axes_to_evaluate(geometry.axes, &strategy);
        let selection = select_snap_destination_with_candidates(
            geometry,
            self.candidates_for(target.stable_node_id, target.snap_container),
            unsnapped_destination,
            &strategy,
            evaluated_axes,
        );
        if !selected_snap_position_along_travel(&selection, delta) {
            return None;
        }

        let state = self.nodes.get_mut(&target.stable_node_id).unwrap();
        state.unsnapped_scroll_destination = Some(unsnapped_destination);
        state.gesture_input_deadline = Some(now + USER_SCROLL_SETTLE_DELAY);
        let mut in_flight_destination = state.snap_scroll.as_ref().map(|scroll| scroll.destination);
        if in_flight_destination.is_none() {
            in_flight_destination = scroll_in_flight_destination;
        }
        if selection.position == in_flight_destination.unwrap_or(current_scroll_offset) {
            if state.snap_scroll.is_none() {
                state.expected_scroll_offset = Some(current_scroll_offset);
            }
            return Some(StepDecision::StepConsumed(StepConsumed::default()));
        }
        Some(StepDecision::SnapScrollStart(SnapScrollStart {
            stable_node_id: target.stable_node_id,
            initial_scroll_offset: current_scroll_offset,
            unsnapped_scroll_destination: unsnapped_destination,
            relative_scroll_delta: CssPixelPoint {
                x: unsnapped_destination.x - step_start.x,
                y: unsnapped_destination.y - step_start.y,
            },
            selection,
            animation_kind: ScrollAnimationKind::SmoothScroll,
            is_absolute_scroll: false,
        }))
    }

    // A scroll key is a command of its own rather than a step of a gesture: while a scroll is in flight it travels on
    // from the offset the steps before it asked for, otherwise from the scrolling box itself. An arrow key has only
    // an intended direction; a paging key has an intended end position as well.
    #[allow(clippy::too_many_arguments)]
    pub fn decide_key_step(
        &mut self,
        scroll_tree: &AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        scroll_node_index: SpatialNodeIndex,
        delta: CssPixelPoint,
        strategy_type: SnapSelectionStrategyType,
        scroll_in_flight_destination: Option<CssPixelPoint>,
        now: MonotonicTime,
    ) -> Option<StepDecision> {
        let target = Self::snap_target_for(scroll_tree, scroll_state_snapshot, scroll_node_index)?;
        let geometry = &target.snap_container.geometry;
        if !(geometry.axes.x && delta.x != CssPixels::default())
            && !(geometry.axes.y && delta.y != CssPixels::default())
        {
            return None;
        }

        let current_scroll_offset = target.current_scroll_offset;
        let state = self.node_state(target.stable_node_id);
        let mut in_flight_destination = state.snap_scroll.as_ref().map(|scroll| scroll.destination);
        if in_flight_destination.is_none() {
            in_flight_destination = scroll_in_flight_destination;
        }
        let step_start = if let Some(snap_scroll) = state.snap_scroll.as_ref() {
            state.unsnapped_scroll_destination.unwrap_or(snap_scroll.destination)
        } else {
            scroll_in_flight_destination.unwrap_or(current_scroll_offset)
        };
        let unsnapped_destination = clamp_scroll_offset(step_start.translated(delta.x, delta.y), geometry);

        let mut strategy = SnapSelectionStrategy {
            r#type: strategy_type,
            start_offset: Some(step_start),
            displacement: delta,
            starting_positions_boundary: None,
        };
        if strategy_type == SnapSelectionStrategyType::Direction {
            strategy.starting_positions_boundary = Some(unsnapped_destination);
        }
        let evaluated_axes = axes_to_evaluate(geometry.axes, &strategy);
        let selection = select_snap_destination_with_candidates(
            geometry,
            self.candidates_for(target.stable_node_id, target.snap_container),
            unsnapped_destination,
            &strategy,
            evaluated_axes,
        );
        if !selected_snap_position_along_travel(&selection, delta) {
            return None;
        }

        let state = self.nodes.get_mut(&target.stable_node_id).unwrap();
        state.unsnapped_scroll_destination = Some(unsnapped_destination);
        state.gesture_input_deadline = Some(now + USER_SCROLL_SETTLE_DELAY);
        if selection.position == in_flight_destination.unwrap_or(current_scroll_offset) {
            if state.snap_scroll.is_none() {
                state.expected_scroll_offset = Some(current_scroll_offset);
            }
            let updated_scroll = state.snap_scroll.as_ref().map(|snap_scroll| StartedUserScroll {
                stable_node_id: target.stable_node_id,
                operation_id: snap_scroll.operation_id,
                initial_scroll_offset: current_scroll_offset,
                unsnapped_scroll_destination: unsnapped_destination,
                relative_scroll_delta: CssPixelPoint {
                    x: unsnapped_destination.x - step_start.x,
                    y: unsnapped_destination.y - step_start.y,
                },
                selection,
                settles_gesture: false,
                is_absolute_scroll: false,
                animation_kind: None,
            });
            return Some(StepDecision::StepConsumed(StepConsumed { updated_scroll }));
        }
        Some(StepDecision::SnapScrollStart(SnapScrollStart {
            stable_node_id: target.stable_node_id,
            initial_scroll_offset: current_scroll_offset,
            unsnapped_scroll_destination: unsnapped_destination,
            relative_scroll_delta: CssPixelPoint {
                x: unsnapped_destination.x - step_start.x,
                y: unsnapped_destination.y - step_start.y,
            },
            selection,
            animation_kind: ScrollAnimationKind::SmoothScroll,
            is_absolute_scroll: false,
        }))
    }

    // The momentum of a flick is a relative scroll with an intended direction and, once its decay tells where it is
    // headed, an intended end position.
    pub fn decide_momentum_delta(
        &mut self,
        scroll_tree: &AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
        scroll_node_index: SpatialNodeIndex,
        momentum_delta: CssPixelPoint,
    ) -> Option<StepDecision> {
        if self.momentum_snap_position_selection == MomentumSnapPositionSelection::ScrollingToSelectedPosition {
            return Some(StepDecision::StepConsumed(StepConsumed::default()));
        }
        if self.momentum_snap_position_selection == MomentumSnapPositionSelection::NoPositionSelected {
            return None;
        }
        let remaining_displacement = self
            .momentum_fling_estimator
            .estimate_remaining_displacement(momentum_delta)?;
        let Some(target) = Self::snap_target_for(scroll_tree, scroll_state_snapshot, scroll_node_index) else {
            self.momentum_snap_position_selection = MomentumSnapPositionSelection::NoPositionSelected;
            return None;
        };
        let geometry = &target.snap_container.geometry;
        let state = self.node_state(target.stable_node_id);
        // The momentum travels on from the snap position a snap scroll already in flight is headed for.
        let in_flight_destination = state.snap_scroll.as_ref().map(|scroll| scroll.destination);
        let step_start = in_flight_destination.unwrap_or(target.current_scroll_offset);
        let unsnapped_destination = clamp_scroll_offset(
            step_start.translated(remaining_displacement.x, remaining_displacement.y),
            geometry,
        );

        let strategy = SnapSelectionStrategy {
            r#type: SnapSelectionStrategyType::EndPositionAndDirection,
            start_offset: Some(step_start),
            displacement: remaining_displacement,
            starting_positions_boundary: None,
        };
        let evaluated_axes = axes_to_evaluate(geometry.axes, &strategy);
        let selection = select_snap_destination_with_candidates(
            geometry,
            self.candidates_for(target.stable_node_id, target.snap_container),
            unsnapped_destination,
            &strategy,
            evaluated_axes,
        );
        if !selected_snap_position_along_travel(&selection, remaining_displacement) {
            self.momentum_snap_position_selection = MomentumSnapPositionSelection::NoPositionSelected;
            return None;
        }
        self.momentum_snap_position_selection = MomentumSnapPositionSelection::ScrollingToSelectedPosition;
        if selection.position == step_start {
            return Some(StepDecision::StepConsumed(StepConsumed::default()));
        }
        Some(StepDecision::SnapScrollStart(SnapScrollStart {
            stable_node_id: target.stable_node_id,
            initial_scroll_offset: target.current_scroll_offset,
            unsnapped_scroll_destination: unsnapped_destination,
            relative_scroll_delta: CssPixelPoint {
                x: unsnapped_destination.x - step_start.x,
                y: unsnapped_destination.y - step_start.y,
            },
            selection,
            animation_kind: ScrollAnimationKind::Momentum,
            is_absolute_scroll: false,
        }))
    }

    // The end of a gesture snaps each scrolling box it panned to the snap position nearest where it was released,
    // and ends the gesture.
    pub fn decide_gesture_end(
        &mut self,
        scroll_tree: &AsyncScrollTree,
        scroll_state_snapshot: &ScrollStateSnapshot,
    ) -> Vec<GestureEndSnap> {
        let mut snaps = Vec::new();
        for (stable_node_id, state) in &self.nodes {
            let Some(gesture_start) = state.gesture_start else {
                continue;
            };
            // Momentum that selected a snap position is still scrolling to it.
            if state.snap_scroll.is_some() {
                continue;
            }
            let stable_node_id = *stable_node_id;
            let Some(scroll_node_index) = scroll_tree.scroll_node_index_for_stable_id(stable_node_id) else {
                continue;
            };
            let Some(target) = Self::snap_target_for(scroll_tree, scroll_state_snapshot, scroll_node_index) else {
                continue;
            };
            let geometry = &target.snap_container.geometry;
            let current_scroll_offset = target.current_scroll_offset;

            // https://drafts.csswg.org/css-scroll-snap-1/#scroll-types
            // A pan reports where the user's input came to rest, so it settles as an absolute scroll; the momentum a flick
            // traveled under must not have passed a snap position that always stops on the way from where it started.
            let strategy = SnapSelectionStrategy {
                r#type: SnapSelectionStrategyType::EndPosition,
                start_offset: if gesture_start.travels_under_momentum {
                    Some(gesture_start.offset)
                } else {
                    None
                },
                displacement: CssPixelPoint {
                    x: current_scroll_offset.x - gesture_start.offset.x,
                    y: current_scroll_offset.y - gesture_start.offset.y,
                },
                starting_positions_boundary: None,
            };
            let evaluated_axes = axes_to_evaluate(geometry.axes, &strategy);
            if evaluated_axes.is_empty() {
                continue;
            }
            let selection = select_snap_destination_with_candidates(
                geometry,
                self.candidates
                    .entry(stable_node_id)
                    .or_insert_with(|| build_snap_candidates(geometry, &target.snap_container.areas, geometry.axes)),
                current_scroll_offset,
                &strategy,
                evaluated_axes,
            );
            if selection.position == current_scroll_offset {
                continue;
            }
            snaps.push(GestureEndSnap {
                scroll_node_index,
                snap_scroll: SnapScrollStart {
                    stable_node_id,
                    initial_scroll_offset: current_scroll_offset,
                    unsnapped_scroll_destination: current_scroll_offset,
                    relative_scroll_delta: CssPixelPoint::default(),
                    selection,
                    animation_kind: ScrollAnimationKind::SmoothScroll,
                    is_absolute_scroll: false,
                },
            });
        }
        self.end_gesture();
        snaps
    }
}
