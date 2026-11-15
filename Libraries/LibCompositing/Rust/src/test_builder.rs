/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::builder::DisplayListBuilder;
use crate::display_list::commands::{ClipNodeIndex, ContextRef, EffectNodeIndex, FillRoundedRectRing};
use crate::display_list::storage::DisplayListStorage;
use crate::ffi::{TestBuilderTree, test_builder};
use std::ffi::c_void;
use std::sync::Arc;

/// Has `test_builder` keep what `builder` recorded, against its finished tree, as the list it replays.
///
/// # Safety
///
/// `test_builder` must be a live, finished handle from `visual_context_tree_test_builder_create`.
unsafe fn record_against_finished_tree(test_builder: *mut c_void, builder: DisplayListBuilder) {
    // SAFETY: Guaranteed by the caller.
    let test_builder = unsafe { self::test_builder(test_builder) };
    let TestBuilderTree::Finished(tree) = &test_builder.tree else {
        panic!("lists are recorded against a finished test tree");
    };
    test_builder.recorded = Some(DisplayListStorage::recorded(Arc::new(builder.finish()), tree.clone()));
}

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create` that was not finished yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_image_filter(
    builder: *mut c_void,
    image_id: u64,
    rect: libgfx_rust::IntRect,
) -> u32 {
    let TestBuilderTree::Building(tree) = &mut (unsafe { test_builder(builder) }).tree else {
        panic!("nodes are added to a test tree before it is finished");
    };
    let filter = libgfx_rust::filter::Filter::Image {
        frame_id: image_id,
        src_rect: rect,
        dest_rect: rect,
        scaling_mode: libgfx_rust::ScalingMode::NearestNeighbor,
    };
    tree.append_effect(
        crate::visual_context::EffectNodeData::Effects(crate::visual_context::EffectsData {
            isolation: false,
            opacity: 1.0,
            blend_mode: libgfx_rust::CompositingAndBlendingOperator::Normal,
            filter: Some(std::sync::Arc::new(filter.serialize())),
            backdrop_filter: None,
        }),
        EffectNodeIndex::NONE,
        crate::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX,
        ClipNodeIndex::NONE,
    )
    .0
}

/// Records a rectangle with the same canonical builder as production recordings, against the finished tree of
/// `test_builder`, which keeps it as the list it replays.
///
/// # Safety
///
/// `test_builder` must be a live, finished handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_filled_rect(
    test_builder: *mut c_void,
    rect: libgfx_rust::IntRect,
    color: libgfx_rust::Color,
    context: ContextRef,
) {
    let mut builder = DisplayListBuilder::new();
    builder.append(
        &crate::display_list::commands::FillRect {
            rect,
            color,
            compositing_and_blending_operator: libgfx_rust::CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        },
        &[],
        context,
    );
    // SAFETY: Guaranteed by the caller.
    unsafe { record_against_finished_tree(test_builder, builder) };
}

/// Records a rounded rectangle ring against the finished tree of `test_builder`, which keeps it as the list it replays.
///
/// # Safety
///
/// `test_builder` must be a live, finished handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_rounded_rect_ring(
    test_builder: *mut c_void,
    ring: FillRoundedRectRing,
    context: ContextRef,
) {
    let mut builder = DisplayListBuilder::new();
    builder.append(&ring, &[], context);
    // SAFETY: Guaranteed by the caller.
    unsafe { record_against_finished_tree(test_builder, builder) };
}
