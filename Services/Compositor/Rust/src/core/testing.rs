/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Test queries and input tracing for compositor contexts.

use super::*;
use libcompositing_rust::ipc_types::Web_InputTraceRecord as InputTraceRecord;
use libcompositing_rust::scrolling::{
    AsyncScrollNodeStableID, FirstWheelStepRouteForTesting, NestedContextRegions, WheelDeltaPrecision,
};

impl CompositorCore {
    pub fn scroll_state_for_testing(
        &self,
        context_id: CompositorContextId,
        stretched_scroller: AsyncScrollNodeStableID,
    ) -> String {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return String::new();
        };
        context.borrow().scroll_state_for_testing(stretched_scroller)
    }

    pub fn route_first_wheel_step_for_testing(
        &self,
        context_id: CompositorContextId,
        position: FloatPoint,
        delta_in_css_pixels: FloatPoint,
    ) -> Option<FirstWheelStepRouteForTesting> {
        let _entered = self.enter();

        let context = self.context_if_present(context_id)?;
        let may_hand_to_nested_contexts =
            context.borrow().presents_to_client() && context.borrow().may_hand_first_wheel_steps_to_nested_contexts();
        if !may_hand_to_nested_contexts {
            return Some(context.borrow_mut().route_first_wheel_step_for_testing(
                position,
                delta_in_css_pixels,
                NestedContextRegions::LeaveToMainThread,
            ));
        }
        let hit = context.borrow_mut().nested_context_for_wheel_at(position);
        let Some(hit) = hit else {
            return Some(context.borrow_mut().route_first_wheel_step_for_testing(
                position,
                delta_in_css_pixels,
                NestedContextRegions::HandToNestedContext,
            ));
        };
        let taking_context = self
            .first_wheel_step_in_nested_contexts(context_id, hit, delta_in_css_pixels, WheelDeltaPrecision::Precise)
            .taking_context;
        let Some(taking_context) = taking_context else {
            return Some(context.borrow_mut().route_first_wheel_step_for_testing(
                position,
                delta_in_css_pixels,
                NestedContextRegions::Ignore,
            ));
        };
        // Node ids are only unique within the process that made them.
        let ids_are_of_this_process = taking_context.context.is_some_and(|taking_context| {
            taking_context
                .borrow()
                .is_owned_by(context.borrow().web_content_client_id())
        });
        let mut route = FirstWheelStepRouteForTesting {
            route: libcompositing_rust::scrolling::FirstWheelStepRoute {
                decision: taking_context.route.decision,
                scroller: None,
            },
            document_id: None,
        };
        if ids_are_of_this_process {
            route.route.scroller = taking_context.route.scroller;
            route.document_id = taking_context.context.unwrap().borrow().document_id();
        }
        Some(route)
    }

    pub fn begin_input_trace_for_testing(&self, context_id: CompositorContextId) {
        let _entered = self.enter();

        if let Some(context) = self.context_if_present(context_id) {
            context.borrow_mut().input_trace_mut().begin();
        }
    }

    pub fn clear_per_test_input_state_for_testing(&self, context_id: CompositorContextId) {
        let _entered = self.enter();

        if let Some(context) = self.context_if_present(context_id) {
            context.borrow_mut().clear_per_test_input_state();
        }
    }

    pub fn take_input_trace_for_testing(&self, context_id: CompositorContextId) -> Vec<InputTraceRecord> {
        let _entered = self.enter();

        let Some(context) = self.context_if_present(context_id) else {
            return Vec::new();
        };
        context.borrow_mut().input_trace_mut().take()
    }
}
