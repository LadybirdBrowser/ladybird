/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the compositor process shares with WebContent: the force-dark classification of images, smooth scroll
//! animations, and the scroll queries of recorded display lists.

use crate::scrolling::smooth_scroll_animation::SmoothScrollAnimation;
use crate::scrolling::{
    AccumulatedVisualContextTree, AsyncScrollNodeKind, DisplayList, InputRoutingDecision, ScrollAnimationKind,
};
use crate::time::Duration;
use libgfx_rust::FloatPoint;
use std::sync::Arc;

/// Decide if force-dark should invert an image: the caller owns the sampling, this owns the policy. Returns false
/// for an empty or null buffer, which leaves the image untouched.
///
/// # Safety
/// `samples` must point to `count` packed colors, or be null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ladybird_web_force_dark_should_filter_image(
    samples: *const u32,
    count: usize,
    transparency_ratio: f32,
) -> bool {
    if samples.is_null() || count == 0 {
        return false;
    }
    // Color is repr(transparent) over u32, so the buffer needs no copy to be read as colors.
    let colors = unsafe { ak::slice_from_raw_parts(samples.cast::<libgfx_rust::Color>(), count) };
    let features = crate::force_dark::features_from_samples(colors, transparency_ratio);
    crate::force_dark::should_filter(&features)
}

#[unsafe(no_mangle)]
pub extern "C" fn smooth_scroll_animation_create(
    start_offset: FloatPoint,
    destination_offset: FloatPoint,
    pixels_per_css_pixel: f64,
    kind: ScrollAnimationKind,
) -> *mut SmoothScrollAnimation {
    Box::into_raw(Box::new(SmoothScrollAnimation::new(
        start_offset,
        destination_offset,
        pixels_per_css_pixel,
        kind,
    )))
}

/// # Safety
///
/// `animation` must be null or a handle from `smooth_scroll_animation_create` that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn smooth_scroll_animation_destroy(animation: *mut SmoothScrollAnimation) {
    if animation.is_null() {
        return;
    }
    // SAFETY: The caller hands over the animation the create call returned.
    drop(unsafe { Box::from_raw(animation) });
}

#[unsafe(no_mangle)]
pub extern "C" fn smooth_scroll_animation_kind(animation: &SmoothScrollAnimation) -> ScrollAnimationKind {
    animation.kind()
}

#[unsafe(no_mangle)]
pub extern "C" fn smooth_scroll_animation_sample(
    animation: &SmoothScrollAnimation,
    elapsed_nanoseconds: i64,
    offset: &mut FloatPoint,
) -> bool {
    let sample = animation.sample(Duration::from_nanoseconds(elapsed_nanoseconds));
    *offset = sample.offset;
    sample.complete
}

#[unsafe(no_mangle)]
pub extern "C" fn smooth_scroll_animation_retarget(
    animation: &mut SmoothScrollAnimation,
    destination_offset: FloatPoint,
    elapsed_nanoseconds: i64,
) {
    animation.retarget(destination_offset, Duration::from_nanoseconds(elapsed_nanoseconds));
}

/// The scrolling state of `display_list`, a list this process recorded, and the tree it was recorded against, as JSON,
/// or of no list, which is the empty one.
pub fn recorded_display_list_async_scrolling_state_json(
    display_list: Option<(&DisplayList, &Arc<crate::visual_context::VisualContextTree>)>,
) -> String {
    let state = match display_list {
        None => crate::scrolling::AsyncScrollingState::default(),
        Some((display_list, visual_context_tree)) => {
            crate::scrolling::async_scrolling_state_from(display_list, visual_context_tree)
        }
    };
    crate::scrolling::async_scrolling_state_to_json(&state)
}

