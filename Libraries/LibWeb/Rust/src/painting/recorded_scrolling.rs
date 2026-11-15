/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the scroll logic of the compositor makes of the recording a document published last, before the recording
//! reaches the compositor, for tests.

use crate::painting::display_list::storage::DisplayListStorage;
use crate::painting::presentation::display_list_properties_of;
use crate::painting::record::RecordingOutput;
use crate::render_state::DocumentHost;
use libcompositing_rust::scrolling::DisplayList;
use libgfx_rust::FloatPoint;
use std::ffi::c_void;
use std::sync::Arc;

/// Where the first step of a wheel gesture goes: `Compositing::FirstWheelStepRouteForTesting`, with the routing decision
/// and the scroller's kind as their `Web::InputRoutingDecision` and `Web::AsyncScrollNodeKind` values.
#[repr(C)]
#[derive(Default)]
pub struct FfiRecordedFirstWheelStepRoute {
    pub decision: u8,
    pub has_scroller: bool,
    pub scroller_node_id: i64,
    pub scroller_kind: u8,
    pub scroller_pseudo_element_type: u8,
    pub has_document_id: bool,
    pub document_id: i64,
}

/// The display list `output` published, as the compositor takes it, and the tree it was recorded against.
fn display_list_of(
    output: &RecordingOutput,
) -> Option<(DisplayList, Arc<crate::painting::visual_context::VisualContextTree>)> {
    let tree = output.visual_context_tree.clone()?;
    let properties = display_list_properties_of(output);
    let display_list = DisplayList::new(
        DisplayListStorage::recorded(output.display_list.clone(), tree.clone()),
        properties.id,
        properties.compatible_visual_context_tree_structural_epoch,
        output.async_scrolling_metadata.clone(),
        properties.surface_clear_color,
        properties.root_group_color,
    );
    Some((display_list, tree))
}

/// The recording `host`'s document published last, as the compositor takes it.
fn published_display_list(
    host: &DocumentHost,
) -> Option<(DisplayList, Arc<crate::painting::visual_context::VisualContextTree>)> {
    let recording = host.recording().published_recording()?;
    display_list_of(&recording)
}

/// Hands `receive` the scrolling state of the recording `host`'s document published last, as JSON, or the empty one
/// where it published none.
///
/// # Safety
///
/// `receive` must copy the UTF-8 bytes it keeps synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_recorded_async_scrolling_state_json(
    host: &DocumentHost,
    receiver: *mut c_void,
    receive: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let display_list = published_display_list(host);
    let json = libcompositing_rust::ffi::recorded_display_list_async_scrolling_state_json(
        display_list.as_ref().map(|(display_list, tree)| (display_list, tree)),
    );
    // SAFETY: The caller copies what it keeps of the JSON synchronously.
    unsafe { receive(receiver, json.as_ptr(), json.len()) };
}

/// Hands `receive` the scrolling state of no display list, the empty one, as JSON.
///
/// # Safety
///
/// `receive` must copy the UTF-8 bytes it keeps synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_empty_async_scrolling_state_json(
    receiver: *mut c_void,
    receive: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let json = libcompositing_rust::ffi::recorded_display_list_async_scrolling_state_json(None);
    // SAFETY: The caller copies what it keeps of the JSON synchronously.
    unsafe { receive(receiver, json.as_ptr(), json.len()) };
}

/// Writes where the first step of a wheel gesture at `position` by `delta_in_css_pixels` goes through the recording
/// `host`'s document published last to `out`, and answers whether it published one.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_recorded_route_first_wheel_step(
    host: &DocumentHost,
    newest_wheel_event_listener_state_generation: u64,
    position: FloatPoint,
    delta_in_css_pixels: FloatPoint,
    out: &mut FfiRecordedFirstWheelStepRoute,
) -> bool {
    let Some((display_list, tree)) = published_display_list(host) else {
        return false;
    };
    let route = libcompositing_rust::ffi::recorded_display_list_route_first_wheel_step(
        &display_list,
        &tree,
        (*host.scroll_state()).clone(),
        newest_wheel_event_listener_state_generation,
        position,
        delta_in_css_pixels,
    );
    *out = FfiRecordedFirstWheelStepRoute {
        decision: route.decision as u8,
        has_scroller: route.has_scroller,
        scroller_node_id: route.scroller_node_id,
        scroller_kind: route.scroller_kind as u8,
        scroller_pseudo_element_type: route.scroller_pseudo_element_type,
        has_document_id: route.has_document_id,
        document_id: route.document_id,
    };
    true
}

/// Whether a box of the recording `host`'s document published last blocks a wheel event at `position`.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_recorded_blocks_wheel_event_at(host: &DocumentHost, position: FloatPoint) -> bool {
    let Some((display_list, tree)) = published_display_list(host) else {
        return false;
    };
    libcompositing_rust::ffi::recorded_display_list_blocks_wheel_event_at(
        &display_list,
        &tree,
        (*host.scroll_state()).clone(),
        position,
    )
}
