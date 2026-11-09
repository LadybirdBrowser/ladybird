/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points the compositor process shares with WebContent: retaining and querying visual
//! context trees, replaying display lists, computing their damage, and the tree builder tests use.

use crate::display_list::commands::{EffectNodeIndex, SpatialNodeIndex};
use crate::display_list::storage::storage_from_handle;
use crate::scrolling::smooth_scroll_animation::SmoothScrollAnimation;
use crate::scrolling::{
    AccumulatedVisualContextTree, AsyncScrollNodeKind, DisplayList, InputRoutingDecision, ScrollAnimationKind,
};
use crate::time::Duration;
use libgfx_rust::FloatPoint;
use libipc_rust::DecodeError;
use libipc_rust::cpp_connection::EncodedValue;
use std::ffi::c_void;
use std::sync::Arc;

/// # Safety
///
/// `tree` must be a live retained tree handle.
pub unsafe fn tree_from_handle<'a>(tree: *const c_void) -> &'a crate::visual_context::VisualContextTree {
    // SAFETY: The caller guarantees `tree` is a live retained handle.
    unsafe { &*tree.cast::<crate::visual_context::VisualContextTree>() }
}

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

/// Replays the list `storage` holds against `tree`, with the scroll containers at `scroll_offsets`.
///
/// # Safety
///
/// `storage` must be a live storage handle of a list recorded against the structural epoch of `tree`, and `callbacks`
/// must be live. The painter callbacks run synchronously and may re-enter a replay for a nested display list.
pub unsafe fn replay_display_list_storage(
    tree: &crate::visual_context::VisualContextTree,
    storage: *const c_void,
    root_group_color: libgfx_rust::Color,
    scroll_offsets: &[FloatPoint],
    callbacks: *const crate::host::FfiDisplayListReplayCallbacks,
) {
    // SAFETY: The caller guarantees a live handle for the duration of the call.
    let storage = unsafe { storage_from_handle(storage) };
    // SAFETY: The caller guarantees `callbacks` is live for the call.
    let mut painter = unsafe { *callbacks };
    crate::display_list::replay::replay_display_list(
        tree,
        storage.bytes(),
        storage.command_runs(),
        storage.effect_clip_plan(tree),
        root_group_color,
        scroll_offsets,
        &mut painter,
    );
}

/// Plays a stream of records that a command nests, outside of any run, with the scroll containers at `scroll_offsets`.
///
/// # Safety
///
/// `records` must be a tape that is valid, and `callbacks` must be live. The painter callbacks run synchronously and
/// may re-enter a replay.
pub unsafe fn replay_nested_records(
    records: &[u8],
    scroll_offsets: &[FloatPoint],
    callbacks: *const crate::host::FfiDisplayListReplayCallbacks,
) {
    // SAFETY: The caller guarantees `callbacks` is live for the call.
    let mut painter = unsafe { *callbacks };
    // NB: Nested records share the context of the command they are nested in, and none of their inline clips has a
    //     space of its own.
    crate::display_list::replay::replay_records(records, scroll_offsets, &|_| None, &mut painter);
}

/// Replays a list with nothing scrolled, as a list nested in another is.
///
/// # Safety
///
/// `tree` must be a live retained tree handle of the structural epoch the storage was recorded
/// against, `storage` a live storage handle, and `callbacks` must be live. The painter callbacks run
/// synchronously and may re-enter this function for a nested display list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_replay(
    tree: *const c_void,
    storage: *const c_void,
    root_group_color: libgfx_rust::Color,
    callbacks: *const crate::host::FfiDisplayListReplayCallbacks,
) {
    // SAFETY: The caller guarantees live handles and callbacks for the duration of the call.
    unsafe { replay_display_list_storage(tree_from_handle(tree), storage, root_group_color, &[], callbacks) };
}

/// Plays a stream of records that a command nests, outside of any run, with nothing scrolled.
///
/// # Safety
///
/// `records` must address `records_size` bytes of a tape that is valid for the call; `callbacks` must be
/// live. The painter callbacks run synchronously and may re-enter this function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_replay_records(
    records: *const u8,
    records_size: usize,
    callbacks: *const crate::host::FfiDisplayListReplayCallbacks,
) {
    // SAFETY: The caller guarantees the slice addresses the stated number of bytes, and live callbacks.
    unsafe { replay_nested_records(ak::slice_from_raw_parts(records, records_size), &[], callbacks) };
}

