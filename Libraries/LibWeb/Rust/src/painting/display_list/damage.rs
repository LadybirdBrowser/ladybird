/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::display_list::builder::{for_each_command, inline_transform_entry_offset, read_command};
use crate::painting::display_list::commands::{
    ContextRef, DisplayListCommandHeader, DisplayListCommandType, DisplayListDataSpan, DisplayListInlineClip,
    DrawGlyphRun, DrawScaledDecodedImageFrame, EffectNodeIndex, INLINE_CLIP_ENTRY_SIZE, OptionalColor,
    OptionalFloatRect, PaintScrollBar, PaintTextShadow, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
};
use crate::painting::visual_context::queries::TreeCullingScratch;
use crate::painting::visual_context::{
    ClipNodeData, EffectNodeData, IncludeVisualViewportTransform, SpatialData, VisualContextTree,
    device_offset_for_index,
};
use libgfx_rust::{AffineTransform, FloatPoint, FloatRect, IntRect, enclosing_int_rect};
use std::mem::offset_of;
use std::rc::Rc;

struct CommandReference<'a> {
    header: DisplayListCommandHeader,
    payload: &'a [u8],
}

fn collect_command_references(command_bytes: &[u8]) -> Vec<CommandReference<'_>> {
    let mut commands = Vec::new();
    for_each_command(command_bytes, |header, _, payload| {
        commands.push(CommandReference {
            header: *header,
            payload,
        });
    });
    commands
}

struct PayloadReader<'a> {
    payload: &'a [u8],
}

impl PayloadReader<'_> {
    fn bytes_at(&self, offset: usize, size: usize) -> &[u8] {
        &self.payload[offset..offset + size]
    }
    fn u32_at(&self, offset: usize) -> u32 {
        u32::from_ne_bytes(self.bytes_at(offset, 4).try_into().expect("four bytes"))
    }
    fn i32_at(&self, offset: usize) -> i32 {
        i32::from_ne_bytes(self.bytes_at(offset, 4).try_into().expect("four bytes"))
    }
    fn u64_at(&self, offset: usize) -> u64 {
        u64::from_ne_bytes(self.bytes_at(offset, 8).try_into().expect("eight bytes"))
    }
    fn f32_at(&self, offset: usize) -> f32 {
        f32::from_ne_bytes(self.bytes_at(offset, 4).try_into().expect("four bytes"))
    }
    fn bool_at(&self, offset: usize) -> bool {
        self.payload[offset] != 0
    }
    fn float_point_at(&self, offset: usize) -> FloatPoint {
        FloatPoint {
            x: self.f32_at(offset),
            y: self.f32_at(offset + 4),
        }
    }
    fn float_rect_at(&self, offset: usize) -> FloatRect {
        FloatRect::new(
            self.f32_at(offset),
            self.f32_at(offset + 4),
            self.f32_at(offset + 8),
            self.f32_at(offset + 12),
        )
    }
    fn int_rect_at(&self, offset: usize) -> IntRect {
        IntRect::new(
            self.i32_at(offset),
            self.i32_at(offset + 4),
            self.i32_at(offset + 8),
            self.i32_at(offset + 12),
        )
    }
    fn optional_float_rect_at(&self, offset: usize) -> Option<FloatRect> {
        self.bool_at(offset + offset_of!(OptionalFloatRect, has_value))
            .then(|| self.float_rect_at(offset + offset_of!(OptionalFloatRect, value)))
    }
    fn optional_color_at(&self, offset: usize) -> Option<u32> {
        self.bool_at(offset + offset_of!(OptionalColor, has_value))
            .then(|| self.u32_at(offset + offset_of!(OptionalColor, value)))
    }
    fn span_bytes_at(&self, offset: usize) -> &[u8] {
        let span_offset = self.u32_at(offset + offset_of!(DisplayListDataSpan, offset)) as usize;
        let span_size = self.u32_at(offset + offset_of!(DisplayListDataSpan, size)) as usize;
        assert!(span_offset <= self.payload.len());
        assert!(span_size <= self.payload.len() - span_offset);
        self.bytes_at(span_offset, span_size)
    }
}

fn inline_clip_lists_are_equal(a: &CommandReference<'_>, b: &CommandReference<'_>) -> bool {
    let count = a.header.inline_clip_count as usize;
    if count == 0 {
        return true;
    }
    let first = PayloadReader { payload: a.payload };
    let second = PayloadReader { payload: b.payload };
    let a_entries_offset = a.payload.len() - count * INLINE_CLIP_ENTRY_SIZE;
    let b_entries_offset = b.payload.len() - count * INLINE_CLIP_ENTRY_SIZE;
    (0..count).all(|index| {
        let a_entry = a_entries_offset + index * INLINE_CLIP_ENTRY_SIZE;
        let b_entry = b_entries_offset + index * INLINE_CLIP_ENTRY_SIZE;
        let same_entry_field = |offset: usize, size: usize| {
            first.bytes_at(a_entry + offset, size) == second.bytes_at(b_entry + offset, size)
        };
        same_entry_field(offset_of!(DisplayListInlineClip, clip_rect_or_path_device_bounds), 16)
            && same_entry_field(offset_of!(DisplayListInlineClip, corner_radii), 32)
            && same_entry_field(offset_of!(DisplayListInlineClip, path_winding_rule), 4)
            && same_entry_field(offset_of!(DisplayListInlineClip, kind), 1)
            && same_entry_field(offset_of!(DisplayListInlineClip, mode), 1)
            && first.span_bytes_at(a_entry + offset_of!(DisplayListInlineClip, path_data))
                == second.span_bytes_at(b_entry + offset_of!(DisplayListInlineClip, path_data))
    })
}

fn inline_transforms_are_equal(a: &CommandReference<'_>, b: &CommandReference<'_>) -> bool {
    match (
        inline_transform_entry_offset(&a.header, a.payload),
        inline_transform_entry_offset(&b.header, b.payload),
    ) {
        (None, None) => true,
        (Some(a_offset), Some(b_offset)) => {
            let transform_size = std::mem::size_of::<AffineTransform>();
            a.payload[a_offset..a_offset + transform_size] == b.payload[b_offset..b_offset + transform_size]
        }
        _ => false,
    }
}

