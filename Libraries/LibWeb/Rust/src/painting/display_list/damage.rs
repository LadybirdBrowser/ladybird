/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::display_list::builder::{HEADER_SIZE, read_header};
use crate::painting::display_list::commands::{
    ContextRef, DisplayListCommandHeader, DisplayListCommandType, DisplayListDataSpan, DisplayListInlineClip,
    DrawGlyphRun, DrawScaledDecodedImageFrame, INLINE_CLIP_ENTRY_SIZE, OptionalColor, OptionalFloatRect,
    PaintTextShadow, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
};
use crate::painting::visual_context::{
    FrameData, IncludeVisualViewportTransform, SpatialData, VisualContextTree, device_offset_for_index,
};
use libgfx_rust::{FloatPoint, FloatRect, IntRect, enclosing_int_rect};
use std::mem::offset_of;
use std::rc::Rc;

struct CommandReference<'a> {
    header: DisplayListCommandHeader,
    payload: &'a [u8],
}

fn collect_command_references(command_bytes: &[u8]) -> Vec<CommandReference<'_>> {
    let mut commands = Vec::new();
    let mut offset = 0;
    while offset < command_bytes.len() {
        let header = read_header(&command_bytes[offset..]);
        offset += HEADER_SIZE;
        let payload = &command_bytes[offset..offset + header.payload_size as usize];
        offset += header.payload_size as usize;
        commands.push(CommandReference { header, payload });
    }
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

