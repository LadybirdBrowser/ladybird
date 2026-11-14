/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::builder::DisplayListBuilder;
use crate::display_list::commands::{ClipNodeIndex, ContextRef, EffectNodeIndex, FillRoundedRectRing};
use crate::display_list::storage::recorded_storage_handle;
use crate::ffi::{TestBuilderTree, test_builder_tree};
use std::ffi::c_void;
use std::sync::Arc;

/// A storage handle for what `builder` recorded, against the finished tree of `test_builder`.
///
/// # Safety
///
/// `test_builder` must be a live, finished handle from `visual_context_tree_test_builder_create`.
unsafe fn recorded_against_finished_tree(test_builder: *mut c_void, builder: DisplayListBuilder) -> *const c_void {
    // SAFETY: Guaranteed by the caller.
    let TestBuilderTree::Finished(tree) = (unsafe { test_builder_tree(test_builder) }) else {
        panic!("lists are recorded against a finished test tree");
    };
    recorded_storage_handle(Arc::new(builder.finish()), tree.clone())
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
    let TestBuilderTree::Building(tree) = (unsafe { test_builder_tree(builder) }) else {
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
/// `test_builder`. Returns a storage handle the caller owns.
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
) -> *const c_void {
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
    unsafe { recorded_against_finished_tree(test_builder, builder) }
}

/// Records a rounded rectangle ring against the finished tree of `test_builder`. Returns a storage handle the caller
/// owns.
///
/// # Safety
///
/// `test_builder` must be a live, finished handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_rounded_rect_ring(
    test_builder: *mut c_void,
    ring: FillRoundedRectRing,
    context: ContextRef,
) -> *const c_void {
    let mut builder = DisplayListBuilder::new();
    builder.append(&ring, &[], context);
    // SAFETY: Guaranteed by the caller.
    unsafe { recorded_against_finished_tree(test_builder, builder) }
}
