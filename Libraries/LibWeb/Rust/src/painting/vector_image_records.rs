/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The records an SVG image's document was recorded as, and how a tape that names them takes them into the payloads
//! of the commands that paint them, which is the form the compositor replays a tape in.

use crate::painting::display_list::builder::{
    HEADER_SIZE, RecordedDisplayList, for_each_command, read_command, read_header,
};
use crate::painting::display_list::commands::{
    DeclareMaskContent, DisplayListCommandHeader, DisplayListCommandRun, DisplayListCommandType, DisplayListDataSpan,
    DisplayListGradientColorStops, DisplayListGradientPaintStyle, DisplayListInlineClip, DisplayListPaintStyle,
    DrawIsolatedGroup, DrawRepeatedTile, EffectNodeIndex, FillPath, FillRect, FillRectWithRoundedCorners,
    INLINE_CLIP_ENTRY_SIZE, ImageFrameResourceId, PaintVectorImage, StrokePath,
};
use crate::painting::display_list::ffi_bytes::FfiBytes;
use crate::painting::display_list::ids::allocate_display_list_id;
use crate::painting::display_list::nested_records::for_each_nested_record_span;
use crate::painting::display_list::recorder::DisplayListRecorder;
use crate::painting::display_list::summary::vector_image_records_require_direct_replay;
use crate::painting::display_list_resources::DisplayListResources;
use crate::painting::force_dark::ForceDarkRole;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{CompositingAndBlendingOperator, FloatRect, IntSize, ScalingMode};
use smallvec::SmallVec;
use std::collections::HashMap;
use std::mem::{offset_of, size_of};
use std::sync::Arc;

/// The records an SVG image's document was recorded as, which name no visual context, and what they draw. They hold
/// the records of the SVG images that the image itself draws.
pub(crate) struct VectorImageRecords {
    id: u64,
    pub(crate) records: Vec<u8>,
    pub(crate) requires_direct_replay: bool,
    /// The fonts and image frames the records draw.
    pub(crate) resources: DisplayListResources,
}

impl VectorImageRecords {
    /// The records `display_list` holds, which its document recorded under no visual context, under a new id, drawing
    /// `resources`.
    pub(crate) fn recorded(display_list: &RecordedDisplayList, resources: &DisplayListResources) -> Arc<Self> {
        debug_assert!(
            display_list.command_runs.len() <= 1,
            "the records of an SVG image are recorded under one visual context"
        );
        let mut records = display_list_holding_vector_image_records(display_list, &resources.vector_images).bytes;
        forget_visual_context_nodes_named_by(&mut records);
        let mut drawn = DisplayListResources::default();
        drawn.include_fonts_and_image_frames(&resources.with_vector_image_resources());
        Arc::new(Self {
            id: allocate_display_list_id(),
            requires_direct_replay: vector_image_records_require_direct_replay(&records),
            records,
            resources: drawn,
        })
    }

    /// Records that draw `frame`, the raster of an SVG image of `size`, under a new id.
    pub(crate) fn drawing_image_frame(frame: ImageFrameHandle, size: IntSize) -> Arc<Self> {
        let mut recorder = DisplayListRecorder::new(None);
        recorder.draw_scaled_decoded_image_frame(
            FloatRect::new(0.0, 0.0, size.width as f32, size.height as f32),
            None,
            ImageFrameResourceId(frame.id()),
            ScalingMode::Bilinear,
            CompositingAndBlendingOperator::Normal,
            None,
            ForceDarkRole::None,
        );
        let mut resources = DisplayListResources::default();
        resources.image_frames.insert(frame.id(), frame);
        Arc::new(Self {
            id: allocate_display_list_id(),
            records: recorder.into_builder().finish().bytes,
            // A raster of the records would hold the pixels the frame holds already.
            requires_direct_replay: true,
            resources,
        })
    }

    /// No records, which stand in for an SVG image that has nothing to show.
    pub(crate) fn empty() -> Arc<Self> {
        Arc::new(Self {
            id: allocate_display_list_id(),
            records: Vec::new(),
            requires_direct_replay: false,
            resources: DisplayListResources::default(),
        })
    }

    pub(crate) fn id(&self) -> u64 {
        self.id
    }
}