fn display_list_commands_are_equal(a: &CommandReference<'_>, b: &CommandReference<'_>) -> bool {
    if a.header.command_type != b.header.command_type
        || a.header.has_bounding_rect != b.header.has_bounding_rect
        || a.header.inline_clip_count != b.header.inline_clip_count
        || a.header.has_inline_transform != b.header.has_inline_transform
        || a.header.bounding_rect != b.header.bounding_rect
    {
        return false;
    }

    if !inline_clip_lists_are_equal(a, b) || !inline_transforms_are_equal(a, b) {
        return false;
    }

    let first = PayloadReader { payload: a.payload };
    let second = PayloadReader { payload: b.payload };
    let same_field = |offset: usize, size: usize| first.bytes_at(offset, size) == second.bytes_at(offset, size);

    if a.header.command_type == DisplayListCommandType::DrawScaledDecodedImageFrame {
        return first.float_rect_at(offset_of!(DrawScaledDecodedImageFrame, dst_rect))
            == second.float_rect_at(offset_of!(DrawScaledDecodedImageFrame, dst_rect))
            && first.optional_float_rect_at(offset_of!(DrawScaledDecodedImageFrame, src_rect))
                == second.optional_float_rect_at(offset_of!(DrawScaledDecodedImageFrame, src_rect))
            && first.u64_at(offset_of!(DrawScaledDecodedImageFrame, frame_id))
                == second.u64_at(offset_of!(DrawScaledDecodedImageFrame, frame_id))
            && same_field(offset_of!(DrawScaledDecodedImageFrame, scaling_mode), 4)
            && same_field(
                offset_of!(DrawScaledDecodedImageFrame, compositing_and_blending_operator),
                4,
            )
            && first.optional_color_at(offset_of!(DrawScaledDecodedImageFrame, isolated_backdrop_color))
                == second.optional_color_at(offset_of!(DrawScaledDecodedImageFrame, isolated_backdrop_color))
            && same_field(offset_of!(DrawScaledDecodedImageFrame, apply_force_dark), 1);
    }

    if a.header.command_type == DisplayListCommandType::DrawGlyphRun {
        return same_field(offset_of!(DrawGlyphRun, font_smoothing), 1)
            && first.u64_at(offset_of!(DrawGlyphRun, font_id)) == second.u64_at(offset_of!(DrawGlyphRun, font_id))
            && first.span_bytes_at(offset_of!(DrawGlyphRun, glyphs))
                == second.span_bytes_at(offset_of!(DrawGlyphRun, glyphs))
            && first.int_rect_at(offset_of!(DrawGlyphRun, rect)) == second.int_rect_at(offset_of!(DrawGlyphRun, rect))
            && first.int_rect_at(offset_of!(DrawGlyphRun, glyph_bounding_rect))
                == second.int_rect_at(offset_of!(DrawGlyphRun, glyph_bounding_rect))
            && first.float_point_at(offset_of!(DrawGlyphRun, translation))
                == second.float_point_at(offset_of!(DrawGlyphRun, translation))
            && first.f32_at(offset_of!(DrawGlyphRun, scale)) == second.f32_at(offset_of!(DrawGlyphRun, scale))
            && same_field(offset_of!(DrawGlyphRun, color), 4)
            && same_field(offset_of!(DrawGlyphRun, orientation), 4);
    }

    if a.header.command_type == DisplayListCommandType::PaintTextShadow {
        return same_field(offset_of!(PaintTextShadow, font_smoothing), 1)
            && first.u64_at(offset_of!(PaintTextShadow, font_id))
                == second.u64_at(offset_of!(PaintTextShadow, font_id))
            && first.span_bytes_at(offset_of!(PaintTextShadow, glyphs))
                == second.span_bytes_at(offset_of!(PaintTextShadow, glyphs))
            && first.int_rect_at(offset_of!(PaintTextShadow, shadow_bounding_rect))
                == second.int_rect_at(offset_of!(PaintTextShadow, shadow_bounding_rect))
            && first.int_rect_at(offset_of!(PaintTextShadow, text_rect))
                == second.int_rect_at(offset_of!(PaintTextShadow, text_rect))
            && first.float_point_at(offset_of!(PaintTextShadow, draw_location))
                == second.float_point_at(offset_of!(PaintTextShadow, draw_location))
            && first.f32_at(offset_of!(PaintTextShadow, scale)) == second.f32_at(offset_of!(PaintTextShadow, scale))
            && first.i32_at(offset_of!(PaintTextShadow, blur_radius))
                == second.i32_at(offset_of!(PaintTextShadow, blur_radius))
            && same_field(offset_of!(PaintTextShadow, color), 4);
    }

    a.header.payload_size == b.header.payload_size && a.payload == b.payload
}

fn spatial_data_is_equal(
    a_index: SpatialNodeIndex,
    a: &SpatialData,
    a_scroll_offsets: &[FloatPoint],
    b_index: SpatialNodeIndex,
    b: &SpatialData,
    b_scroll_offsets: &[FloatPoint],
) -> bool {
    match (a, b) {
        // The payload carries no offset key; replay reads the offset stored under each node's
        // own index, so that is what has to match.
        (SpatialData::Scroll(_), SpatialData::Scroll(_)) | (SpatialData::Sticky(_), SpatialData::Sticky(_)) => {
            device_offset_for_index(a_scroll_offsets, a_index) == device_offset_for_index(b_scroll_offsets, b_index)
        }
        (SpatialData::Transform(data), SpatialData::Transform(other)) => data == other,
        (SpatialData::Perspective(data), SpatialData::Perspective(other)) => data == other,
        (SpatialData::BackfaceVisibility(data), SpatialData::BackfaceVisibility(other)) => data == other,
        (SpatialData::AnchorScrollShift(data), SpatialData::AnchorScrollShift(other)) => {
            data.negate == other.negate
                && data.compensate_horizontal_scroll == other.compensate_horizontal_scroll
                && data.compensate_vertical_scroll == other.compensate_vertical_scroll
                && data.masked_offset(a_scroll_offsets) == other.masked_offset(b_scroll_offsets)
        }
        _ => false,
    }
}

fn clip_data_is_equal(a: &ClipNodeData, b: &ClipNodeData) -> bool {
    match (a, b) {
        (ClipNodeData::Rect(data), ClipNodeData::Rect(other)) => data == other,
        (ClipNodeData::Path(data), ClipNodeData::Path(other)) => {
            data.bounding_rect == other.bounding_rect
                && data.fill_rule == other.fill_rule
                && (Rc::ptr_eq(&data.path, &other.path)
                    || data.path.serialize_to_bytes() == other.path.serialize_to_bytes())
        }
        _ => false,
    }
}

fn effect_data_is_equal(a: &EffectNodeData, b: &EffectNodeData) -> bool {
    match (a, b) {
        (EffectNodeData::BackgroundColorAnimation, EffectNodeData::BackgroundColorAnimation) => true,
        (EffectNodeData::Effects(data), EffectNodeData::Effects(other)) => {
            data.opacity == other.opacity
                && data.blend_mode == other.blend_mode
                && data.filter == other.filter
                && data.backdrop_filter == other.backdrop_filter
        }
        // Mask content is per-recording and invisible here, so mask chains always damage.
        (EffectNodeData::Mask(_), EffectNodeData::Mask(_)) => false,
        _ => false,
    }
}

fn spatial_depths(tree: &VisualContextTree) -> Vec<u32> {
    let mut depths: Vec<u32> = vec![0; tree.spatial_nodes.len()];
    for index in tree.spatial_dependency_order() {
        if index == VISUAL_VIEWPORT_NODE_INDEX.0 {
            continue;
        }
        depths[index as usize] = depths[tree.spatial_nodes[index as usize].parent.0 as usize] + 1;
    }
    depths
}

