/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::commands::{ClipNodeIndex, ContextRef, EffectNodeIndex};
use crate::ffi::test_builder_tree;
use std::ffi::c_void;

/// # Safety
///
/// `builder` must be a live handle from `visual_context_tree_test_builder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn visual_context_tree_test_builder_append_image_filter(
    builder: *mut c_void,
    image_id: u64,
    rect: libgfx_rust::IntRect,
) -> u32 {
    let tree = unsafe { test_builder_tree(builder) };
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

/// Records a rectangle with the same canonical builder as production recordings. Returns one
/// owned Arc reference consumed by `display_list_storage_adopt_recorded`.
#[unsafe(no_mangle)]
pub extern "C" fn visual_context_tree_test_builder_filled_rect(
    rect: libgfx_rust::IntRect,
    color: libgfx_rust::Color,
    context: ContextRef,
) -> *const c_void {
    let mut builder = crate::display_list::builder::DisplayListBuilder::new();
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
    std::sync::Arc::into_raw(std::sync::Arc::new(builder.finish())).cast()
}
