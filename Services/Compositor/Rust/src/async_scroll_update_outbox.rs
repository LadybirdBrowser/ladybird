/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libcompositing_rust::scrolling::{
    AppliedScrollRequest, AsyncScrollNodeStableID, AsyncScrollOffset, AsyncScrollOperationID,
    PendingAsyncScrollUpdates, StartedUserScroll,
};

#[derive(Default)]
pub struct AsyncScrollUpdateOutbox {
    pending_scroll_offsets: Vec<AsyncScrollOffset>,
    last_publication_sequence: u64,
    last_allocated_operation_id: AsyncScrollOperationID,
    completed_operation_ids: Vec<AsyncScrollOperationID>,
    operation_ids_taken_over_by_user_input: Vec<AsyncScrollOperationID>,
    started_user_scrolls: Vec<StartedUserScroll>,
    applied_scroll_requests: Vec<AppliedScrollRequest>,
    boxes_whose_user_scroll_gesture_ended: Vec<AsyncScrollNodeStableID>,
    published_user_scroll_gesture_state: UserScrollGestureState,
}

// What the compositor holds on to, published whenever it changes.
#[derive(Default, PartialEq, Eq)]
pub struct UserScrollGestureState {
    pub boxes_in_user_scroll_gesture: Vec<AsyncScrollNodeStableID>,
    pub wheel_latched_scroller: Option<AsyncScrollNodeStableID>,
}

impl AsyncScrollUpdateOutbox {
    pub fn allocate_operation_id(&mut self) -> AsyncScrollOperationID {
        self.last_allocated_operation_id = self.last_allocated_operation_id.wrapping_add(1);
        self.last_allocated_operation_id
    }

    pub fn complete_operation(&mut self, operation_id: AsyncScrollOperationID) {
        self.completed_operation_ids.push(operation_id);
    }

    pub fn note_operation_taken_over_by_user_input(&mut self, operation_id: AsyncScrollOperationID) {
        self.operation_ids_taken_over_by_user_input.push(operation_id);
    }

    pub fn record_started_user_scroll(&mut self, started_user_scroll: StartedUserScroll) {
        self.started_user_scrolls.push(started_user_scroll);
    }

    pub fn note_scroll_request_applied(&mut self, applied_scroll_request: AppliedScrollRequest) {
        self.applied_scroll_requests.push(applied_scroll_request);
    }
}

fn set_or_append_pending_scroll_offset(
    pending_scroll_offsets: &mut Vec<AsyncScrollOffset>,
    scroll_offset: &AsyncScrollOffset,
) {
    for existing in &mut *pending_scroll_offsets {
        if existing.stable_node_id == scroll_offset.stable_node_id {
            existing.merge_later_scroll(scroll_offset);
            return;
        }
    }
    pending_scroll_offsets.push(scroll_offset.clone());
}

impl AsyncScrollUpdateOutbox {
    pub fn record_scroll_offsets(&mut self, scroll_offsets: &[AsyncScrollOffset]) {
        for scroll_offset in scroll_offsets {
            set_or_append_pending_scroll_offset(&mut self.pending_scroll_offsets, scroll_offset);
        }
    }

    pub fn note_user_scroll_gesture_ended(&mut self, stable_node_ids: &[AsyncScrollNodeStableID]) {
        for &stable_node_id in stable_node_ids {
            if !self.boxes_whose_user_scroll_gesture_ended.contains(&stable_node_id) {
                self.boxes_whose_user_scroll_gesture_ended.push(stable_node_id);
            }
        }
    }

    pub fn has_updates(&self, user_scroll_gesture_state: &UserScrollGestureState) -> bool {
        !self.pending_scroll_offsets.is_empty()
            || !self.completed_operation_ids.is_empty()
            || !self.operation_ids_taken_over_by_user_input.is_empty()
            || !self.started_user_scrolls.is_empty()
            || !self.applied_scroll_requests.is_empty()
            || !self.boxes_whose_user_scroll_gesture_ended.is_empty()
            || *user_scroll_gesture_state != self.published_user_scroll_gesture_state
    }

    pub fn take_updates(&mut self, user_scroll_gesture_state: UserScrollGestureState) -> PendingAsyncScrollUpdates {
        self.last_publication_sequence = self.last_publication_sequence.wrapping_add(1);
        let updates = PendingAsyncScrollUpdates {
            document_id: None,
            sequence: self.last_publication_sequence,
            scroll_offsets: std::mem::take(&mut self.pending_scroll_offsets),
            completed_operation_ids: std::mem::take(&mut self.completed_operation_ids),
            operation_ids_taken_over_by_user_input: std::mem::take(&mut self.operation_ids_taken_over_by_user_input),
            started_user_scrolls: std::mem::take(&mut self.started_user_scrolls),
            applied_scroll_requests: std::mem::take(&mut self.applied_scroll_requests),
            boxes_whose_user_scroll_gesture_ended: std::mem::take(&mut self.boxes_whose_user_scroll_gesture_ended),
            boxes_in_user_scroll_gesture: user_scroll_gesture_state.boxes_in_user_scroll_gesture.clone(),
            wheel_latched_scroller: user_scroll_gesture_state.wheel_latched_scroller,
        };
        self.published_user_scroll_gesture_state = user_scroll_gesture_state;
        updates
    }
}
