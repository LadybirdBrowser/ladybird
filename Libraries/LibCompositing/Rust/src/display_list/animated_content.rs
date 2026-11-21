/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Whether what the animations of a visual context tree move can reach the viewport.

use super::replay::{OwnSpaceClipExtent, own_space_clip_extent};
use super::storage::DisplayListStorage;
use crate::display_list::builder::{
    bounding_rect_computed_from_payload, for_each_command, has_inline_clip_in_its_own_space, inline_clips_of,
};
use crate::display_list::commands::{EffectNodeIndex, SpatialNodeIndex};
use crate::host::FfiAnimatedContentViewportEffect;
use crate::visual_context::{EffectNodeData, VisualContextTree, WorldTransforms};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, IntRect};
use std::cell::OnceCell;

/// Whether the content the tree's animations move at `sample_time_ns` can reach the viewport. The
/// finished animations keep their final values in place; the running ones must all be opacity or
/// plane rotations, whose extents the bounds query can replace, for the answer to be anything but
/// yes. The answer is cached until scene geometry or scrolling changes, and bounding rotating
/// content deliberately ignores clips, so no animation phase can reveal pixels outside the bounds.
pub fn animated_content_may_affect_viewport_at(
    display_list: &DisplayListStorage,
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    viewport_rect: IntRect,
    sample_time_ns: i64,
) -> FfiAnimatedContentViewportEffect {
    let extents = tree.visual_animation_extents_at(sample_time_ns);
    if !extents.all_running_animations_are_bounded {
        return FfiAnimatedContentViewportEffect {
            may_affect_viewport: true,
            stable_until_scene_changes: false,
        };
    }
    let may_affect_viewport_with_tree = |tree: &VisualContextTree| {
        animated_content_may_affect_viewport(
            display_list,
            tree,
            scroll_offsets,
            &extents.rotation_nodes,
            &extents.opacity_nodes,
            viewport_rect,
        )
    };
    let may_affect_viewport = if extents.has_finished_animation {
        may_affect_viewport_with_tree(&tree.with_visual_animation_samples(sample_time_ns))
    } else {
        may_affect_viewport_with_tree(tree)
    };
    FfiAnimatedContentViewportEffect {
        may_affect_viewport,
        stable_until_scene_changes: !extents.has_unfinished_finite_animation,
    }
}