/// The scroll tree of `display_list`, the list the document last recorded, with the tree it was recorded against, as
/// the compositor builds it once the list reaches it.
fn recorded_scroll_tree_of(
    display_list: &DisplayList,
    visual_context_tree: &Arc<crate::visual_context::VisualContextTree>,
    mut snapshot: crate::scrolling::ScrollStateSnapshot,
    newest_wheel_event_listener_state_generation: u64,
) -> (
    crate::scrolling::AsyncScrollTree,
    AccumulatedVisualContextTree,
    crate::scrolling::WheelRoutingAdmission,
) {
    let visual_context_tree = AccumulatedVisualContextTree::from_arc(visual_context_tree.clone());
    snapshot.set_node_count(visual_context_tree.spatial_node_count());
    let async_scrolling_state =
        crate::scrolling::async_scrolling_state_from(display_list, visual_context_tree.as_arc());
    let admission = crate::scrolling::wheel_routing_admission_for_with_generation(
        &async_scrolling_state,
        newest_wheel_event_listener_state_generation,
    );
    let mut scroll_tree = crate::scrolling::AsyncScrollTree::default();
    scroll_tree.set_state(async_scrolling_state);
    scroll_tree.rebuild_wheel_hit_test_targets(Some(display_list), Some(&visual_context_tree), &snapshot);
    (scroll_tree, visual_context_tree, admission)
}

/// Where the first step of a wheel gesture goes, as `Compositing::FirstWheelStepRouteForTesting` holds it.
#[repr(C)]
pub struct FfiFirstWheelStepRouteForTesting {
    pub decision: InputRoutingDecision,
    pub has_scroller: bool,
    pub scroller_node_id: i64,
    pub scroller_kind: AsyncScrollNodeKind,
    pub scroller_pseudo_element_type: u8,
    pub has_document_id: bool,
    pub document_id: i64,
}

/// Where the first step of a wheel gesture at `position` by `delta_in_css_pixels` goes through `display_list`, a list this
/// process recorded, and the tree it was recorded against, with the document scrolled as `snapshot` says.
pub fn recorded_display_list_route_first_wheel_step(
    display_list: &DisplayList,
    visual_context_tree: &Arc<crate::visual_context::VisualContextTree>,
    snapshot: crate::scrolling::ScrollStateSnapshot,
    newest_wheel_event_listener_state_generation: u64,
    position: FloatPoint,
    delta_in_css_pixels: FloatPoint,
) -> FfiFirstWheelStepRouteForTesting {
    let (scroll_tree, visual_context_tree, admission) = recorded_scroll_tree_of(
        display_list,
        visual_context_tree,
        snapshot,
        newest_wheel_event_listener_state_generation,
    );
    let delta = delta_in_css_pixels.scaled(scroll_tree.device_pixels_per_css_pixel() as f32);
    let first_step = scroll_tree.route_first_wheel_step(
        admission,
        &visual_context_tree,
        position,
        delta,
        crate::scrolling::NestedContextRegions::LeaveToMainThread,
    );
    let document_id = scroll_tree.document_id();
    FfiFirstWheelStepRouteForTesting {
        decision: first_step.decision,
        has_scroller: first_step.scroller.is_some(),
        scroller_node_id: first_step.scroller.map_or(0, |scroller| scroller.node_id.0),
        scroller_kind: first_step
            .scroller
            .map_or(AsyncScrollNodeKind::Element, |scroller| scroller.kind),
        scroller_pseudo_element_type: first_step.scroller.map_or(0, |scroller| scroller.pseudo_element_type),
        has_document_id: document_id.is_some(),
        document_id: document_id.map_or(0, |document_id| document_id.0),
    }
}

/// Whether a box of `display_list`, a list this process recorded, blocks a wheel event at `position`, with the document
/// scrolled as `snapshot` says.
pub fn recorded_display_list_blocks_wheel_event_at(
    display_list: &DisplayList,
    visual_context_tree: &Arc<crate::visual_context::VisualContextTree>,
    snapshot: crate::scrolling::ScrollStateSnapshot,
    position: FloatPoint,
) -> bool {
    let (scroll_tree, visual_context_tree, _) = recorded_scroll_tree_of(display_list, visual_context_tree, snapshot, 0);
    scroll_tree.blocks_wheel_event_at_position(&visual_context_tree, position)
}