// Walks two root paths in lockstep from their tips, pairing node with node; both must end together.
fn chains_pair_up(
    mut old: u32,
    mut new: u32,
    old_parent: impl Fn(u32) -> u32,
    new_parent: impl Fn(u32) -> u32,
    nodes_match: impl Fn(u32, u32) -> bool,
) -> bool {
    loop {
        if old == u32::MAX || new == u32::MAX {
            return old == new;
        }
        if !nodes_match(old, new) {
            return false;
        }
        old = old_parent(old);
        new = new_parent(new);
    }
}

struct TreeChainComparison<'a> {
    old_tree: &'a VisualContextTree,
    old_scroll_offsets: &'a [FloatPoint],
    old_spatial_depths: Vec<u32>,
    old_culling: TreeCullingScratch,
    new_tree: &'a VisualContextTree,
    new_scroll_offsets: &'a [FloatPoint],
    new_spatial_depths: Vec<u32>,
    new_culling: TreeCullingScratch,
}

impl TreeChainComparison<'_> {
    // Two contexts have the same shape when their spatial, clip and effect chains pair up node
    // by node with matching kinds and spatial depths.
    fn chains_are_compatible(&self, old_context: ContextRef, new_context: ContextRef) -> bool {
        if self.old_spatial_depths[old_context.spatial.0 as usize]
            != self.new_spatial_depths[new_context.spatial.0 as usize]
        {
            return false;
        }
        let mut old_index = old_context.spatial;
        let mut new_index = new_context.spatial;
        loop {
            let old_node = &self.old_tree.spatial_nodes[old_index.0 as usize];
            let new_node = &self.new_tree.spatial_nodes[new_index.0 as usize];
            if std::mem::discriminant(&old_node.data) != std::mem::discriminant(&new_node.data) {
                return false;
            }
            if old_index == VISUAL_VIEWPORT_NODE_INDEX {
                break;
            }
            old_index = old_node.parent;
            new_index = new_node.parent;
        }
        let (old_tree, new_tree) = (self.old_tree, self.new_tree);
        let same_spatial_depth = |old_spatial: SpatialNodeIndex, new_spatial: SpatialNodeIndex| {
            self.old_spatial_depths[old_spatial.0 as usize] == self.new_spatial_depths[new_spatial.0 as usize]
        };
        chains_pair_up(
            old_context.clip.0,
            new_context.clip.0,
            |index| old_tree.clip_nodes[index as usize].parent.0,
            |index| new_tree.clip_nodes[index as usize].parent.0,
            |old, new| {
                let (old, new) = (&old_tree.clip_nodes[old as usize], &new_tree.clip_nodes[new as usize]);
                std::mem::discriminant(&old.data) == std::mem::discriminant(&new.data)
                    && same_spatial_depth(old.spatial, new.spatial)
            },
        ) && chains_pair_up(
            old_context.effect.0,
            new_context.effect.0,
            |index| old_tree.effect_nodes[index as usize].parent.0,
            |index| new_tree.effect_nodes[index as usize].parent.0,
            |old, new| {
                let (old, new) = (
                    &old_tree.effect_nodes[old as usize],
                    &new_tree.effect_nodes[new as usize],
                );
                std::mem::discriminant(&old.data) == std::mem::discriminant(&new.data)
                    && same_spatial_depth(old.spatial, new.spatial)
            },
        )
    }

    // Value equality of compatible chains. An effect's output clip is compared by clip depth: a
    // layer that moved relative to the clips around it is treated as a change.
    fn chains_are_equal(&self, old_context: ContextRef, new_context: ContextRef) -> bool {
        let mut old_index = old_context.spatial;
        let mut new_index = new_context.spatial;
        loop {
            let old_node = &self.old_tree.spatial_nodes[old_index.0 as usize];
            let new_node = &self.new_tree.spatial_nodes[new_index.0 as usize];
            if !spatial_data_is_equal(
                old_index,
                &old_node.data,
                self.old_scroll_offsets,
                new_index,
                &new_node.data,
                self.new_scroll_offsets,
            ) {
                return false;
            }
            if old_index == VISUAL_VIEWPORT_NODE_INDEX {
                break;
            }
            old_index = old_node.parent;
            new_index = new_node.parent;
        }
        let (old_tree, new_tree) = (self.old_tree, self.new_tree);
        chains_pair_up(
            old_context.clip.0,
            new_context.clip.0,
            |index| old_tree.clip_nodes[index as usize].parent.0,
            |index| new_tree.clip_nodes[index as usize].parent.0,
            |old, new| {
                clip_data_is_equal(
                    &old_tree.clip_nodes[old as usize].data,
                    &new_tree.clip_nodes[new as usize].data,
                )
            },
        ) && chains_pair_up(
            old_context.effect.0,
            new_context.effect.0,
            |index| old_tree.effect_nodes[index as usize].parent.0,
            |index| new_tree.effect_nodes[index as usize].parent.0,
            |old, new| {
                let (old, new) = (
                    &old_tree.effect_nodes[old as usize],
                    &new_tree.effect_nodes[new as usize],
                );
                effect_data_is_equal(&old.data, &new.data)
                    && self.old_culling.clip_depth(old.output_clip()) == self.new_culling.clip_depth(new.output_clip())
            },
        )
    }

    fn changing_filter_may_affect_output_bounds(&self, old_context: ContextRef, new_context: ContextRef) -> bool {
        let mut old_effect = old_context.effect;
        let mut new_effect = new_context.effect;
        while !old_effect.is_none() {
            let old_node = &self.old_tree.effect_nodes[old_effect.0 as usize];
            let new_node = &self.new_tree.effect_nodes[new_effect.0 as usize];
            if let (EffectNodeData::Effects(old_effects), EffectNodeData::Effects(new_effects)) =
                (&old_node.data, &new_node.data)
                && old_effects.filter != new_effects.filter
                && old_effects
                    .filter
                    .iter()
                    .chain(new_effects.filter.iter())
                    .any(|filter| crate::painting::filter_bytes::may_affect_output_bounds(filter))
            {
                return true;
            }
            old_effect = old_node.parent;
            new_effect = new_node.parent;
        }
        false
    }
}

fn intersect_like_gfx_rect(rect: FloatRect, other: FloatRect) -> FloatRect {
    let left = rect.x.max(other.x);
    let right = rect.right().min(other.right());
    let top = rect.y.max(other.y);
    let bottom = rect.bottom().min(other.bottom());
    if left > right || top > bottom {
        return FloatRect::default();
    }
    FloatRect::new(left, top, right - left, bottom - top)
}

fn intersect_like_gfx_int_rect(rect: IntRect, other: IntRect) -> IntRect {
    let left = rect.x.max(other.x);
    let right = rect.right().min(other.right());
    let top = rect.y.max(other.y);
    let bottom = rect.bottom().min(other.bottom());
    if left > right || top > bottom {
        return IntRect::default();
    }
    IntRect::new(left, top, right - left, bottom - top)
}

struct DamageAccumulator {
    damage_rect: Option<IntRect>,
    changed_unbounded_command: bool,
}