/// # Safety
///
/// `tree` must be a retained tree handle (from `layout_arena_main_visual_context_tree_retain`,
/// a host callback, or an earlier retain), used on the thread that owns it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_retain(tree: *const c_void) -> *const c_void {
    // SAFETY: The caller guarantees `tree` is a live retained handle, so the strong count is at least one.
    unsafe { std::sync::Arc::increment_strong_count(tree.cast::<crate::visual_context::VisualContextTree>()) };
    tree
}

/// # Safety
///
/// `tree` must be null or a retained tree handle that the caller gives up with this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_release(tree: *const c_void) {
    if tree.is_null() {
        return;
    }
    // SAFETY: The caller gives up the reference it retained, and the strong count is at least one.
    unsafe { std::sync::Arc::decrement_strong_count(tree.cast::<crate::visual_context::VisualContextTree>()) };
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_structural_epoch(tree: *const c_void) -> u64 {
    unsafe { tree_from_handle(tree) }.structural_epoch
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `append` is called synchronously with `sink`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_serialize(
    tree: *const c_void,
    sink: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let bytes = unsafe { tree_from_handle(tree) }.to_bytes();
    // SAFETY: The C++ sink copies the bytes synchronously.
    unsafe { append(sink, bytes.as_ptr(), bytes.len()) };
}

/// # Safety
///
/// `bytes` must address `length` readable bytes for the call. Returns a retained tree handle the
/// caller owns, or null when the bytes do not describe a valid tree.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_deserialize(bytes: *const u8, length: usize) -> *const c_void {
    // SAFETY: The caller guarantees `bytes` addresses `length` readable bytes.
    let bytes = unsafe { ak::slice_from_raw_parts(bytes, length) };
    match crate::visual_context::VisualContextTree::from_bytes(bytes) {
        Ok(tree) => std::sync::Arc::into_raw(std::sync::Arc::new(tree)).cast(),
        Err(_) => std::ptr::null(),
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle. Every node slot, live or dead.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.node_count()
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_live_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.live_node_count()
}

/// # Safety
///
/// `tree` must be a live retained tree handle and `storage` a live storage handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_references_only_live_visual_context_nodes(
    tree: *const c_void,
    storage: *const c_void,
) -> bool {
    // SAFETY: The caller guarantees live handles for the duration of the call.
    let (tree, storage) = unsafe { (tree_from_handle(tree), storage_from_handle(storage)) };
    tree.display_list_references_only_live_nodes(storage.command_runs())
}

/// Whether the node's scroll translations and the origins drawn in its space round to whole device pixels: no
/// transform but identities lies above it.
///
/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_translations_snap_to_device_pixels(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
) -> bool {
    unsafe { tree_from_handle(tree) }.translations_snap_to_device_pixels(spatial)
}

/// # Safety
///
/// `tree` must be a live retained tree handle. Returns a retained handle to a copy of the tree whose
/// nodes carry the values its animations take at `sample_time_ns`; the copy keeps the structural
/// epoch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_with_visual_animation_samples(
    tree: *const c_void,
    sample_time_ns: i64,
) -> *const c_void {
    let sampled = unsafe { tree_from_handle(tree) }.with_visual_animation_samples(sample_time_ns);
    std::sync::Arc::into_raw(std::sync::Arc::new(sampled)).cast()
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_has_visual_animations(tree: *const c_void) -> bool {
    unsafe { tree_from_handle(tree) }.has_visual_animations()
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_visual_animation_summary(
    tree: *const c_void,
) -> crate::host::FfiVisualAnimationSummary {
    unsafe { tree_from_handle(tree) }.visual_animation_summary()
}

/// # Safety
///
/// `tree` must be a live retained tree handle and `color` must point to writable storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_sampled_background_color(
    tree: *const c_void,
    effect: EffectNodeIndex,
    color: *mut libgfx_rust::Color,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    let Some(sampled_color) = tree.sampled_background_color(effect) else {
        return false;
    };
    // SAFETY: The caller guarantees that `color` points to writable storage.
    unsafe { *color = sampled_color };
    true
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `visit` is called synchronously with `context` for the id
/// of every image frame that a filter or backdrop filter of an effect node draws.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_for_each_effects_filter_image_frame_id(
    tree: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, u64),
) {
    let tree = unsafe { tree_from_handle(tree) };
    for node in &tree.effect_nodes {
        let crate::visual_context::EffectNodeData::Effects(effects) = &node.data else {
            continue;
        };
        let backdrop_filter_bytes = effects.backdrop_filter.as_ref().map(|backdrop| &backdrop.filter);
        for filter_bytes in effects.filter.iter().chain(backdrop_filter_bytes) {
            let Some(filter) = libgfx_rust::filter::Filter::deserialize(filter_bytes) else {
                continue;
            };
            // SAFETY: The caller guarantees `visit` accepts `context`.
            filter.for_each_image_frame_id(&mut |id| unsafe { visit(context, id) });
        }
    }
}