// The records replay against the visual context tree of whatever tape takes them in, of which they name no node.
fn forget_visual_context_nodes_named_by(records: &mut [u8]) {
    let mut effect_field_offsets = Vec::new();
    crate::painting::display_list::nested_records::for_each_command_including_nested(
        records,
        &mut |command_type, payload_offset, _| match command_type {
            DisplayListCommandType::FillRect => {
                effect_field_offsets.push(payload_offset + offset_of!(FillRect, background_color_animation_effect));
            }
            DisplayListCommandType::FillRectWithRoundedCorners => effect_field_offsets
                .push(payload_offset + offset_of!(FillRectWithRoundedCorners, background_color_animation_effect)),
            _ => {}
        },
    );
    for offset in effect_field_offsets {
        EffectNodeIndex::NONE.write_ffi_bytes(&mut records[offset..offset + size_of::<EffectNodeIndex>()]);
    }
}

// Where a payload grows as the records nested in it take in the records of SVG images: at `position` in the payload as
// it was, by `growth` bytes.
#[derive(Clone, Copy)]
struct PayloadGrowth {
    position: u32,
    growth: u32,
}

fn offset_after_growth(offset: u32, growths: &[PayloadGrowth]) -> u32 {
    offset
        + growths
            .iter()
            .filter(|growth| growth.position <= offset)
            .map(|growth| growth.growth)
            .sum::<u32>()
}

// The offsets, in the payload of a command that can hold nested records, of every span its command struct holds.
fn span_field_offsets_of(command_type: DisplayListCommandType) -> SmallVec<[usize; 6]> {
    fn paint_style_span_field_offsets(paint_style: usize) -> [usize; 3] {
        let color_stops = paint_style
            + offset_of!(DisplayListPaintStyle, gradient)
            + offset_of!(DisplayListGradientPaintStyle, color_stops);
        [
            color_stops + offset_of!(DisplayListGradientColorStops, colors),
            color_stops + offset_of!(DisplayListGradientColorStops, positions),
            paint_style + offset_of!(DisplayListPaintStyle, pattern_tile),
        ]
    }
    let mut offsets = SmallVec::new();
    match command_type {
        DisplayListCommandType::DrawIsolatedGroup => offsets.extend([
            offset_of!(DrawIsolatedGroup, content),
            offset_of!(DrawIsolatedGroup, mask),
            offset_of!(DrawIsolatedGroup, filter),
        ]),
        DisplayListCommandType::DrawRepeatedTile => offsets.push(offset_of!(DrawRepeatedTile, tile)),
        DisplayListCommandType::DeclareMaskContent => offsets.push(offset_of!(DeclareMaskContent, content)),
        DisplayListCommandType::PaintVectorImage => offsets.push(offset_of!(PaintVectorImage, records)),
        DisplayListCommandType::FillPath => {
            offsets.push(offset_of!(FillPath, path_data));
            offsets.extend(paint_style_span_field_offsets(offset_of!(FillPath, paint_style)));
        }
        DisplayListCommandType::StrokePath => {
            offsets.extend([offset_of!(StrokePath, dash_array), offset_of!(StrokePath, path_data)]);
            offsets.extend(paint_style_span_field_offsets(offset_of!(StrokePath, paint_style)));
        }
        _ => {}
    }
    offsets
}

fn read_span(bytes: &[u8], offset: usize) -> DisplayListDataSpan {
    read_command::<DisplayListDataSpan>(&bytes[offset..])
}

fn write_span(bytes: &mut [u8], offset: usize, span: DisplayListDataSpan) {
    span.write_ffi_bytes(&mut bytes[offset..offset + size_of::<DisplayListDataSpan>()]);
}