pub fn animated_content_may_affect_viewport(
    display_list: &DisplayListStorage,
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    rotation_nodes: &[SpatialNodeIndex],
    opacity_nodes: &[EffectNodeIndex],
    viewport_rect: IntRect,
) -> bool {
    let is_in_rotating_subtree_by_spatial_node = tree.spatial_nodes_in_subtrees_of(rotation_nodes);
    let mut is_rotation_node_by_spatial_node = vec![false; tree.spatial_nodes.len()];
    for node in rotation_nodes {
        let Some(is_rotation_node) = is_rotation_node_by_spatial_node.get_mut(node.0 as usize) else {
            return true;
        };
        *is_rotation_node = true;
    }
    let world_made_on_first_use = OnceCell::new();
    let mut may_affect_viewport = false;
    for chunk in display_list.chunks() {
        let records = chunk.records();
        for run in chunk.runs() {
            let run_bytes = &records[run.offset as usize..(run.offset + run.size) as usize];
            for_each_command(run_bytes, |header, _, payload| {
                if may_affect_viewport || header.command_type.is_compositor_metadata() {
                    return;
                }
                let spatial_is_animated = is_in_rotating_subtree_by_spatial_node[run.context.spatial.0 as usize];
                let has_inline_clip_in_its_own_space =
                    header.inline_clip_count != 0 && has_inline_clip_in_its_own_space(header, payload);
                if has_inline_clip_in_its_own_space
                    && !spatial_is_animated
                    && inline_clips_of(header, payload).any(|inline_clip| {
                        inline_clip.has_own_space
                            && is_in_rotating_subtree_by_spatial_node
                                .get(inline_clip.space.0 as usize)
                                .is_none_or(|is_animated| *is_animated)
                    })
                {
                    may_affect_viewport = true;
                    return;
                }
                let mut clip = run.context.clip;
                while !clip.is_none() {
                    let node = &tree.clip_nodes[clip.0 as usize];
                    if !spatial_is_animated && is_in_rotating_subtree_by_spatial_node[node.spatial.0 as usize] {
                        may_affect_viewport = true;
                        return;
                    }
                    clip = node.parent;
                }
                let mut effect = run.context.effect;
                let mut opacity_is_animated = false;
                let mut filter_can_draw_past_record_bounds = false;
                while !effect.is_none() {
                    let node = &tree.effect_nodes[effect.0 as usize];
                    opacity_is_animated |= opacity_nodes.contains(&effect);
                    if !spatial_is_animated && is_in_rotating_subtree_by_spatial_node[node.spatial.0 as usize] {
                        may_affect_viewport = true;
                        return;
                    }
                    if let EffectNodeData::Effects(effects) = &node.data
                        && (effects.filter.is_some() || effects.backdrop_filter.is_some())
                    {
                        filter_can_draw_past_record_bounds = true;
                    }
                    effect = node.parent;
                }
                if !spatial_is_animated && !opacity_is_animated {
                    return;
                }
                if filter_can_draw_past_record_bounds {
                    may_affect_viewport = true;
                    return;
                }
                let bounding_rect = bounding_rect_computed_from_payload(header, payload);
                let mut rect = bounding_rect.unwrap_or_default().to_float();
                if has_inline_clip_in_its_own_space && bounding_rect.is_some() {
                    let world = world_made_on_first_use
                        .get_or_init(|| WorldTransforms::new(tree, scroll_offsets, Some(FloatMatrix4x4::identity())));
                    if let Some(command_to_root) = world.draw_matrix_if_known(run.context.spatial)
                        && let Some(OwnSpaceClipExtent::Within(clipped_rect)) =
                            own_space_clip_extent(header, payload, rect, command_to_root, |space| {
                                world.draw_matrix_if_known(space)
                            })
                    {
                        if clipped_rect.is_empty() {
                            return;
                        }
                        rect = clipped_rect;
                    }
                }
                let Some(bounds) = tree.rect_with_rotation_bounds_to_viewport(
                    run.context.spatial,
                    rect,
                    &is_rotation_node_by_spatial_node,
                    scroll_offsets,
                ) else {
                    may_affect_viewport = true;
                    return;
                };
                let bounds = bounds.inflated(2.0, 2.0);
                may_affect_viewport = bounds.x <= viewport_rect.right() as f32
                    && bounds.right() >= viewport_rect.x as f32
                    && bounds.y <= viewport_rect.bottom() as f32
                    && bounds.bottom() >= viewport_rect.y as f32;
            });
        }
    }
    may_affect_viewport
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::{DisplayListBuilder, PendingInlineClip};
    use crate::display_list::commands::{ContextRef, FillRect, VISUAL_VIEWPORT_NODE_INDEX};
    use crate::node_slot_id::NodeSlotId;
    use crate::visual_context::{
        ClipNodeIndex, EffectsData, ScrollData, SpatialData, TransformData, TransformDataRole,
    };
    use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatRect};

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        }
    }

    #[test]
    fn animated_content_follows_inline_clips_in_spaces_of_their_own() {
        let mut tree = VisualContextTree::create(transform(FloatMatrix4x4::identity()));
        let scroll = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: 1,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let rotating = tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let effect = tree.append_effect(
            EffectNodeData::Effects(EffectsData {
                isolation: false,
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        let viewport = IntRect::new(0, 0, 800, 600);
        let draw_clipped_to = |clip_space: SpatialNodeIndex, clip_rect: IntRect, context: ContextRef| {
            let mut builder = DisplayListBuilder::new();
            builder.append_with_inline_clips(
                &FillRect {
                    rect: viewport,
                    color: Color::from_rgb(255, 0, 0),
                    compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                    background_color_animation_effect: EffectNodeIndex::NONE,
                },
                &[],
                context,
                &[PendingInlineClip::intersecting_float_rect(clip_rect.to_float()).in_space(clip_space)],
            );
            DisplayListStorage::of_builder(&builder)
        };
        let may_affect_viewport = |display_list: &DisplayListStorage, rotation_nodes: &[SpatialNodeIndex]| {
            animated_content_may_affect_viewport(
                display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint::default()],
                rotation_nodes,
                &[effect],
                viewport,
            )
        };
        let faded = ContextRef {
            effect,
            ..ContextRef::default()
        };

        // An animated opacity only affects the viewport where the clip lets the draw show.
        let box_on_screen = IntRect::new(0, 10, 10, 10);
        let box_off_screen = IntRect::new(0, 2000, 10, 10);
        assert!(may_affect_viewport(&draw_clipped_to(scroll, box_on_screen, faded), &[]));
        assert!(!may_affect_viewport(
            &draw_clipped_to(scroll, box_off_screen, faded),
            &[]
        ));

        // A clip in a rotating space moves the draw it clips, though the draw's own space stands still.
        assert!(may_affect_viewport(
            &draw_clipped_to(rotating, box_on_screen, ContextRef::default()),
            &[rotating]
        ));
    }
}