/// # Safety
///
/// Returns a builder handle for hand-built test trees rooted at an identity visual viewport
/// transform; every builder call must receive it until `visual_context_tree_test_builder_finish`
/// or `visual_context_tree_test_builder_destroy` consumes it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_create() -> *mut c_void {
    let tree = crate::visual_context::VisualContextTree::create(crate::visual_context::TransformData {
        matrix: libgfx_rust::FloatMatrix4x4::identity(),
        origin: libgfx_rust::FloatPoint::default(),
        sorting_context_root_index: None,
        flattens_inherited_transform: false,
        role: crate::visual_context::TransformDataRole::CssTransform,
        synthetic_plane: false,
        establishes_sorting_context: false,
        bounds: libgfx_rust::FloatRect::default(),
    });
    Box::into_raw(Box::new(tree)).cast()
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
pub(super) unsafe fn test_builder_tree<'a>(builder: *mut c_void) -> &'a mut crate::visual_context::VisualContextTree {
    // SAFETY: The caller guarantees `builder` is the live boxed tree the create call returned.
    unsafe { &mut *builder.cast::<crate::visual_context::VisualContextTree>() }
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`, consumed by this
/// call. Returns a retained tree handle the caller owns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_finish(builder: *mut c_void) -> *const c_void {
    // SAFETY: The caller hands over the boxed tree the create call returned.
    let tree = unsafe { Box::from_raw(builder.cast::<crate::visual_context::VisualContextTree>()) };
    std::sync::Arc::into_raw(std::sync::Arc::new(*tree)).cast()
}

/// # Safety
///
/// `builder` must be null or a live handle from `visual_context_tree_test_builder_create`, consumed
/// by this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_destroy(builder: *mut c_void) {
    if builder.is_null() {
        return;
    }
    // SAFETY: The caller hands over the boxed tree the create call returned.
    drop(unsafe { Box::from_raw(builder.cast::<crate::visual_context::VisualContextTree>()) });
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

/// A C++ display list as the scroll logic takes it: its tape, and what travels beside the tape that the logic reads.
#[repr(C)]
pub struct FfiDisplayList<'a> {
    pub storage: *const c_void,
    pub id: u64,
    pub compatible_visual_context_tree_structural_epoch: u64,
    /// A `Compositing::DisplayList::AsyncScrollingMetadata`, or none.
    pub async_scrolling_metadata: EncodedValue<'a>,
}

impl FfiDisplayList<'_> {
    /// # Safety
    ///
    /// `storage` must be a live display list storage handle.
    pub unsafe fn share(&self) -> Result<DisplayList, DecodeError> {
        let async_scrolling_metadata = self.async_scrolling_metadata.decode()?;
        let storage = self.storage.cast::<crate::display_list::storage::DisplayListStorage>();
        // SAFETY: The caller lends a strong reference to the storage, of which this takes one more.
        let storage = unsafe {
            Arc::increment_strong_count(storage);
            Arc::from_raw(storage)
        };
        Ok(DisplayList::new(
            storage,
            self.id,
            self.compatible_visual_context_tree_structural_epoch,
            async_scrolling_metadata,
        ))
    }
}

/// # Safety
///
/// `visual_context_tree` must be a live retained tree handle.
pub unsafe fn tree_sharing_handle(visual_context_tree: *const c_void) -> AccumulatedVisualContextTree {
    let tree = visual_context_tree.cast::<crate::visual_context::VisualContextTree>();
    // SAFETY: The caller lends a strong reference to the tree, of which this takes one more.
    AccumulatedVisualContextTree::from_arc(unsafe {
        Arc::increment_strong_count(tree);
        Arc::from_raw(tree)
    })
}

const ENCODED_IN_THIS_PROCESS: &str = "a value this process encoded for itself decodes";

/// The scrolling state of no display list is the empty one.
///
/// # Safety
///
/// The storage of `display_list` must be a live display list storage handle, and `visual_context_tree` a live tree
/// handle where there is a display list. `receive` must copy the UTF-8 bytes it keeps synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn recorded_display_list_async_scrolling_state_json(
    display_list: Option<&FfiDisplayList<'_>>,
    visual_context_tree: *const c_void,
    receiver: *mut c_void,
    receive: unsafe extern "C" fn(*mut c_void, ak::Span<'_, u8>),
) {
    let state = match display_list {
        None => crate::scrolling::AsyncScrollingState::default(),
        Some(display_list) => {
            // SAFETY: The caller lends live storage and tree handles.
            let (display_list, visual_context_tree) = unsafe {
                (
                    display_list.share().expect(ENCODED_IN_THIS_PROCESS),
                    tree_sharing_handle(visual_context_tree),
                )
            };
            crate::scrolling::async_scrolling_state_from(&display_list, visual_context_tree.as_arc())
        }
    };
    let json = crate::scrolling::async_scrolling_state_to_json(&state);
    // SAFETY: The caller copies what it keeps of the JSON synchronously.
    unsafe { receive(receiver, ak::Span::new(json.as_bytes())) };
}

/// The scroll tree of the display list the document last recorded, as the compositor builds it once the list reaches
/// it.
///
/// # Safety
///
/// The storage of `display_list` and `visual_context_tree` must be live handles.
unsafe fn recorded_scroll_tree_of(
    display_list: &FfiDisplayList<'_>,
    visual_context_tree: *const c_void,
    scroll_state_snapshot: EncodedValue<'_>,
    newest_wheel_event_listener_state_generation: u64,
) -> (
    crate::scrolling::AsyncScrollTree,
    AccumulatedVisualContextTree,
    crate::scrolling::WheelRoutingAdmission,
) {
    // SAFETY: The caller lends live storage and tree handles.
    let (display_list, visual_context_tree) = unsafe {
        (
            display_list.share().expect(ENCODED_IN_THIS_PROCESS),
            tree_sharing_handle(visual_context_tree),
        )
    };
    let mut snapshot = scroll_state_snapshot
        .decode::<crate::scrolling::ScrollStateSnapshot>()
        .expect(ENCODED_IN_THIS_PROCESS);
    snapshot.set_node_count(visual_context_tree.spatial_node_count());
    let async_scrolling_state =
        crate::scrolling::async_scrolling_state_from(&display_list, visual_context_tree.as_arc());
    let admission = crate::scrolling::wheel_routing_admission_for_with_generation(
        &async_scrolling_state,
        newest_wheel_event_listener_state_generation,
    );
    let mut scroll_tree = crate::scrolling::AsyncScrollTree::default();
    scroll_tree.set_state(async_scrolling_state);
    scroll_tree.rebuild_wheel_hit_test_targets(Some(&display_list), Some(&visual_context_tree), &snapshot);
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

/// # Safety
///
/// The storage of `display_list` and `visual_context_tree` must be live handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn recorded_display_list_route_first_wheel_step(
    display_list: &FfiDisplayList<'_>,
    visual_context_tree: *const c_void,
    scroll_state_snapshot: EncodedValue<'_>,
    newest_wheel_event_listener_state_generation: u64,
    position: FloatPoint,
    delta_in_css_pixels: FloatPoint,
) -> FfiFirstWheelStepRouteForTesting {
    // SAFETY: The caller lends live handles.
    let (scroll_tree, visual_context_tree, admission) = unsafe {
        recorded_scroll_tree_of(
            display_list,
            visual_context_tree,
            scroll_state_snapshot,
            newest_wheel_event_listener_state_generation,
        )
    };
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

/// # Safety
///
/// The storage of `display_list` and `visual_context_tree` must be live handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn recorded_display_list_blocks_wheel_event_at(
    display_list: &FfiDisplayList<'_>,
    visual_context_tree: *const c_void,
    scroll_state_snapshot: EncodedValue<'_>,
    position: FloatPoint,
) -> bool {
    // SAFETY: The caller lends live handles.
    let (scroll_tree, visual_context_tree, _) =
        unsafe { recorded_scroll_tree_of(display_list, visual_context_tree, scroll_state_snapshot, 0) };
    scroll_tree.blocks_wheel_event_at_position(&visual_context_tree, position)
}
