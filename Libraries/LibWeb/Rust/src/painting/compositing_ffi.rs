/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry points the compositor process shares with WebContent: retaining and querying visual
//! context trees, replaying display lists, computing their damage, and the tree builder tests use.

use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::{ClipNodeIndex, ContextRef, EffectNodeIndex, SpatialNodeIndex};
use std::ffi::c_void;
use std::rc::Rc;

pub(crate) unsafe fn ffi_slice<'a, T>(data: *const T, length: usize) -> &'a [T] {
    assert!(!data.is_null() || length == 0);
    if length == 0 {
        return &[];
    }
    // SAFETY: The caller guarantees `data` points at `length` valid values for
    // the duration of the borrow.
    unsafe { std::slice::from_raw_parts(data, length) }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
pub(crate) unsafe fn tree_from_handle<'a>(
    tree: *const c_void,
) -> &'a crate::painting::visual_context::VisualContextTree {
    // SAFETY: The caller guarantees `tree` is a live retained handle.
    unsafe { &*tree.cast::<crate::painting::visual_context::VisualContextTree>() }
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
    let colors = unsafe { std::slice::from_raw_parts(samples.cast::<libgfx_rust::Color>(), count) };
    let features = crate::painting::force_dark::features_from_samples(colors, transparency_ratio);
    crate::painting::force_dark::should_filter(&features)
}