impl DamageAccumulator {
    fn add_command_damage(
        &mut self,
        command: &CommandReference<'_>,
        visual_context_tree: &VisualContextTree,
        scroll_offsets: &[FloatPoint],
        culling: &TreeCullingScratch,
    ) {
        let context = command.header.context;
        if culling.context_culls_everything(context) {
            return;
        }
        if !command.header.has_bounding_rect {
            if command.header.command_type.is_compositor_metadata() {
                return;
            }
            self.changed_unbounded_command = true;
            return;
        }
        let bounding_rect = command.header.bounding_rect;
        let mut transformed_rect = visual_context_tree.transform_rect_to_viewport(
            context.spatial,
            FloatRect::new(
                bounding_rect.x as f32,
                bounding_rect.y as f32,
                bounding_rect.width as f32,
                bounding_rect.height as f32,
            ),
            scroll_offsets,
            IncludeVisualViewportTransform::Yes,
        );
        // Transform matrices with entries near float max can overflow the projection to non-finite values.
        // NaN survives both intersect() and is_empty(), so treat such rects as unbounded damage instead of
        // feeding them to enclosing_int_rect(), where the float-to-int conversion would be undefined.
        if !transformed_rect.x.is_finite()
            || !transformed_rect.y.is_finite()
            || !transformed_rect.width.is_finite()
            || !transformed_rect.height.is_finite()
        {
            self.changed_unbounded_command = true;
            return;
        }
        // Eye-plane clamping in the projection can produce coordinates beyond integer range, and converting
        // such a float to int is undefined.
        const DAMAGE_COORDINATE_LIMIT: f32 = 16777216.0;
        transformed_rect = intersect_like_gfx_rect(
            transformed_rect,
            FloatRect::new(
                -DAMAGE_COORDINATE_LIMIT,
                -DAMAGE_COORDINATE_LIMIT,
                2.0 * DAMAGE_COORDINATE_LIMIT,
                2.0 * DAMAGE_COORDINATE_LIMIT,
            ),
        );
        if transformed_rect.is_empty() {
            return;
        }
        let command_damage = enclosing_int_rect(transformed_rect);
        self.damage_rect = Some(match self.damage_rect {
            Some(damage_rect) => damage_rect.united(command_damage),
            None => command_damage,
        });
    }
}