fn display_list_commands_are_equal(a: &CommandReference<'_>, b: &CommandReference<'_>) -> bool {
    if a.header.command_type != b.header.command_type
        || a.header.has_bounding_rect != b.header.has_bounding_rect
        || a.header.inline_clip_count != b.header.inline_clip_count
        || a.header.bounding_rect != b.header.bounding_rect
    {
        return false;
    }

    if !inline_clip_lists_are_equal(a, b) {
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
                == second.optional_color_at(offset_of!(DrawScaledDecodedImageFrame, isolated_backdrop_color));
    }

    if a.header.command_type == DisplayListCommandType::DrawGlyphRun {
        return first.u64_at(offset_of!(DrawGlyphRun, font_id)) == second.u64_at(offset_of!(DrawGlyphRun, font_id))
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
        return first.u64_at(offset_of!(PaintTextShadow, font_id))
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

fn frame_data_is_equal(a: &FrameData, b: &FrameData) -> bool {
    match (a, b) {
        (FrameData::Clip(data), FrameData::Clip(other)) => data == other,
        (FrameData::ClipPath(data), FrameData::ClipPath(other)) => {
            data.bounding_rect == other.bounding_rect
                && data.fill_rule == other.fill_rule
                && (Rc::ptr_eq(&data.path, &other.path)
                    || data.path.serialize_to_bytes() == other.path.serialize_to_bytes())
        }
        (FrameData::Effects(data), FrameData::Effects(other)) => {
            data.opacity == other.opacity && data.blend_mode == other.blend_mode && data.filter == other.filter
        }
        // Mask content is per-recording and invisible here, so mask chains always damage.
        (FrameData::Mask(_), FrameData::Mask(_)) => false,
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

struct TreeChainComparison<'a> {
    old_tree: &'a VisualContextTree,
    old_scroll_offsets: &'a [FloatPoint],
    old_spatial_depths: Vec<u32>,
    new_tree: &'a VisualContextTree,
    new_scroll_offsets: &'a [FloatPoint],
    new_spatial_depths: Vec<u32>,
}

impl TreeChainComparison<'_> {
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
        let mut old_frame = old_context.frame;
        let mut new_frame = new_context.frame;
        loop {
            if old_frame.is_none() || new_frame.is_none() {
                return old_frame == new_frame;
            }
            let old_node = &self.old_tree.frame_nodes[old_frame.0 as usize];
            let new_node = &self.new_tree.frame_nodes[new_frame.0 as usize];
            if std::mem::discriminant(&old_node.data) != std::mem::discriminant(&new_node.data) {
                return false;
            }
            if self.old_spatial_depths[old_node.spatial.0 as usize]
                != self.new_spatial_depths[new_node.spatial.0 as usize]
            {
                return false;
            }
            old_frame = old_node.parent;
            new_frame = new_node.parent;
        }
    }

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
        let mut old_frame = old_context.frame;
        let mut new_frame = new_context.frame;
        while !old_frame.is_none() {
            let old_node = &self.old_tree.frame_nodes[old_frame.0 as usize];
            let new_node = &self.new_tree.frame_nodes[new_frame.0 as usize];
            if !frame_data_is_equal(&old_node.data, &new_node.data) {
                return false;
            }
            old_frame = old_node.parent;
            new_frame = new_node.parent;
        }
        true
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
        frames_with_empty_effective_clip: &[bool],
    ) {
        let context = command.header.context;
        if !context.frame.is_none() && frames_with_empty_effective_clip[context.frame.0 as usize] {
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
    let chains = TreeChainComparison {
        old_tree: old_visual_context_tree,
        old_scroll_offsets,
        old_spatial_depths: spatial_depths(old_visual_context_tree),
        new_tree: new_visual_context_tree,
        new_scroll_offsets,
        new_spatial_depths: spatial_depths(new_visual_context_tree),
    };
    let old_frames_with_empty_effective_clip = old_visual_context_tree.frames_with_empty_effective_clip();
    let new_frames_with_empty_effective_clip = new_visual_context_tree.frames_with_empty_effective_clip();
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
            &old_frames_with_empty_effective_clip,
        );
    };
    let add_new_command_damage = |damage: &mut DamageAccumulator, command: &CommandReference<'_>| {
        damage.add_command_damage(
            command,
            new_visual_context_tree,
            new_scroll_offsets,
            &new_frames_with_empty_effective_clip,
        );
    };
    let add_visual_context_damage =
        |damage: &mut DamageAccumulator, old_command: &CommandReference<'_>, new_command: &CommandReference<'_>| {
            if chains.chains_are_equal(old_command.header.context, new_command.header.context) {
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

    for i in 0..common_prefix_length {
        add_visual_context_damage(&mut damage, &old_commands[i], &new_commands[i]);
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
    use crate::painting::display_list::commands::{
        CanvasId, CompositorMainThreadWheelEventRegion, DisplayListCommand, DisplayListGlyph, DrawCanvas, FillRect,
        FontResourceId, FrameNodeIndex, ImageFrameResourceId, InlineClipKind,
    };
    use crate::painting::display_list::ffi_bytes::FfiBytes;
    use crate::painting::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use crate::painting::visual_context::{
        ClipData, ClipMode, EffectsData, FrameData, MaskData, MaskLayerOrigin, ScrollData, SpatialData, TransformData,
        TransformDataRole,
    };
    use libgfx_rust::{
        Color, CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, MaskKind, Orientation, ScalingMode,
        WindingRule, translation_matrix,
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
            },
            Some(rect),
            ContextRef::default(),
        )
    }

    fn glyph_run_command_bytes(inline_padding: usize) -> Vec<u8> {
        let glyph = DisplayListGlyph {
            position: FloatPoint { x: 1.0, y: 2.0 },
            glyph_id: 3,
        };
        let command = DrawGlyphRun {
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

    fn clip(rect: FloatRect) -> FrameData {
        FrameData::Clip(ClipData {
            rect,
            corner_radii: CornerRadii::default(),
            mode: ClipMode::Intersect,
        })
    }

    fn mask(rect: IntRect) -> FrameData {
        FrameData::Mask(MaskData {
            rect,
            kind: MaskKind::Alpha,
            origin: MaskLayerOrigin::CssMaskLayers,
        })
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

    fn context_in(spatial: SpatialNodeIndex, frame: FrameNodeIndex) -> ContextRef {
        ContextRef { spatial, frame }
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
            },
            Some(rect),
            context_in(in_order_transform, FrameNodeIndex::NONE),
        );
        let new_commands = command_bytes(
            &FillRect {
                rect,
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            },
            Some(rect),
            context_in(permuted_transform, FrameNodeIndex::NONE),
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
    fn inactive_optional_storage_does_not_damage_scaled_images() {
        let tree = identity_tree();
        let command = DrawScaledDecodedImageFrame {
            dst_rect: FloatRect::new(0.0, 0.0, 100.0, 100.0),
            src_rect: OptionalFloatRect::none(),
            frame_id: ImageFrameResourceId(1),
            scaling_mode: ScalingMode::Bilinear,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            isolated_backdrop_color: OptionalColor::none(),
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
    fn inline_payload_alignment_does_not_damage_glyph_runs() {
        let tree = identity_tree();
        let old_display_list = glyph_run_command_bytes(0);
        let new_display_list = glyph_run_command_bytes(4);
        assert_eq!(
            damage(&old_display_list, &tree, &new_display_list, &tree),
            Some(IntRect::default())
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
            },
            None,
            ContextRef::default(),
        );
        let new_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(0, 0, 10, 10),
                color: BLUE,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
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
    fn mask_visual_context_damages_affected_commands() {
        let mut old_tree = identity_tree();
        let old_mask_frame = old_tree.append_frame(
            mask(IntRect::new(0, 0, 100, 100)),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut new_tree = identity_tree();
        new_tree.append_frame(
            mask(IntRect::new(0, 0, 100, 100)),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(10, 10, 20, 20),
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            },
            Some(IntRect::new(10, 10, 20, 20)),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_mask_frame),
        );
        assert_eq!(
            damage(&display_list, &old_tree, &display_list, &new_tree),
            Some(IntRect::new(9, 9, 22, 22))
        );
    }

    #[test]
    fn commands_under_an_empty_effective_clip_do_not_damage() {
        let mut old_tree = identity_tree();
        let old_frame = old_tree.append_frame(
            clip(FloatRect::default()),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut new_tree = identity_tree();
        let new_frame = new_tree.append_frame(
            clip(FloatRect::default()),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let old_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(10, 10, 20, 20),
                color: RED,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            },
            Some(IntRect::new(10, 10, 20, 20)),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_frame),
        );
        let new_display_list = command_bytes(
            &FillRect {
                rect: IntRect::new(30, 30, 20, 20),
                color: BLUE,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            },
            Some(IntRect::new(30, 30, 20, 20)),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, new_frame),
        );
        assert_eq!(
            damage(&old_display_list, &old_tree, &new_display_list, &new_tree),
            Some(IntRect::default())
        );
    }

    #[test]
    fn a_clip_growing_from_empty_damages_the_commands_it_reveals() {
        let mut old_tree = identity_tree();
        let old_frame = old_tree.append_frame(
            clip(FloatRect::default()),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let mut new_tree = identity_tree();
        let new_frame = new_tree.append_frame(
            clip(FloatRect::new(0.0, 0.0, 100.0, 100.0)),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let fill = FillRect {
            rect: IntRect::new(10, 10, 20, 20),
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
        };
        let old_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, old_frame),
        );
        let new_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(VISUAL_VIEWPORT_NODE_INDEX, new_frame),
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
        new_tree.append_frame(
            FrameData::Effects(EffectsData {
                opacity: 0.5,
                blend_mode: CompositingAndBlendingOperator::Normal,
                filter: None,
            }),
            FrameNodeIndex::NONE,
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let new_command_spatial = new_tree.append_spatial(
            SpatialData::Transform(transform(FloatMatrix4x4::identity())),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        let fill = FillRect {
            rect: IntRect::new(10, 10, 20, 20),
            color: RED,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
        };
        let old_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(old_command_spatial, FrameNodeIndex::NONE),
        );
        let new_display_list = command_bytes(
            &fill,
            Some(fill.rect),
            context_in(new_command_spatial, FrameNodeIndex::NONE),
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