// Appends the record `header` and `payload` make to `out`, with the records of the SVG images it paints, and of those
// the records nested in it paint, in the payloads of the commands that paint them.
fn append_record_holding_vector_image_records(
    header: &DisplayListCommandHeader,
    payload: &[u8],
    vector_images: &HashMap<u64, Arc<VectorImageRecords>>,
    out: &mut Vec<u8>,
) {
    let record_start = out.len();
    out.extend_from_slice(&[0; HEADER_SIZE]);
    let payload_start = out.len();

    let mut nested_spans: SmallVec<[DisplayListDataSpan; 2]> = SmallVec::new();
    for_each_nested_record_span(header.command_type, payload, |_, span| {
        if !span.is_empty() {
            nested_spans.push(span);
        }
    });
    nested_spans.sort_unstable_by_key(|span| span.offset);

    let mut growths: SmallVec<[PayloadGrowth; 2]> = SmallVec::new();
    // The sizes the nested spans grow to, by the offsets they had.
    let mut grown_span_sizes: SmallVec<[(u32, u32); 2]> = SmallVec::new();
    let mut copied_up_to = 0;
    let mut vector_image_records_span = None;
    if header.command_type == DisplayListCommandType::PaintVectorImage {
        let command = read_command::<PaintVectorImage>(payload);
        if command.records.is_empty()
            && let Some(vector_image) = vector_images.get(&command.vector_image_id.0)
            && !vector_image.records.is_empty()
        {
            const { assert!(size_of::<PaintVectorImage>().is_multiple_of(8)) };
            let position = size_of::<PaintVectorImage>();
            out.extend_from_slice(&payload[..position]);
            out.extend_from_slice(&vector_image.records);
            copied_up_to = position;
            let growth = u32::try_from(vector_image.records.len()).expect("display list payload exceeds u32");
            growths.push(PayloadGrowth {
                position: position as u32,
                growth,
            });
            vector_image_records_span = Some((
                DisplayListDataSpan {
                    offset: position as u32,
                    size: growth,
                },
                vector_image.requires_direct_replay,
            ));
        }
    }
    for span in &nested_spans {
        let (start, end) = (span.offset as usize, (span.offset + span.size) as usize);
        out.extend_from_slice(&payload[copied_up_to..start]);
        let grown_start = out.len();
        append_records_holding_vector_image_records(&payload[start..end], vector_images, out);
        let grown_size = u32::try_from(out.len() - grown_start).expect("display list payload exceeds u32");
        copied_up_to = end;
        if grown_size != span.size {
            growths.push(PayloadGrowth {
                position: end as u32,
                growth: grown_size - span.size,
            });
            grown_span_sizes.push((span.offset, grown_size));
        }
    }
    out.extend_from_slice(&payload[copied_up_to..]);

    let mut grown_header = *header;
    if !growths.is_empty() {
        let total_growth: u32 = growths.iter().map(|growth| growth.growth).sum();
        grown_header.payload_size += total_growth;
        let grown_payload = &mut out[payload_start..];
        let mut span_field_offsets = span_field_offsets_of(header.command_type);
        // The paths of the inline clips sit after everything else the payload holds but the entries that name them.
        let clip_entries_start =
            payload.len() + total_growth as usize - usize::from(header.inline_clip_count) * INLINE_CLIP_ENTRY_SIZE;
        span_field_offsets.extend((0..usize::from(header.inline_clip_count)).map(|index| {
            clip_entries_start + index * INLINE_CLIP_ENTRY_SIZE + offset_of!(DisplayListInlineClip, path_data)
        }));
        for field_offset in span_field_offsets {
            let mut span = read_span(grown_payload, field_offset);
            if let Some(&(_, grown_size)) = grown_span_sizes.iter().find(|(offset, _)| *offset == span.offset)
                && !span.is_empty()
            {
                span.size = grown_size;
            }
            span.offset = offset_after_growth(span.offset, &growths);
            write_span(grown_payload, field_offset, span);
        }
        if let Some((records_span, requires_direct_replay)) = vector_image_records_span {
            write_span(grown_payload, offset_of!(PaintVectorImage, records), records_span);
            grown_payload[offset_of!(PaintVectorImage, requires_direct_replay)] = u8::from(requires_direct_replay);
        }
    }
    grown_header.write_ffi_bytes(&mut out[record_start..record_start + HEADER_SIZE]);
}

fn append_records_holding_vector_image_records(
    records: &[u8],
    vector_images: &HashMap<u64, Arc<VectorImageRecords>>,
    out: &mut Vec<u8>,
) {
    for_each_command(records, |header, _, payload| {
        append_record_holding_vector_image_records(header, payload, vector_images, out);
    });
}