/// # Safety
///
/// Both trees must be live retained tree handles and every pointer must address the stated number of
/// bytes, runs or points for the call; each run table must be the validated table of its tape.
/// Writes the damage rect through `out_damage_rect` and returns whether the damage is bounded;
/// unbounded damage means the whole viewport must repaint.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_compute_damage(
    old_command_bytes: *const u8,
    old_command_bytes_length: usize,
    old_command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    old_command_run_count: usize,
    old_tree: *const c_void,
    old_scroll_offsets: *const libgfx_rust::FloatPoint,
    old_scroll_offsets_len: usize,
    new_command_bytes: *const u8,
    new_command_bytes_length: usize,
    new_command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    new_command_run_count: usize,
    new_tree: *const c_void,
    new_scroll_offsets: *const libgfx_rust::FloatPoint,
    new_scroll_offsets_len: usize,
    viewport_rect: libgfx_rust::IntRect,
    out_damage_rect: *mut libgfx_rust::IntRect,
) -> bool {
    let (old_tree, new_tree) = unsafe { (tree_from_handle(old_tree), tree_from_handle(new_tree)) };
    // SAFETY: The caller guarantees the slices address the stated number of values.
    let (old_command_bytes, old_command_runs, old_scroll_offsets) = unsafe {
        (
            ffi_slice(old_command_bytes, old_command_bytes_length),
            ffi_slice(old_command_runs, old_command_run_count),
            ffi_slice(old_scroll_offsets, old_scroll_offsets_len),
        )
    };
    // SAFETY: As above, for the new frame's inputs.
    let (new_command_bytes, new_command_runs, new_scroll_offsets) = unsafe {
        (
            ffi_slice(new_command_bytes, new_command_bytes_length),
            ffi_slice(new_command_runs, new_command_run_count),
            ffi_slice(new_scroll_offsets, new_scroll_offsets_len),
        )
    };
    let damage = crate::painting::display_list::damage::compute_display_list_damage(
        crate::painting::display_list::damage::DisplayListFrame {
            command_bytes: old_command_bytes,
            command_runs: old_command_runs,
            visual_context_tree: old_tree,
            scroll_offsets: old_scroll_offsets,
        },
        crate::painting::display_list::damage::DisplayListFrame {
            command_bytes: new_command_bytes,
            command_runs: new_command_runs,
            visual_context_tree: new_tree,
            scroll_offsets: new_scroll_offsets,
        },
        viewport_rect,
    );
    match damage {
        Some(damage_rect) => {
            // SAFETY: The caller guarantees `out_damage_rect` is writable.
            unsafe { *out_damage_rect = damage_rect };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// The tree must be a live retained handle and each pointer must address the stated number of values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_animated_content_may_affect_viewport(
    command_bytes: *const u8,
    command_bytes_length: usize,
    tree: *const c_void,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    viewport_rect: libgfx_rust::IntRect,
    sample_time_ns: i64,
) -> crate::painting::host::FfiAnimatedContentViewportEffect {
    // SAFETY: The caller guarantees a live tree and valid slices for the duration of the call.
    unsafe {
        crate::painting::display_list::damage::animated_content_may_affect_viewport_at(
            ffi_slice(command_bytes, command_bytes_length),
            tree_from_handle(tree),
            ffi_slice(scroll_offsets, scroll_offsets_len),
            viewport_rect,
            sample_time_ns,
        )
    }
}

/// # Safety
/// `tree` must be a live, structurally valid tree; `command_runs` must be valid for
/// `command_run_count` entries. The returned immutable plan is safe to share across
/// replay threads while its owner keeps it alive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_create_effect_clip_plan(
    tree: *const c_void,
    command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    command_run_count: usize,
) -> *const c_void {
    let tree = unsafe { tree_from_handle(tree) };
    let runs = unsafe { ffi_slice(command_runs, command_run_count) };
    crate::painting::display_list::effect_clip_plan::EffectClipPlan::new(tree, runs)
        .map_or(std::ptr::null(), |plan| Box::into_raw(Box::new(plan)).cast())
}

/// # Safety
/// `plan` must be a plan returned by `display_list_create_effect_clip_plan`, and
/// no thread may still be using it. This consumes ownership of the plan.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_destroy_effect_clip_plan(plan: *const c_void) {
    if !plan.is_null() {
        unsafe {
            drop(Box::from_raw(
                plan.cast_mut()
                    .cast::<crate::painting::display_list::effect_clip_plan::EffectClipPlan>(),
            ));
        }
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle, `command_runs` must address `command_run_count`
/// runs and `scroll_offsets` `scroll_offsets_len` points for the call, and `callbacks` must be
/// live. The painter callbacks run synchronously and may re-enter this function for a nested
/// display list. `effect_clip_plan` must remain live and must have been prepared
/// for these runs and the tree's current structural epoch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_replay(
    tree: *const c_void,
    effect_clip_plan: *const c_void,
    command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    command_run_count: usize,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    callbacks: *const crate::painting::host::FfiDisplayListReplayCallbacks,
) {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the slices address the stated number of values.
    let (command_runs, scroll_offsets) = unsafe {
        (
            ffi_slice(command_runs, command_run_count),
            ffi_slice(scroll_offsets, scroll_offsets_len),
        )
    };
    // SAFETY: The caller guarantees `callbacks` is live for the call.
    let mut painter = unsafe { *callbacks };
    let effect_clip_plan =
        unsafe { &*effect_clip_plan.cast::<crate::painting::display_list::effect_clip_plan::EffectClipPlan>() };
    crate::painting::display_list::replay::replay_display_list(
        tree,
        command_runs,
        effect_clip_plan,
        scroll_offsets,
        &mut painter,
    );
}

/// # Safety
///
/// `tree` must be a retained tree handle (from `layout_arena_main_visual_context_tree_retain`,
/// a host callback, or an earlier retain), used on the thread that owns it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_retain(tree: *const c_void) -> *const c_void {
    // SAFETY: The caller guarantees `tree` is a live retained handle, so the strong count is at least one.
    unsafe { Rc::increment_strong_count(tree.cast::<crate::painting::visual_context::VisualContextTree>()) };
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
    unsafe { Rc::decrement_strong_count(tree.cast::<crate::painting::visual_context::VisualContextTree>()) };
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
    let bytes = unsafe { ffi_slice(bytes, length) };
    match crate::painting::visual_context::VisualContextTree::from_bytes(bytes) {
        Some(tree) => Rc::into_raw(Rc::new(tree)).cast(),
        None => std::ptr::null(),
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_spatial_node_count(tree: *const c_void) -> usize {
    unsafe { tree_from_handle(tree) }.spatial_nodes.len()
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_context_is_valid(tree: *const c_void, context: ContextRef) -> bool {
    unsafe { tree_from_handle(tree) }.context_is_valid(context)
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
/// `tree` must be a live retained tree handle; `command_runs` must address `command_run_count`
/// runs for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_references_only_live_visual_context_nodes(
    tree: *const c_void,
    command_runs: *const crate::painting::display_list::commands::DisplayListCommandRun,
    command_run_count: usize,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the slice addresses the stated number of runs.
    let command_runs = unsafe { ffi_slice(command_runs, command_run_count) };
    tree.display_list_references_only_live_nodes(command_runs)
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len`
/// points and `out_local_point` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_transform_point_for_hit_test(
    tree: *const c_void,
    context: crate::painting::display_list::commands::ContextRef,
    screen_point: libgfx_rust::FloatPoint,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    respect_clip: bool,
    out_local_point: *mut libgfx_rust::FloatPoint,
) -> bool {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    let clip_behavior = crate::painting::visual_context::ClipBehavior::from_respect_clip(respect_clip);
    match tree.transform_point_for_hit_test(context, screen_point, scroll_offsets, clip_behavior) {
        Some(local_point) => {
            // SAFETY: The caller guarantees `out_local_point` is writable.
            unsafe { *out_local_point = local_point };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_inverse_transform_point(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    screen_point: libgfx_rust::FloatPoint,
) -> libgfx_rust::FloatPoint {
    unsafe { tree_from_handle(tree) }.inverse_transform_point(spatial, screen_point)
}

fn include_visual_viewport_transform_from(
    include: bool,
) -> crate::painting::visual_context::IncludeVisualViewportTransform {
    if include {
        crate::painting::visual_context::IncludeVisualViewportTransform::Yes
    } else {
        crate::painting::visual_context::IncludeVisualViewportTransform::No
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_transform_rect_to_viewport(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    rect: libgfx_rust::FloatRect,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    include_visual_viewport_transform: bool,
) -> libgfx_rust::FloatRect {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    tree.transform_rect_to_viewport(
        spatial,
        rect,
        scroll_offsets,
        include_visual_viewport_transform_from(include_visual_viewport_transform),
    )
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_cumulative_scroll_chain_offset(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
) -> libgfx_rust::FloatPoint {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    tree.cumulative_scroll_chain_offset(spatial, scroll_offsets)
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len` points.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_accumulated_matrix(
    tree: *const c_void,
    spatial: SpatialNodeIndex,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    include_visual_viewport_transform: bool,
) -> libgfx_rust::FloatMatrix4x4 {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points.
    let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
    tree.accumulated_matrix(
        spatial,
        scroll_offsets,
        include_visual_viewport_transform_from(include_visual_viewport_transform),
    )
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `scroll_offsets` must address `scroll_offsets_len`
/// points, and `push` is called synchronously with `sink` for every sticky node's resolved entry.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_resolve_sticky_offsets(
    tree: *const c_void,
    scroll_offsets: *const libgfx_rust::FloatPoint,
    scroll_offsets_len: usize,
    sink: *mut c_void,
    push: unsafe extern "C" fn(*mut c_void, SpatialNodeIndex, libgfx_rust::FloatPoint),
) {
    let resolved_sticky_entries = {
        let tree = unsafe { tree_from_handle(tree) };
        // SAFETY: The caller guarantees the offsets address `scroll_offsets_len` points, and the
        // borrow ends before the sink may grow the same storage.
        let scroll_offsets = unsafe { ffi_slice(scroll_offsets, scroll_offsets_len) };
        tree.resolve_sticky_offsets(scroll_offsets)
    };
    for (node_index, offset) in resolved_sticky_entries {
        // SAFETY: The C++ sink records the entry synchronously.
        unsafe { push(sink, node_index, offset) };
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_visual_viewport_transform(
    tree: *const c_void,
) -> crate::painting::host::FfiVisualViewportTransform {
    let tree = unsafe { tree_from_handle(tree) };
    let crate::painting::visual_context::SpatialData::Transform(transform) =
        &tree.spatial_nodes[crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX.0 as usize].data
    else {
        unreachable!("the visual viewport node is a transform");
    };
    crate::painting::host::FfiVisualViewportTransform {
        matrix: transform.matrix,
        origin: transform.origin,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle. Returns a retained handle to a copy of the tree whose
/// visual viewport node carries the given transform; the copy keeps the structural epoch.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_with_visual_viewport_transform(
    tree: *const c_void,
    transform: crate::painting::host::FfiVisualViewportTransform,
) -> *const c_void {
    let mut copy = unsafe { tree_from_handle(tree) }.clone();
    copy.set_visual_viewport_matrix_and_origin(transform.matrix, transform.origin);
    Rc::into_raw(Rc::new(copy)).cast()
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
    Rc::into_raw(Rc::new(sampled)).cast()
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
pub unsafe extern "C" fn visual_context_tree_has_active_visual_animation_at(
    tree: *const c_void,
    sample_time_ns: i64,
) -> bool {
    unsafe { tree_from_handle(tree) }.has_active_visual_animation_at(sample_time_ns)
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_visual_animation_summary(
    tree: *const c_void,
) -> crate::painting::host::FfiVisualAnimationSummary {
    unsafe { tree_from_handle(tree) }.visual_animation_summary()
}

/// # Safety
///
/// `tree` must be a live retained tree handle and `out_flags` must address one writable flag per
/// spatial node (`flag_count` of them).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_mark_spatial_subtrees_of_transform_animations(
    tree: *const c_void,
    out_flags: *mut bool,
    flag_count: usize,
) {
    let in_subtree = unsafe { tree_from_handle(tree) }.spatial_nodes_in_subtrees_of_transform_animations();
    assert_eq!(in_subtree.len(), flag_count);
    // SAFETY: The caller guarantees `out_flags` addresses `flag_count` writable flags.
    unsafe { std::ptr::copy_nonoverlapping(in_subtree.as_ptr(), out_flags, flag_count) };
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
/// `tree` must be a live retained tree handle and `out_opacity` must be writable. Returns whether
/// `effect` names an effects node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_effects_opacity(
    tree: *const c_void,
    effect: EffectNodeIndex,
    out_opacity: *mut f32,
) -> bool {
    match unsafe { tree_from_handle(tree) }.effects_opacity(effect) {
        Some(opacity) => {
            // SAFETY: The caller guarantees `out_opacity` is writable.
            unsafe { *out_opacity = opacity };
            true
        }
        None => false,
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `roots` must address `root_count` spatial indices and
/// `out_flags` must address one writable flag per spatial node (`flag_count` of them).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_mark_spatial_subtrees(
    tree: *const c_void,
    roots: *const SpatialNodeIndex,
    root_count: usize,
    out_flags: *mut bool,
    flag_count: usize,
) {
    let tree = unsafe { tree_from_handle(tree) };
    // SAFETY: The caller guarantees the roots address `root_count` indices.
    let roots = unsafe { ffi_slice(roots, root_count) };
    let in_subtree = tree.spatial_nodes_in_subtrees_of(roots);
    assert_eq!(in_subtree.len(), flag_count);
    // SAFETY: The caller guarantees `out_flags` addresses `flag_count` writable flags.
    unsafe { std::ptr::copy_nonoverlapping(in_subtree.as_ptr(), out_flags, flag_count) };
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_has_unisolated_destination_reading_effect(tree: *const c_void) -> bool {
    unsafe { tree_from_handle(tree) }.has_unisolated_destination_reading_effect()
}

/// # Safety
///
/// `tree` must be a live retained tree handle; `visit` is called synchronously with `context` for every
/// filter and backdrop filter an effect node carries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_for_each_effects_filter_bytes(
    tree: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let tree = unsafe { tree_from_handle(tree) };
    for node in &tree.effect_nodes {
        let crate::painting::visual_context::EffectNodeData::Effects(effects) = &node.data else {
            continue;
        };
        let backdrop_filter_bytes = effects.backdrop_filter.as_ref().map(|backdrop| &backdrop.filter);
        for filter_bytes in effects.filter.iter().chain(backdrop_filter_bytes) {
            // SAFETY: The C++ visitor reads the bytes synchronously.
            unsafe { visit(context, filter_bytes.as_ptr(), filter_bytes.len()) };
        }
    }
}

/// # Safety
///
/// `tree` must be a live retained tree handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_effect_is_isolated_by_layer(
    tree: *const c_void,
    effect: EffectNodeIndex,
) -> bool {
    unsafe { tree_from_handle(tree) }.effect_is_isolated_by_layer(effect)
}

/// # Safety
///
/// Returns a builder handle for hand-built test trees rooted at an identity visual viewport
/// transform; every builder call must receive it until `visual_context_tree_test_builder_finish`
/// or `visual_context_tree_test_builder_destroy` consumes it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_create() -> *mut c_void {
    let tree =
        crate::painting::visual_context::VisualContextTree::create(crate::painting::visual_context::TransformData {
            matrix: libgfx_rust::FloatMatrix4x4::identity(),
            origin: libgfx_rust::FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: crate::painting::visual_context::TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        });
    Box::into_raw(Box::new(tree)).cast()
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
unsafe fn test_builder_tree<'a>(builder: *mut c_void) -> &'a mut crate::painting::visual_context::VisualContextTree {
    // SAFETY: The caller guarantees `builder` is the live boxed tree the create call returned.
    unsafe { &mut *builder.cast::<crate::painting::visual_context::VisualContextTree>() }
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_transform(
    builder: *mut c_void,
    parent: u32,
    matrix: libgfx_rust::FloatMatrix4x4,
    origin: libgfx_rust::FloatPoint,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_spatial(
        crate::painting::visual_context::SpatialData::Transform(crate::painting::visual_context::TransformData {
            matrix,
            origin,
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: crate::painting::visual_context::TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }),
        SpatialNodeIndex(parent),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_scroll(builder: *mut c_void, parent: u32) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_spatial(
        crate::painting::visual_context::SpatialData::Scroll(crate::painting::visual_context::ScrollData {
            state_slot: crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: SpatialNodeIndex(parent),
        }),
        SpatialNodeIndex(parent),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_sticky(
    builder: *mut c_void,
    parent: u32,
    constraints: crate::painting::host::FfiTestStickyConstraints,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    let inset = |value: crate::painting::display_list::commands::OptionalF32| value.has_value.then_some(value.value);
    tree.append_spatial(
        crate::painting::visual_context::SpatialData::Sticky(crate::painting::visual_context::StickyData {
            scroller: SpatialNodeIndex(constraints.scroller),
            parent_sticky: constraints
                .has_parent_sticky
                .then_some(SpatialNodeIndex(constraints.parent_sticky)),
            position_relative_to_scroller: constraints.position_relative_to_scroller,
            border_box_size: constraints.border_box_size,
            scrollport_size: constraints.scrollport_size,
            containing_block_region: constraints.containing_block_region,
            needs_parent_offset_adjustment: constraints.needs_parent_offset_adjustment,
            inset_top: inset(constraints.inset_top),
            inset_right: inset(constraints.inset_right),
            inset_bottom: inset(constraints.inset_bottom),
            inset_left: inset(constraints.inset_left),
            state_slot: crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT,
            owner_paintable: NodeSlotId::INVALID,
            registry_parent_node: if constraints.has_parent_sticky {
                SpatialNodeIndex(constraints.parent_sticky)
            } else {
                SpatialNodeIndex(constraints.scroller)
            },
        }),
        SpatialNodeIndex(parent),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_clip(
    builder: *mut c_void,
    parent_clip: u32,
    spatial: u32,
    rect: libgfx_rust::FloatRect,
    corner_radii: libgfx_rust::CornerRadii,
    mode: crate::painting::visual_context::ClipMode,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_clip(
        crate::painting::visual_context::ClipNodeData::Rect(crate::painting::visual_context::ClipData {
            rect,
            corner_radii,
            mode,
        }),
        ClipNodeIndex(parent_clip),
        SpatialNodeIndex(spatial),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_background_color_animation(
    builder: *mut c_void,
    parent_effect: u32,
    spatial: u32,
    local_clip: u32,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_effect(
        crate::painting::visual_context::EffectNodeData::BackgroundColorAnimation,
        EffectNodeIndex(parent_effect),
        SpatialNodeIndex(spatial),
        ClipNodeIndex(local_clip),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`; `path_bytes`
/// must address `path_bytes_length` readable bytes of a serialized `Gfx::Path`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_clip_path(
    builder: *mut c_void,
    parent_clip: u32,
    spatial: u32,
    path_bytes: *const u8,
    path_bytes_length: usize,
    bounding_rect: libgfx_rust::IntRect,
    fill_rule: libgfx_rust::WindingRule,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    // SAFETY: The caller guarantees `path_bytes` addresses `path_bytes_length` readable bytes.
    let path_bytes = unsafe { ffi_slice(path_bytes, path_bytes_length) };
    let path = libgfx_rust::path::OwnedPath::from_serialized_bytes(path_bytes);
    tree.append_clip(
        crate::painting::visual_context::ClipNodeData::Path(crate::painting::visual_context::ClipPathData {
            path: Rc::new(path),
            bounding_rect,
            fill_rule,
        }),
        ClipNodeIndex(parent_clip),
        SpatialNodeIndex(spatial),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_effects(
    builder: *mut c_void,
    parent_effect: u32,
    spatial: u32,
    local_clip: u32,
    opacity: f32,
    blend_mode: libgfx_rust::CompositingAndBlendingOperator,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
    tree.append_effect(
        crate::painting::visual_context::EffectNodeData::Effects(crate::painting::visual_context::EffectsData {
            opacity,
            blend_mode,
            filter: None,
            backdrop_filter: None,
        }),
        EffectNodeIndex(parent_effect),
        SpatialNodeIndex(spatial),
        ClipNodeIndex(local_clip),
    )
    .0
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`. Gives the tree
/// the structural epoch another tree carries, so a test can stage a compatible tree-only update.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_set_structural_epoch(
    builder: *mut c_void,
    structural_epoch: u64,
) {
    let tree = unsafe { test_builder_tree(builder) };
    tree.structural_epoch = structural_epoch;
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`, consumed by this
/// call. Returns a retained tree handle the caller owns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_finish(builder: *mut c_void) -> *const c_void {
    // SAFETY: The caller hands over the boxed tree the create call returned.
    let tree = unsafe { Box::from_raw(builder.cast::<crate::painting::visual_context::VisualContextTree>()) };
    Rc::into_raw(Rc::new(*tree)).cast()
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
    drop(unsafe { Box::from_raw(builder.cast::<crate::painting::visual_context::VisualContextTree>()) });
}