// This query is cached until scene geometry or scrolling changes. It deliberately ignores clips when
// bounding rotating content, so animation phases cannot reveal pixels outside the computed bounds.
pub fn animated_content_may_affect_viewport(
    command_bytes: &[u8],
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    rotation_nodes: &[SpatialNodeIndex],
    opacity_nodes: &[EffectNodeIndex],
    viewport_rect: IntRect,
) -> bool {
    let in_rotating_subtree = tree.spatial_nodes_in_subtrees_of(rotation_nodes);
    let mut rotating_nodes = vec![false; tree.spatial_nodes.len()];
    for node in rotation_nodes {
        let Some(flag) = rotating_nodes.get_mut(node.0 as usize) else {
            return true;
        };
        *flag = true;
    }
    let mut may_affect_viewport = false;
    for_each_command(command_bytes, |header, _, _| {
        if may_affect_viewport || header.command_type.is_compositor_metadata() {
            return;
        }
        let spatial_is_animated = in_rotating_subtree[header.context.spatial.0 as usize];
        let mut clip = header.context.clip;
        while !clip.is_none() {
            let node = &tree.clip_nodes[clip.0 as usize];
            if !spatial_is_animated && in_rotating_subtree[node.spatial.0 as usize] {
                may_affect_viewport = true;
                return;
            }
            clip = node.parent;
        }
        let mut effect = header.context.effect;
        let mut opacity_is_animated = false;
        let mut has_filter = false;
        while !effect.is_none() {
            let node = &tree.effect_nodes[effect.0 as usize];
            opacity_is_animated |= opacity_nodes.contains(&effect);
            if !spatial_is_animated && in_rotating_subtree[node.spatial.0 as usize] {
                may_affect_viewport = true;
                return;
            }
            if let EffectNodeData::Effects(effects) = &node.data
                && (effects.filter.is_some() || effects.backdrop_filter.is_some())
            {
                has_filter = true;
            }
            effect = node.parent;
        }
        if !spatial_is_animated && !opacity_is_animated {
            return;
        }
        // Filters can expand the output beyond the command bounds.
        if has_filter {
            may_affect_viewport = true;
            return;
        }
        if !header.has_bounding_rect {
            may_affect_viewport = true;
            return;
        }
        let rect = header.bounding_rect;
        let Some(bounds) = tree.rect_with_rotation_bounds_to_viewport(
            header.context.spatial,
            FloatRect::new(rect.x as f32, rect.y as f32, rect.width as f32, rect.height as f32),
            &rotating_nodes,
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
    may_affect_viewport
}

pub fn compute_display_list_damage(
    old_display_list_commands: &[u8],
    old_visual_context_tree: &VisualContextTree,
    old_scroll_offsets: &[FloatPoint],
    new_display_list_commands: &[u8],
    new_visual_context_tree: &VisualContextTree,
    new_scroll_offsets: &[FloatPoint],
    viewport_rect: IntRect,
) -> Option<IntRect> {
    let old_commands = collect_command_references(old_display_list_commands);
    let new_commands = collect_command_references(new_display_list_commands);
    let mut old_culling = TreeCullingScratch::default();
    old_visual_context_tree.fill_culling_scratch(&mut old_culling);
    let mut new_culling = TreeCullingScratch::default();
    new_visual_context_tree.fill_culling_scratch(&mut new_culling);
    let chains = TreeChainComparison {
        old_tree: old_visual_context_tree,
        old_scroll_offsets,
        old_spatial_depths: spatial_depths(old_visual_context_tree),
        old_culling,
        new_tree: new_visual_context_tree,
        new_scroll_offsets,
        new_spatial_depths: spatial_depths(new_visual_context_tree),
        new_culling,
    };
    let old_frames_with_empty_effective_clip = &chains.old_culling;
    let new_frames_with_empty_effective_clip = &chains.new_culling;
    let commands_are_equal = |old_command: &CommandReference<'_>, new_command: &CommandReference<'_>| {
        display_list_commands_are_equal(old_command, new_command)
            && chains.chains_are_compatible(old_command.header.context, new_command.header.context)
    };
    let common_length = old_commands.len().min(new_commands.len());
    let mut common_prefix_length = 0;
    while common_prefix_length < common_length
        && commands_are_equal(&old_commands[common_prefix_length], &new_commands[common_prefix_length])
    {
        common_prefix_length += 1;
    }

    let mut common_suffix_length = 0;
    while common_suffix_length < common_length - common_prefix_length
        && commands_are_equal(
            &old_commands[old_commands.len() - common_suffix_length - 1],
            &new_commands[new_commands.len() - common_suffix_length - 1],
        )
    {
        common_suffix_length += 1;
    }

    let mut damage = DamageAccumulator {
        damage_rect: None,
        changed_unbounded_command: false,
    };
    let add_old_command_damage = |damage: &mut DamageAccumulator, command: &CommandReference<'_>| {
        damage.add_command_damage(
            command,
            old_visual_context_tree,
            old_scroll_offsets,
            old_frames_with_empty_effective_clip,
        );
    };
    let add_new_command_damage = |damage: &mut DamageAccumulator, command: &CommandReference<'_>| {
        damage.add_command_damage(
            command,
            new_visual_context_tree,
            new_scroll_offsets,
            new_frames_with_empty_effective_clip,
        );
    };
    let add_visual_context_damage =
        |damage: &mut DamageAccumulator, old_command: &CommandReference<'_>, new_command: &CommandReference<'_>| {
            if chains.chains_are_equal(old_command.header.context, new_command.header.context) {
                return;
            }
            if chains.changing_filter_may_affect_output_bounds(old_command.header.context, new_command.header.context) {
                damage.changed_unbounded_command = true;
                return;
            }
            if !old_command.header.has_bounding_rect || !new_command.header.has_bounding_rect {
                if old_command.header.command_type == DisplayListCommandType::CompositorViewportScrollbar {
                    damage.changed_unbounded_command = true;
                }
                return;
            }
            add_old_command_damage(damage, old_command);
            add_new_command_damage(damage, new_command);
        };
    let add_scrollbar_scroll_damage =
        |damage: &mut DamageAccumulator, old_command: &CommandReference<'_>, new_command: &CommandReference<'_>| {
            if old_command.header.command_type != DisplayListCommandType::PaintScrollBar
                || new_command.header.command_type != DisplayListCommandType::PaintScrollBar
            {
                return;
            }

            let old_scroll_node_index = read_command::<PaintScrollBar>(old_command.payload).scroll_node_index;
            let new_scroll_node_index = read_command::<PaintScrollBar>(new_command.payload).scroll_node_index;

            if old_scroll_node_index != new_scroll_node_index {
                return;
            }

            let old_offset = device_offset_for_index(old_scroll_offsets, old_scroll_node_index);
            let new_offset = device_offset_for_index(new_scroll_offsets, new_scroll_node_index);

            if old_offset == new_offset {
                return;
            }

            add_old_command_damage(damage, old_command);
            add_new_command_damage(damage, new_command);
        };

    for i in 0..common_prefix_length {
        add_visual_context_damage(&mut damage, &old_commands[i], &new_commands[i]);
        add_scrollbar_scroll_damage(&mut damage, &old_commands[i], &new_commands[i]);
    }

    let mut old_index = common_prefix_length;
    let mut new_index = common_prefix_length;
    let old_end = old_commands.len() - common_suffix_length;
    let new_end = new_commands.len() - common_suffix_length;
    // Realign short inserted or removed sequences without making damage computation quadratic in the display list size.
    const MAXIMUM_REALIGNMENT_DISTANCE: usize = 8;
    let is_realignment_anchor = |candidate_old_index: usize, candidate_new_index: usize| {
        if !commands_are_equal(&old_commands[candidate_old_index], &new_commands[candidate_new_index]) {
            return false;
        }
        if candidate_old_index + 1 == old_end || candidate_new_index + 1 == new_end {
            return true;
        }
        commands_are_equal(
            &old_commands[candidate_old_index + 1],
            &new_commands[candidate_new_index + 1],
        )
    };
    while old_index < old_end && new_index < new_end {
        if commands_are_equal(&old_commands[old_index], &new_commands[new_index]) {
            add_visual_context_damage(&mut damage, &old_commands[old_index], &new_commands[new_index]);
            add_scrollbar_scroll_damage(&mut damage, &old_commands[old_index], &new_commands[new_index]);
            old_index += 1;
            new_index += 1;
            continue;
        }

        let mut skipped_old_commands = None;
        let mut skipped_new_commands = None;
        for distance in 1..=MAXIMUM_REALIGNMENT_DISTANCE {
            if skipped_old_commands.is_none()
                && old_index + distance < old_end
                && is_realignment_anchor(old_index + distance, new_index)
            {
                skipped_old_commands = Some(distance);
            }
            if skipped_new_commands.is_none()
                && new_index + distance < new_end
                && is_realignment_anchor(old_index, new_index + distance)
            {
                skipped_new_commands = Some(distance);
            }
        }

        if let Some(skipped) = skipped_old_commands
            && skipped_new_commands.is_none_or(|skipped_new| skipped <= skipped_new)
        {
            for _ in 0..skipped {
                add_old_command_damage(&mut damage, &old_commands[old_index]);
                old_index += 1;
            }
            continue;
        }
        if let Some(skipped) = skipped_new_commands {
            for _ in 0..skipped {
                add_new_command_damage(&mut damage, &new_commands[new_index]);
                new_index += 1;
            }
            continue;
        }

        add_old_command_damage(&mut damage, &old_commands[old_index]);
        old_index += 1;
        add_new_command_damage(&mut damage, &new_commands[new_index]);
        new_index += 1;
    }
    while old_index < old_end {
        add_old_command_damage(&mut damage, &old_commands[old_index]);
        old_index += 1;
    }
    while new_index < new_end {
        add_new_command_damage(&mut damage, &new_commands[new_index]);
        new_index += 1;
    }

    for i in 0..common_suffix_length {
        add_visual_context_damage(
            &mut damage,
            &old_commands[old_commands.len() - common_suffix_length + i],
            &new_commands[new_commands.len() - common_suffix_length + i],
        );
        add_scrollbar_scroll_damage(
            &mut damage,
            &old_commands[old_commands.len() - common_suffix_length + i],
            &new_commands[new_commands.len() - common_suffix_length + i],
        );
    }

    if damage.changed_unbounded_command {
        return None;
    }
    let Some(damage_rect) = damage.damage_rect else {
        return Some(IntRect::default());
    };
    Some(intersect_like_gfx_int_rect(
        damage_rect.inflated_edges(1, 1, 1, 1),
        viewport_rect,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeSlotId;
    use crate::painting::display_list::builder::HEADER_SIZE;
    use crate::painting::display_list::commands::{
        BackdropFilterRegion, CanvasId, CompositorMainThreadWheelEventRegion, DisplayListCommand, DisplayListGlyph,
        DrawCanvas, FillRect, FontResourceId, ImageFrameResourceId, InlineClipKind,
    };
    use crate::painting::display_list::ffi_bytes::FfiBytes;
    use crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use crate::painting::visual_context::{
        BackdropFilterData, ClipData, ClipMode, ClipNodeData, ClipNodeIndex, EffectNodeData, EffectNodeIndex,
        EffectsData, MaskData, MaskLayerOrigin, ScrollData, SpatialData, TransformData, TransformDataRole,
    };
    use libgfx_rust::filter::Filter;
    use libgfx_rust::{
        Color, ColorFilterType, CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, MaskKind, Orientation,
        ScalingMode, WindingRule, translation_matrix,
    };

    const RED: Color = Color(0xffff0000);
    const GREEN: Color = Color(0xff00ff00);
    const BLUE: Color = Color(0xff0000ff);
    const YELLOW: Color = Color(0xffffff00);
    const CYAN: Color = Color(0xff00ffff);

    // Appends one record the way the Rust builder writes it: header, payload, then zero padding up to
    // the command alignment, but with the header fields chosen by the test.
    fn append_record<C: DisplayListCommand>(
        bytes: &mut Vec<u8>,
        command: &C,
        inline_data: &[u8],
        bounding_rect: Option<IntRect>,
        context: ContextRef,
        inline_clips: &[DisplayListInlineClip],
    ) {
        let unpadded_payload_size = std::mem::size_of::<C>() + inline_data.len();
        let entries_size = inline_clips.len() * INLINE_CLIP_ENTRY_SIZE;
        let padded_record_size = (HEADER_SIZE + unpadded_payload_size + entries_size).next_multiple_of(16);
        let header = DisplayListCommandHeader {
            command_type: C::COMMAND_TYPE,
            has_bounding_rect: bounding_rect.is_some(),
            inline_clip_count: inline_clips.len() as u8,
            has_inline_transform: false,
            payload_size: (padded_record_size - HEADER_SIZE) as u32,
            context,
            bounding_rect: bounding_rect.unwrap_or_default(),
        };
        let start = bytes.len();
        bytes.resize(start + padded_record_size, 0);
        header.write_ffi_bytes(&mut bytes[start..start + HEADER_SIZE]);
        let payload_start = start + HEADER_SIZE;
        command.write_ffi_bytes(&mut bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
        let inline_start = payload_start + std::mem::size_of::<C>();
        bytes[inline_start..inline_start + inline_data.len()].copy_from_slice(inline_data);
        let entries_start = start + padded_record_size - entries_size;
        for (index, entry) in inline_clips.iter().enumerate() {
            let entry_start = entries_start + index * INLINE_CLIP_ENTRY_SIZE;
            entry.write_ffi_bytes(&mut bytes[entry_start..entry_start + INLINE_CLIP_ENTRY_SIZE]);
        }
    }

    fn command_bytes<C: DisplayListCommand>(
        command: &C,
        bounding_rect: Option<IntRect>,
        context: ContextRef,
    ) -> Vec<u8> {
        let mut bytes = Vec::new();
        append_record(&mut bytes, command, &[], bounding_rect, context, &[]);
        bytes
    }

    fn fill_command_bytes(rect: IntRect, color: Color) -> Vec<u8> {
        command_bytes(
            &FillRect {
                rect,
                color,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            Some(rect),
            ContextRef::default(),
        )
    }

    fn glyph_run_command_bytes(inline_padding: usize, font_smoothing: u8) -> Vec<u8> {
        let glyph = DisplayListGlyph {
            position: FloatPoint { x: 1.0, y: 2.0 },
            glyph_id: 3,
        };
        let command = DrawGlyphRun {
            font_smoothing,
            font_id: FontResourceId(1),
            glyphs: DisplayListDataSpan {
                offset: (std::mem::size_of::<DrawGlyphRun>() + inline_padding) as u32,
                size: std::mem::size_of::<DisplayListGlyph>() as u32,
            },
            rect: IntRect::new(10, 10, 20, 20),
            glyph_bounding_rect: IntRect::new(10, 10, 20, 20),
            translation: FloatPoint { x: 10.0, y: 10.0 },
            scale: 1.0,
            color: RED,
            orientation: Orientation::Horizontal,
        };
        let mut inline_data = vec![0u8; inline_padding];
        inline_data.extend_from_slice(&glyph.to_ffi_bytes());
        let mut bytes = Vec::new();
        append_record(
            &mut bytes,
            &command,
            &inline_data,
            Some(command.glyph_bounding_rect),
            ContextRef::default(),
            &[],
        );
        bytes
    }

    fn rect_inline_clip(rect: IntRect) -> DisplayListInlineClip {
        DisplayListInlineClip {
            clip_rect_or_path_device_bounds: rect.to_float(),
            corner_radii: CornerRadii::default(),
            path_data: DisplayListDataSpan::default(),
            path_winding_rule: WindingRule::Nonzero,
            kind: InlineClipKind::Rect,
            mode: ClipMode::Intersect,
        }
    }

    fn canvas_command_bytes(rect: IntRect, content_generation: u64) -> Vec<u8> {
        let command = DrawCanvas {
            dst_rect: rect,
            canvas_id: CanvasId(1),
            content_generation,
            scaling_mode: ScalingMode::NearestNeighbor,
        };
        command_bytes(&command, Some(rect), ContextRef::default())
    }

    fn transform(matrix: FloatMatrix4x4) -> TransformData {
        TransformData {
            matrix,
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
        }
    }

    fn identity_tree() -> VisualContextTree {
        VisualContextTree::create(transform(FloatMatrix4x4::identity()))
    }

    fn clip(rect: FloatRect) -> ClipNodeData {
        ClipNodeData::Rect(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }

    fn mask(rect: IntRect) -> EffectNodeData {
        EffectNodeData::Mask(MaskData {
            rect,
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
        })
    }

    fn effects(filter: Vec<u8>) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: Some(Rc::new(filter)),
            backdrop_filter: None,
        })
    }

    // A context under a fresh root clip, or under a fresh root effect without an output clip.
    fn clip_context(tree: &mut VisualContextTree, data: ClipNodeData) -> ContextRef {
        let clip = tree.append_clip(data, ClipNodeIndex::NONE, VISUAL_VIEWPORT_NODE_INDEX);
        ContextRef {
            clip,
            effect: EffectNodeIndex::NONE,
            ..ContextRef::default()
        }
    }

    fn effect_context(tree: &mut VisualContextTree, data: EffectNodeData) -> ContextRef {
        let effect = tree.append_effect(
            data,
            EffectNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
            ClipNodeIndex::NONE,
        );
        ContextRef {
            clip: ClipNodeIndex::NONE,
            effect,
            ..ContextRef::default()
        }
    }

    fn blur_filter(radius: f32) -> Vec<u8> {
        Filter::blur(radius, radius, None).serialize()
    }

    fn color_filter(amount: f32) -> Vec<u8> {
        Filter::color(ColorFilterType::Brightness, amount, None).serialize()
    }

    fn drop_shadow_filter(offset: f32, radius: f32) -> Vec<u8> {
        Filter::drop_shadow(offset, offset, radius, RED, None).serialize()
    }

    fn damage_for_filter_change(old_filter: Vec<u8>, new_filter: Vec<u8>) -> Option<IntRect> {
        let mut old_tree = identity_tree();
        let old_context = effect_context(&mut old_tree, effects(old_filter));
        let mut new_tree = identity_tree();
        let new_context = effect_context(&mut new_tree, effects(new_filter));
        let rect = IntRect::new(10, 10, 20, 20);
        let fill = FillRect {
            rect,
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_display_list = command_bytes(&fill, Some(rect), context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context));
        let new_display_list = command_bytes(&fill, Some(rect), context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context));
        damage(&old_display_list, &old_tree, &new_display_list, &new_tree)
    }

    fn damage(
        old_bytes: &[u8],
        old_tree: &VisualContextTree,
        new_bytes: &[u8],
        new_tree: &VisualContextTree,
    ) -> Option<IntRect> {
        compute_display_list_damage(
            old_bytes,
            old_tree,
            &[],
            new_bytes,
            new_tree,
            &[],
            IntRect::new(0, 0, 100, 100),
        )
    }

    fn context_in(spatial: SpatialNodeIndex, context: ContextRef) -> ContextRef {
        ContextRef { spatial, ..context }
    }

    #[test]
    fn a_child_stored_below_its_parent_damages_like_the_in_order_tree() {
        let mut in_order = identity_tree();
        let in_order_scroll = in_order.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let in_order_transform = in_order.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            in_order_scroll,
        );

        let mut permuted = identity_tree();
        let permuted_transform = permuted.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let permuted_scroll = permuted.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
                registry_parent_node: VISUAL_VIEWPORT_NODE_INDEX,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        permuted.spatial_nodes[permuted_transform.0 as usize].parent = permuted_scroll;

        let rect = IntRect::new(10, 10, 20, 20);
        let old_commands = command_bytes(
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            Some(rect),
            context_in(in_order_transform, ContextRef::default()),
        );
        let new_commands = command_bytes(
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            Some(rect),
            context_in(permuted_transform, ContextRef::default()),
        );
        assert_eq!(
            damage(&old_commands, &in_order, &new_commands, &permuted),
            Some(IntRect::default())
        );
    }

    #[test]
    fn identical_display_lists_have_no_damage() {
        let tree = identity_tree();
        let display_list = fill_command_bytes(IntRect::new(10, 10, 20, 20), RED);
        assert_eq!(
            damage(&display_list, &tree, &display_list, &tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn changed_scroll_offset_damages_scrollbar() {
        let tree = identity_tree();

        let scrollbar = PaintScrollBar {
            scroll_node_index: SpatialNodeIndex(1),
            gutter_rect: IntRect::default(),
            thumb_rect: IntRect::new(10, 12, 20, 8),
            track_rect: IntRect::new(10, 10, 80, 12),
            scroll_size: 0.75,
            thumb_color: RED,
            track_color: Color::TRANSPARENT,
            vertical: false,
        };

        let display_list = command_bytes(&scrollbar, scrollbar.bounding_rect(), ContextRef::default());

        assert_eq!(
            compute_display_list_damage(
                &display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint::default()],
                &display_list,
                &tree,
                &[FloatPoint::default(), FloatPoint { x: -40.0, y: 0.0 }],
                IntRect::new(0, 0, 100, 100),
            ),
            Some(IntRect::new(9, 9, 82, 14))
        );
    }

    #[test]
    fn inactive_optional_storage_does_not_damage_scaled_images() {
        let tree = identity_tree();
        let command = DrawScaledDecodedImageFrame {
            dst_rect: FloatRect::new(0.0, 0.0, 100.0, 100.0),
            src_rect: OptionalFloatRect::none(),
            frame_id: ImageFrameResourceId(1),
            scaling_mode: ScalingMode::Bilinear,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            isolated_backdrop_color: OptionalColor::none(),
            apply_force_dark: false,
        };
        let old_display_list = command_bytes(&command, Some(IntRect::new(0, 0, 100, 100)), ContextRef::default());
        let mut new_display_list = old_display_list.clone();
        // Optional<T> leaves its inactive T storage unspecified. Alter that storage
        // without changing the empty src_rect value represented by the command.
        new_display_list[HEADER_SIZE + std::mem::size_of::<FloatRect>()] ^= 0xff;
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn force_dark_reach_change_damages_scaled_images() {
        // A force-dark flip re-records the same image draw with only apply_force_dark moved; the raster output
        // changes, so the comparison has to see the field.
        let tree = identity_tree();
        let mut command = DrawScaledDecodedImageFrame {
            dst_rect: FloatRect::new(0.0, 0.0, 100.0, 100.0),
            src_rect: OptionalFloatRect::none(),
            frame_id: ImageFrameResourceId(1),
            scaling_mode: ScalingMode::Bilinear,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            isolated_backdrop_color: OptionalColor::none(),
            apply_force_dark: false,
        };
        let old_display_list = command_bytes(&command, Some(IntRect::new(0, 0, 100, 100)), ContextRef::default());
        command.apply_force_dark = true;
        let new_display_list = command_bytes(&command, Some(IntRect::new(0, 0, 100, 100)), ContextRef::default());
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(0, 0, 100, 100))
        );
    }

    #[test]
    fn inline_payload_alignment_does_not_damage_glyph_runs() {
        let tree = identity_tree();
        let old_display_list = glyph_run_command_bytes(0, crate::css::css_enums::font_smoothing::AUTO);
        let new_display_list = glyph_run_command_bytes(4, crate::css::css_enums::font_smoothing::AUTO);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn changing_font_smoothing_damages_glyph_runs() {
        let tree = identity_tree();
        let old_display_list = glyph_run_command_bytes(0, crate::css::css_enums::font_smoothing::AUTO);
        let new_display_list = glyph_run_command_bytes(0, crate::css::css_enums::font_smoothing::ANTIALIASED);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn damage_contains_old_and_new_command_bounds() {
        let tree = identity_tree();
        let old_display_list = fill_command_bytes(IntRect::new(10, 10, 20, 20), RED);
        let new_display_list = fill_command_bytes(IntRect::new(40, 40, 20, 20), BLUE);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(9, 9, 52, 52))
        );
    }

    #[test]
    fn adding_an_inline_clip_to_a_draw_damages_the_draw() {
        let tree = identity_tree();
        let rect = IntRect::new(10, 10, 20, 20);
        let old_display_list = fill_command_bytes(rect, RED);
        let mut new_display_list = Vec::new();
        append_record(
            &mut new_display_list,
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            &[],
            Some(rect),
            ContextRef::default(),
            &[rect_inline_clip(rect)],
        );
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn a_changed_inline_clip_rect_damages_a_field_compared_command() {
        let tree = identity_tree();
        let record_with_clip = |clip_rect: IntRect| {
            let mut bytes = Vec::new();
            append_record(
                &mut bytes,
                &DrawGlyphRun {
                    font_smoothing: crate::css::css_enums::font_smoothing::AUTO,
                    font_id: FontResourceId(1),
                    glyphs: DisplayListDataSpan::default(),
                    rect: IntRect::new(10, 10, 20, 20),
                    glyph_bounding_rect: IntRect::new(10, 10, 20, 20),
                    translation: FloatPoint { x: 10.0, y: 10.0 },
                    scale: 1.0,
                    color: RED,
                    orientation: Orientation::Horizontal,
                },
                &[],
                Some(IntRect::new(10, 10, 20, 20)),
                ContextRef::default(),
                &[rect_inline_clip(clip_rect)],
            );
            bytes
        };
        let old_display_list = record_with_clip(IntRect::new(10, 10, 20, 20));
        let unchanged_display_list = record_with_clip(IntRect::new(10, 10, 20, 20));
        let changed_display_list = record_with_clip(IntRect::new(10, 10, 12, 20));
        assert_eq!(
            damage(&old_display_list, &tree, &unchanged_display_list, &tree),
            Some(IntRect::default())
        );
        assert_eq!(
            damage(&old_display_list, &tree, &changed_display_list, &tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn changed_unbounded_commands_require_full_repaint() {
        let tree = identity_tree();
        let old_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(0, 0, 10, 10),
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            None,
            ContextRef::default(),
        );
        let new_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(0, 0, 10, 10),
                color: BLUE,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            None,
            ContextRef::default(),
        );
        assert_eq!(damage(&old_display_list, &tree, &new_display_list, &tree), None);
    }

    #[test]
    fn changed_compositor_metadata_has_no_raster_damage() {
        let tree = identity_tree();
        let old_display_list = command_bytes(
            &CompositorMainThreadWheelEventRegion {
                rect: FloatRect::new(0.0, 0.0, 10.0, 10.0),
            },
            None,
            ContextRef::default(),
        );
        let new_display_list = command_bytes(
            &CompositorMainThreadWheelEventRegion {
                rect: FloatRect::new(20.0, 20.0, 10.0, 10.0),
            },
            None,
            ContextRef::default(),
        );
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn changed_visual_context_damages_affected_commands() {
        let old_tree = identity_tree();
        let new_tree = VisualContextTree::create(transform(translation_matrix(10.0, 0.0, 0.0)));
        let display_list = fill_command_bytes(IntRect::new(10, 10, 20, 20), RED);
        assert_eq!(
            damage(&display_list, &old_tree, &display_list, &new_tree),
            Some(IntRect::new(9, 9, 32, 22))
        );
    }

    #[test]
    fn changed_extent_affecting_filter_has_unbounded_damage() {
        assert_eq!(damage_for_filter_change(blur_filter(1.0), blur_filter(10.0)), None);
        assert_eq!(
            damage_for_filter_change(drop_shadow_filter(1.0, 1.0), drop_shadow_filter(10.0, 10.0)),
            None
        );
    }

    #[test]
    fn changed_color_filter_has_bounded_damage() {
        assert_eq!(
            damage_for_filter_change(color_filter(0.5), color_filter(1.0)),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    fn backdrop_effects(filter: Vec<u8>, region: IntRect) -> EffectNodeData {
        EffectNodeData::Effects(EffectsData {
            opacity: 1.0,
            blend_mode: CompositingAndBlendingOperator::Normal,
            filter: None,
            backdrop_filter: Some(BackdropFilterData {
                filter: Rc::new(filter),
                region,
                corner_radii: CornerRadii::default(),
            }),
        })
    }

    // A backdrop filter's output is limited to its region, which the region command records, so a
    // changed backdrop filter damages that region even when it widens the filter's extent.
    #[test]
    fn changed_backdrop_filter_damages_its_region() {
        let region = IntRect::new(10, 10, 20, 20);
        let mut old_tree = identity_tree();
        let old_context = effect_context(&mut old_tree, backdrop_effects(blur_filter(1.0), region));
        let mut new_tree = identity_tree();
        let new_context = effect_context(&mut new_tree, backdrop_effects(blur_filter(10.0), region));
        let mut unchanged_tree = identity_tree();
        let unchanged_context = effect_context(&mut unchanged_tree, backdrop_effects(blur_filter(1.0), region));
        let marker = BackdropFilterRegion { rect: region };
        let old_display_list = command_bytes(
            &marker,
            Some(region),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context),
        );
        let new_display_list = command_bytes(
            &marker,
            Some(region),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context),
        );
        let unchanged_display_list = command_bytes(
            &marker,
            Some(region),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, unchanged_context),
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &unchanged_display_list, &unchanged_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn mask_visual_context_damages_affected_commands() {
        let mut old_tree = identity_tree();
        let old_mask_context = effect_context(&mut old_tree, mask(IntRect::new(0, 0, 100, 100)));
        let mut new_tree = identity_tree();
        effect_context(&mut new_tree, mask(IntRect::new(0, 0, 100, 100)));
        let display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(10, 10, 20, 20),
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            Some(IntRect::new(10, 10, 20, 20)),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_mask_context),
        );
        assert_eq!(
            damage(&display_list, &old_tree, &display_list, &new_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn commands_under_an_empty_effective_clip_do_not_damage() {
        let mut old_tree = identity_tree();
        let old_context = clip_context(&mut old_tree, clip(FloatRect::default()));
        let mut new_tree = identity_tree();
        let new_context = clip_context(&mut new_tree, clip(FloatRect::default()));
        let old_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(10, 10, 20, 20),
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            Some(IntRect::new(10, 10, 20, 20)),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context),
        );
        let new_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(30, 30, 20, 20),
                color: BLUE,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                background_color_animation_effect: EffectNodeIndex::NONE,
            },
            Some(IntRect::new(30, 30, 20, 20)),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context),
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn a_clip_growing_from_empty_damages_the_commands_it_reveals() {
        let mut old_tree = identity_tree();
        let old_context = clip_context(&mut old_tree, clip(FloatRect::default()));
        let mut new_tree = identity_tree();
        let new_context = clip_context(&mut new_tree, clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)));
        let fill = FillRect {
            rect: IntRect::new(10, 10, 20, 20),
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_context),
        );
        let new_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, new_context),
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn unrelated_inserted_visual_context_does_not_damage_commands() {
        let mut old_tree = identity_tree();
        let old_command_spatial = old_tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut new_tree = identity_tree();
        effect_context(
            &mut new_tree,
            EffectNodeData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
                backdrop_filter: None,
            }),
        );
        let new_command_spatial = new_tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let fill = FillRect {
            rect: IntRect::new(10, 10, 20, 20),
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let old_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(old_command_spatial, ContextRef::default()),
        );
        let new_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(new_command_spatial, ContextRef::default()),
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn changed_canvas_content_generation_damages_canvas_rect() {
        let tree = identity_tree();
        let old_display_list = canvas_command_bytes(IntRect::new(10, 10, 20, 20), 1);
        let new_display_list = canvas_command_bytes(IntRect::new(10, 10, 20, 20), 2);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn unchanged_canvas_content_generation_does_not_damage_canvas() {
        let tree = identity_tree();
        let canvas = canvas_command_bytes(IntRect::new(10, 10, 20, 20), 1);
        let old_fill = fill_command_bytes(IntRect::new(50, 50, 10, 10), RED);
        let new_fill = fill_command_bytes(IntRect::new(50, 50, 10, 10), BLUE);
        let old_display_list = [canvas.clone(), old_fill].concat();
        let new_display_list = [canvas, new_fill].concat();
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(49, 49, 12, 12))
        );
    }

    #[test]
    fn inserted_and_removed_commands_do_not_damage_shifted_commands() {
        let tree = identity_tree();
        let first = fill_command_bytes(IntRect::new(10, 10, 10, 10), RED);
        let second = fill_command_bytes(IntRect::new(30, 10, 10, 10), GREEN);
        let third = fill_command_bytes(IntRect::new(50, 10, 10, 10), BLUE);
        let removed = fill_command_bytes(IntRect::new(70, 10, 10, 10), YELLOW);
        let inserted = fill_command_bytes(IntRect::new(20, 40, 10, 10), CYAN);
        let old_display_list = [first.clone(), second.clone(), third.clone(), removed].concat();
        let new_display_list = [first, inserted, second, third].concat();
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::new(19, 9, 62, 42))
        );
    }
}