/// The tape of `display_list` with the records of `vector_images`, the SVG images its commands name, in the payloads
/// of the commands that paint them, as the compositor replays it.
pub(crate) fn display_list_holding_vector_image_records(
    display_list: &RecordedDisplayList,
    vector_images: &HashMap<u64, Arc<VectorImageRecords>>,
) -> RecordedDisplayList {
    if vector_images.is_empty() {
        return display_list.clone();
    }
    let mut bytes = Vec::with_capacity(
        display_list.bytes.len()
            + vector_images
                .values()
                .map(|vector_image| vector_image.records.len())
                .sum::<usize>(),
    );
    let mut command_runs = Vec::with_capacity(display_list.command_runs.len());
    for run in &display_list.command_runs {
        let offset = u32::try_from(bytes.len()).expect("display list exceeds u32");
        let records = &display_list.bytes[run.offset as usize..(run.offset + run.size) as usize];
        append_records_holding_vector_image_records(records, vector_images, &mut bytes);
        command_runs.push(DisplayListCommandRun {
            offset,
            size: u32::try_from(bytes.len()).expect("display list exceeds u32") - offset,
            ..*run
        });
    }
    debug_assert!(
        display_list.command_runs.is_empty() || read_header(&bytes).payload_size as usize + HEADER_SIZE <= bytes.len()
    );
    RecordedDisplayList { bytes, command_runs }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::painting::display_list::builder::{DisplayListBuilder, PendingInlineClip, inline_clips_of};
    use crate::painting::display_list::commands::{ContextRef, DisplayListResourceId, Repeat};
    use crate::painting::display_list::nested_records::span_bytes;
    use crate::painting::display_list::storage::{DisplayListStorage, receive_packed};
    use crate::painting::visual_context::VisualContextTree;
    use libgfx_rust::path::Path;
    use libgfx_rust::{
        Color, CompositingAndBlendingOperator, FloatPoint, FloatRect, FloatSize, IntRect, IntSize, MaskKind,
        ScalingMode, WindingRule,
    };

    fn fill(color: Color) -> FillRect {
        FillRect {
            rect: IntRect::new(0, 0, 10, 10),
            color,
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    fn reference_to(vector_image: &VectorImageRecords) -> PaintVectorImage {
        PaintVectorImage {
            vector_image_id: DisplayListResourceId(vector_image.id()),
            rect: FloatRect::new(0.0, 0.0, 10.0, 10.0),
            recorded_size: IntSize { width: 10, height: 10 },
            records: DisplayListDataSpan::default(),
            requires_direct_replay: false,
        }
    }

    fn vector_image_of(fills: &[FillRect]) -> Arc<VectorImageRecords> {
        let mut builder = DisplayListBuilder::new();
        for fill in fills {
            builder.append(fill, &[], ContextRef::default());
        }
        VectorImageRecords::recorded(&builder.finish(), &DisplayListResources::default())
    }

    fn triangle_clip() -> PendingInlineClip {
        let mut path = Path::new();
        path.move_to(FloatPoint { x: 0.0, y: 0.0 });
        path.line_to(FloatPoint { x: 10.0, y: 0.0 });
        path.line_to(FloatPoint { x: 0.0, y: 10.0 });
        path.close();
        debug_assert_eq!(path.serialize_to_bytes(), triangle_clip_path_bytes());
        PendingInlineClip::intersecting_path(&path, WindingRule::Nonzero)
    }

    fn triangle_clip_path_bytes() -> Vec<u8> {
        let mut path = Path::new();
        path.move_to(FloatPoint { x: 0.0, y: 0.0 });
        path.line_to(FloatPoint { x: 10.0, y: 0.0 });
        path.line_to(FloatPoint { x: 0.0, y: 10.0 });
        path.close();
        path.serialize_to_bytes()
    }

    // Checks the tape as the compositor does when it receives it.
    fn assert_is_valid_for_the_compositor(display_list: &RecordedDisplayList) {
        let storage = DisplayListStorage::recorded(
            Arc::new(display_list.clone()),
            Arc::new(VisualContextTree::create_identity()),
        );
        receive_packed(
            &storage.packed(),
            display_list.bytes.len(),
            display_list.command_runs.len(),
        )
        .expect("the tape passes the compositor's validation");
    }

    #[test]
    fn a_tape_takes_the_records_of_the_images_it_names_wherever_it_names_them() {
        let vector_image = vector_image_of(&[fill(Color::from_rgb(255, 0, 0)), fill(Color::from_rgb(0, 255, 0))]);
        let vector_images = HashMap::from([(vector_image.id(), vector_image.clone())]);

        let mut builder = DisplayListBuilder::new();
        let context = ContextRef::default();
        builder.append(&fill(Color::from_rgb(0, 0, 255)), &[], context);
        // A reference under a clip path, whose bytes sit after the command struct.
        builder.append_with_inline_clips(&reference_to(&vector_image), &[], context, &[triangle_clip()]);
        // A reference in a tile, in the content of a masked group whose filter bytes follow the nested records.
        let mut group = builder.begin_group::<DrawIsolatedGroup>(vec![triangle_clip()]);
        let tile = builder.begin_group::<DrawRepeatedTile>(Vec::new());
        builder.append(&reference_to(&vector_image), &[], context);
        let tile_span = builder.group_content_span(&tile);
        builder.finish_group(
            tile,
            &DrawRepeatedTile {
                dst_rect: FloatRect::new(0.0, 0.0, 10.0, 10.0),
                clip_rect: IntRect::new(0, 0, 10, 10),
                tile_size: IntSize { width: 10, height: 10 },
                tile_step: FloatSize {
                    width: 10.0,
                    height: 10.0,
                },
                tile: tile_span,
                scaling_mode: ScalingMode::Bilinear,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                repeat: Repeat { x: true, y: true },
            },
            context,
        );
        builder.begin_group_mask(&mut group);
        builder.append(&reference_to(&vector_image), &[], context);
        let filter = builder.append_group_inline_data(&mut group, &[1, 2, 3, 4, 5]);
        let (content, mask) = (builder.group_content_span(&group), builder.group_mask_span(&group));
        builder.finish_group(
            group,
            &DrawIsolatedGroup {
                clip_rect: FloatRect::new(0.0, 0.0, 10.0, 10.0),
                content,
                mask,
                filter,
                opacity: 1.0,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                mask_kind: MaskKind::Alpha,
            },
            context,
        );
        builder.append(&fill(Color::from_rgb(255, 255, 255)), &[], context);
        let display_list = builder.finish();

        let expanded = display_list_holding_vector_image_records(&display_list, &vector_images);
        assert_eq!(expanded.command_runs.len(), 1);
        assert_eq!(expanded.command_runs[0].size as usize, expanded.bytes.len());
        assert_eq!(
            expanded.bytes.len(),
            display_list.bytes.len() + 3 * vector_image.records.len()
        );
        assert_is_valid_for_the_compositor(&expanded);

        let mut images_found = 0;
        let mut clip_paths_found = 0;
        let mut filters_found = 0;
        crate::painting::display_list::nested_records::for_each_command_including_nested(
            &expanded.bytes,
            &mut |command_type, _, payload| match command_type {
                DisplayListCommandType::PaintVectorImage => {
                    let command = read_command::<PaintVectorImage>(payload);
                    assert_eq!(span_bytes(payload, command.records), &vector_image.records[..]);
                    images_found += 1;
                }
                DisplayListCommandType::DrawIsolatedGroup => {
                    let command = read_command::<DrawIsolatedGroup>(payload);
                    assert_eq!(span_bytes(payload, command.filter), &[1, 2, 3, 4, 5]);
                    filters_found += 1;
                }
                _ => {}
            },
        );
        assert_eq!((images_found, filters_found), (3, 1));
        for_each_command(&expanded.bytes, |header, _, payload| {
            for clip in inline_clips_of(header, payload) {
                assert_eq!(span_bytes(payload, clip.path_data), &triangle_clip_path_bytes()[..]);
                clip_paths_found += 1;
            }
        });
        assert_eq!(clip_paths_found, 2);
    }

    #[test]
    fn the_records_of_an_image_hold_those_of_the_images_it_draws() {
        let inner = vector_image_of(&[fill(Color::from_rgb(255, 0, 0))]);
        let mut builder = DisplayListBuilder::new();
        builder.append(&reference_to(&inner), &[], ContextRef::default());
        let resources = DisplayListResources {
            vector_images: HashMap::from([(inner.id(), inner.clone())]),
            ..Default::default()
        };
        let outer = VectorImageRecords::recorded(&builder.finish(), &resources);
        assert!(outer.resources.vector_images.is_empty());
        for_each_command(&outer.records, |header, _, payload| {
            assert_eq!(header.command_type, DisplayListCommandType::PaintVectorImage);
            let command = read_command::<PaintVectorImage>(payload);
            assert_eq!(span_bytes(payload, command.records), &inner.records[..]);
        });
    }
}
