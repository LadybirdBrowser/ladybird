/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::commands::*;
use crate::painting::display_list::ffi_bytes::FfiBytes;
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{CornerRadii, FloatRect, IntRect, WindingRule, enclosing_int_rect};
use std::rc::Rc;

pub const COMMAND_ALIGNMENT: usize = 16;
pub const HEADER_SIZE: usize = std::mem::size_of::<DisplayListCommandHeader>();
const PATH_DATA_ALIGNMENT: usize = std::mem::align_of::<u32>();

#[derive(Clone)]
pub struct PendingInlineClip {
    kind: InlineClipKind,
    mode: ClipMode,
    clip_rect_or_path_device_bounds: FloatRect,
    corner_radii: CornerRadii,
    path_winding_rule: WindingRule,
    serialized_path_bytes: Option<Rc<Vec<u8>>>,
    narrows_clip_rect_to_the_command_bounding_rect: bool,
}

impl PendingInlineClip {
    pub fn intersecting_device_rect(rect: IntRect) -> Self {
        Self {
            narrows_clip_rect_to_the_command_bounding_rect: true,
            ..Self::intersecting_float_rect(rect.to_float())
        }
    }

    pub fn intersecting_float_rect(rect: FloatRect) -> Self {
        Self {
            kind: InlineClipKind::Rect,
            mode: ClipMode::Intersect,
            clip_rect_or_path_device_bounds: rect,
            corner_radii: CornerRadii::default(),
            path_winding_rule: WindingRule::Nonzero,
            serialized_path_bytes: None,
            narrows_clip_rect_to_the_command_bounding_rect: false,
        }
    }

    pub fn intersecting_rounded_rect(rect: FloatRect, corner_radii: CornerRadii) -> Self {
        Self {
            kind: InlineClipKind::RoundedRect,
            corner_radii,
            ..Self::intersecting_float_rect(rect)
        }
    }

    pub fn subtracting_rect(rect: FloatRect) -> Self {
        Self {
            mode: ClipMode::Difference,
            ..Self::intersecting_float_rect(rect)
        }
    }

    pub fn subtracting_rounded_rect(rect: FloatRect, corner_radii: CornerRadii) -> Self {
        Self {
            mode: ClipMode::Difference,
            ..Self::intersecting_rounded_rect(rect, corner_radii)
        }
    }

    pub fn intersecting_path(path: &OwnedPath, path_winding_rule: WindingRule) -> Self {
        Self {
            kind: InlineClipKind::Path,
            path_winding_rule,
            serialized_path_bytes: Some(Rc::new(path.serialize_to_bytes())),
            ..Self::intersecting_float_rect(enclosing_int_rect(FloatRect::from_array(path.bounding_box())).to_float())
        }
    }

    fn header_bounding_rect_restriction(&self) -> Option<IntRect> {
        match self.mode {
            ClipMode::Intersect => Some(enclosing_int_rect(self.clip_rect_or_path_device_bounds)),
            ClipMode::Difference => None,
        }
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct CommandRange {
    pub offset: u32,
    pub size: u32,
}

impl CommandRange {
    pub const fn is_empty(self) -> bool {
        self.size == 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ContextRewrite {
    pub recorded_context: ContextRef,
    pub current_context: ContextRef,
}

impl ContextRewrite {
    fn is_identity(&self) -> bool {
        self.recorded_context == self.current_context
    }

    fn rewrite(&self, context: ContextRef) -> ContextRef {
        let spatial = if context.spatial == self.recorded_context.spatial {
            self.current_context.spatial
        } else {
            context.spatial
        };
        let frame = if context.frame == self.recorded_context.frame {
            self.current_context.frame
        } else {
            context.frame
        };
        ContextRef { spatial, frame }
    }
}

// A finished tape together with the run table summarizing it.
#[derive(Default)]
pub struct RecordedDisplayList {
    pub bytes: Vec<u8>,
    pub command_runs: Vec<DisplayListCommandRun>,
}

#[derive(Default)]
pub struct DisplayListBuilder {
    bytes: Vec<u8>,
    runs: Vec<DisplayListCommandRun>,
}

impl DisplayListBuilder {
    pub fn new() -> Self {
        Self::default()
    }

    pub fn bytes(&self) -> &[u8] {
        &self.bytes
    }

    pub fn command_runs(&self) -> &[DisplayListCommandRun] {
        &self.runs
    }

    pub fn finish(self) -> RecordedDisplayList {
        RecordedDisplayList {
            bytes: self.bytes,
            command_runs: self.runs,
        }
    }

    pub fn byte_size(&self) -> usize {
        self.bytes.len()
    }

    pub fn append<C: DisplayListCommand>(&mut self, command: &C, inline_data: &[u8], context: ContextRef) {
        self.append_with_inline_clips(command, inline_data, context, &[]);
    }

    pub fn append_with_inline_clips<C: DisplayListCommand>(
        &mut self,
        command: &C,
        inline_data: &[u8],
        context: ContextRef,
        inline_clips: &[PendingInlineClip],
    ) {
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        let inline_clip_count = u8::try_from(inline_clips.len()).expect("too many inline clips on one command");
        let mut unpadded_payload_size = std::mem::size_of::<C>() + inline_data.len();
        let mut path_spans = vec![DisplayListDataSpan::default(); inline_clips.len()];
        for (index, clip) in inline_clips.iter().enumerate() {
            let Some(path_bytes) = &clip.serialized_path_bytes else {
                continue;
            };
            let path_offset = unpadded_payload_size.next_multiple_of(PATH_DATA_ALIGNMENT);
            path_spans[index] = DisplayListDataSpan {
                offset: u32::try_from(path_offset).expect("display list payload exceeds u32"),
                size: u32::try_from(path_bytes.len()).expect("display list payload exceeds u32"),
            };
            unpadded_payload_size = path_offset + path_bytes.len();
        }
        let entries_size = inline_clips.len() * INLINE_CLIP_ENTRY_SIZE;
        let padded_record_size =
            (HEADER_SIZE + unpadded_payload_size + entries_size).next_multiple_of(COMMAND_ALIGNMENT);
        let payload_size = padded_record_size - HEADER_SIZE;
        let entries_offset = payload_size - entries_size;
        debug_assert_eq!(entries_offset % COMMAND_ALIGNMENT, 0);
        let mut bounding_rect = command.bounding_rect();
        for clip in inline_clips {
            if let Some(restriction) = clip.header_bounding_rect_restriction() {
                bounding_rect = Some(match bounding_rect {
                    Some(rect) => rect.intersected(restriction),
                    None => restriction,
                });
            }
        }
        let header = DisplayListCommandHeader {
            command_type: C::COMMAND_TYPE,
            has_bounding_rect: bounding_rect.is_some(),
            inline_clip_count,
            payload_size: u32::try_from(payload_size).expect("display list payload exceeds u32"),
            context,
            bounding_rect: bounding_rect.unwrap_or_default(),
        };
        let start = self.bytes.len();
        self.bytes.resize(start + padded_record_size, 0);
        header.write_ffi_bytes(&mut self.bytes[start..start + HEADER_SIZE]);
        let payload_start = start + HEADER_SIZE;
        command.write_ffi_bytes(&mut self.bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
        let inline_start = payload_start + std::mem::size_of::<C>();
        self.bytes[inline_start..inline_start + inline_data.len()].copy_from_slice(inline_data);
        for (index, clip) in inline_clips.iter().enumerate() {
            if let Some(path_bytes) = &clip.serialized_path_bytes {
                let path_start = payload_start + path_spans[index].offset as usize;
                self.bytes[path_start..path_start + path_bytes.len()].copy_from_slice(path_bytes);
            }
            let entry = DisplayListInlineClip {
                clip_rect_or_path_device_bounds: if clip.narrows_clip_rect_to_the_command_bounding_rect {
                    header.bounding_rect.to_float()
                } else {
                    clip.clip_rect_or_path_device_bounds
                },
                corner_radii: clip.corner_radii,
                path_data: path_spans[index],
                path_winding_rule: clip.path_winding_rule,
                kind: clip.kind,
                mode: clip.mode,
            };
            let entry_start = payload_start + entries_offset + index * INLINE_CLIP_ENTRY_SIZE;
            entry.write_ffi_bytes(&mut self.bytes[entry_start..entry_start + INLINE_CLIP_ENTRY_SIZE]);
        }
        note_command(&mut self.runs, &header, start, padded_record_size);
    }

    pub fn append_command_range(&mut self, source: &[u8], range: CommandRange, rewrite: Option<ContextRewrite>) -> u32 {
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        debug_assert_eq!(range.size as usize % COMMAND_ALIGNMENT, 0);
        let destination_offset = self.bytes.len();
        if range.is_empty() {
            return u32::try_from(destination_offset).expect("display list exceeds u32");
        }
        let source_range = &source[range.offset as usize..(range.offset + range.size) as usize];
        self.bytes.extend_from_slice(source_range);
        self.note_appended_records(destination_offset, rewrite.filter(|rewrite| !rewrite.is_identity()));
        u32::try_from(destination_offset).expect("display list exceeds u32")
    }

    // Folds the records appended from `start` on into the run table, first rewriting their
    // contexts when a spliced capture is replayed under different ones.
    fn note_appended_records(&mut self, start: usize, rewrite: Option<ContextRewrite>) {
        let Self { bytes, runs } = self;
        let mut offset = start;
        while offset < bytes.len() {
            let mut header = read_header(&bytes[offset..]);
            if let Some(rewrite) = rewrite {
                let context = rewrite.rewrite(header.context);
                if context != header.context {
                    header.context = context;
                    let field_offset = offset + std::mem::offset_of!(DisplayListCommandHeader, context);
                    context.write_ffi_bytes(&mut bytes[field_offset..field_offset + std::mem::size_of::<ContextRef>()]);
                }
            }
            let record_size = HEADER_SIZE + header.payload_size as usize;
            note_command(runs, &header, offset, record_size);
            offset += record_size;
        }
        assert_eq!(offset, bytes.len());
    }
}

fn note_command(
    runs: &mut Vec<DisplayListCommandRun>,
    header: &DisplayListCommandHeader,
    offset: usize,
    record_size: usize,
) {
    let offset = u32::try_from(offset).expect("display list exceeds u32");
    let record_size = u32::try_from(record_size).expect("display list record exceeds u32");
    if runs.last().is_none_or(|run| run.context != header.context) {
        runs.push(DisplayListCommandRun {
            offset,
            context: header.context,
            ..Default::default()
        });
    }
    let run = runs.last_mut().expect("a run was pushed above");
    debug_assert_eq!(run.offset + run.size, offset);
    run.size += record_size;
    if header.command_type.is_compositor_metadata() {
        run.has_compositor_metadata = true;
    } else if header.has_bounding_rect {
        run.ink_bounds = run.ink_bounds.united(header.bounding_rect);
    } else {
        run.has_unbounded_draw = true;
    }
}

pub fn read_header(bytes: &[u8]) -> DisplayListCommandHeader {
    assert!(bytes.len() >= HEADER_SIZE);
    let cursor = HeaderReader { bytes };
    DisplayListCommandHeader {
        command_type: DisplayListCommandType::from_u8(
            cursor.u8_at(std::mem::offset_of!(DisplayListCommandHeader, command_type)),
        )
        .expect("invalid display list command type"),
        has_bounding_rect: cursor.bool_at(std::mem::offset_of!(DisplayListCommandHeader, has_bounding_rect)),
        inline_clip_count: cursor.u8_at(std::mem::offset_of!(DisplayListCommandHeader, inline_clip_count)),
        payload_size: cursor.u32_at(std::mem::offset_of!(DisplayListCommandHeader, payload_size)),
        context: {
            let base = std::mem::offset_of!(DisplayListCommandHeader, context);
            ContextRef {
                spatial: SpatialNodeIndex(cursor.u32_at(base + std::mem::offset_of!(ContextRef, spatial))),
                frame: FrameNodeIndex(cursor.u32_at(base + std::mem::offset_of!(ContextRef, frame))),
            }
        },
        bounding_rect: {
            let base = std::mem::offset_of!(DisplayListCommandHeader, bounding_rect);
            libgfx_rust::IntRect {
                x: cursor.i32_at(base),
                y: cursor.i32_at(base + 4),
                width: cursor.i32_at(base + 8),
                height: cursor.i32_at(base + 12),
            }
        },
    }
}

struct HeaderReader<'a> {
    bytes: &'a [u8],
}

impl HeaderReader<'_> {
    fn u8_at(&self, offset: usize) -> u8 {
        self.bytes[offset]
    }
    fn bool_at(&self, offset: usize) -> bool {
        self.bytes[offset] != 0
    }
    fn u32_at(&self, offset: usize) -> u32 {
        u32::from_ne_bytes(self.bytes[offset..offset + 4].try_into().unwrap())
    }
    fn i32_at(&self, offset: usize) -> i32 {
        i32::from_ne_bytes(self.bytes[offset..offset + 4].try_into().unwrap())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatRect, IntRect};

    fn context(spatial: u32, frame: Option<u32>) -> ContextRef {
        ContextRef {
            spatial: SpatialNodeIndex(spatial),
            frame: frame.map_or(FrameNodeIndex::NONE, FrameNodeIndex),
        }
    }

    fn fill_rect(x: i32, y: i32, width: i32, height: i32) -> FillRect {
        FillRect {
            rect: IntRect::new(x, y, width, height),
            color: Color::default(),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
        }
    }

    fn whole_tape(builder: &DisplayListBuilder) -> CommandRange {
        CommandRange {
            offset: 0,
            size: u32::try_from(builder.byte_size()).unwrap(),
        }
    }

    fn rewrite(recorded: ContextRef, current: ContextRef) -> ContextRewrite {
        ContextRewrite {
            recorded_context: recorded,
            current_context: current,
        }
    }

    fn header_contexts(builder: &DisplayListBuilder) -> Vec<ContextRef> {
        let mut contexts = Vec::new();
        let mut offset = 0;
        while offset < builder.byte_size() {
            let header = read_header(&builder.bytes()[offset..]);
            contexts.push(header.context);
            offset += HEADER_SIZE + header.payload_size as usize;
        }
        contexts
    }

    fn assert_runs_cover_tape(builder: &DisplayListBuilder) {
        let mut expected_offset = 0;
        for run in builder.command_runs() {
            assert_eq!(run.offset, expected_offset);
            assert_ne!(run.size, 0);
            assert_eq!(run.size as usize % COMMAND_ALIGNMENT, 0);
            expected_offset += run.size;
        }
        assert_eq!(expected_offset as usize, builder.byte_size());
    }

    #[test]
    fn consecutive_commands_in_one_context_share_a_run() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append(&fill_rect(0, 0, 10, 10), &[], root);
        builder.append(&fill_rect(20, 20, 10, 10), &[], root);
        assert_runs_cover_tape(&builder);
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].context, root);
        assert_eq!(runs[0].ink_bounds, IntRect::new(0, 0, 30, 30));
        assert!(!runs[0].has_unbounded_draw);
        assert!(!runs[0].has_compositor_metadata);
    }

    #[test]
    fn context_changes_start_new_runs() {
        let mut builder = DisplayListBuilder::new();
        let a = context(1, None);
        let b = context(1, Some(0));
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        builder.append(&fill_rect(0, 0, 10, 10), &[], b);
        builder.append(&fill_rect(0, 0, 10, 10), &[], b);
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        assert_runs_cover_tape(&builder);
        let contexts: Vec<_> = builder.command_runs().iter().map(|run| run.context).collect();
        assert_eq!(contexts, vec![a, b, a]);
    }

    #[test]
    fn ink_bounds_skip_metadata() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append(&fill_rect(5, 5, 10, 10), &[], root);
        let metadata = CompositorBlockingWheelEventRegion {
            rect: FloatRect::new(0.0, 0.0, 900.0, 900.0),
        };
        builder.append(&metadata, &[], root);
        let run = builder.command_runs()[0];
        assert_eq!(run.ink_bounds, IntRect::new(5, 5, 10, 10));
        assert!(run.has_compositor_metadata);
        assert!(!run.has_unbounded_draw);
    }

    fn inline_clip_entries(builder: &DisplayListBuilder, record_offset: usize) -> Vec<DisplayListInlineClip> {
        let header = read_header(&builder.bytes()[record_offset..]);
        let payload =
            &builder.bytes()[record_offset + HEADER_SIZE..record_offset + HEADER_SIZE + header.payload_size as usize];
        let count = header.inline_clip_count as usize;
        let entries_offset = payload.len() - count * INLINE_CLIP_ENTRY_SIZE;
        (0..count)
            .map(|index| {
                let entry_bytes = &payload[entries_offset + index * INLINE_CLIP_ENTRY_SIZE..];
                let reader = HeaderReader { bytes: entry_bytes };
                DisplayListInlineClip {
                    clip_rect_or_path_device_bounds: FloatRect {
                        x: f32::from_ne_bytes(entry_bytes[0..4].try_into().unwrap()),
                        y: f32::from_ne_bytes(entry_bytes[4..8].try_into().unwrap()),
                        width: f32::from_ne_bytes(entry_bytes[8..12].try_into().unwrap()),
                        height: f32::from_ne_bytes(entry_bytes[12..16].try_into().unwrap()),
                    },
                    corner_radii: CornerRadii::default(),
                    path_data: DisplayListDataSpan {
                        offset: reader.u32_at(std::mem::offset_of!(DisplayListInlineClip, path_data)),
                        size: reader.u32_at(std::mem::offset_of!(DisplayListInlineClip, path_data) + 4),
                    },
                    path_winding_rule: WindingRule::Nonzero,
                    kind: match reader.u8_at(std::mem::offset_of!(DisplayListInlineClip, kind)) {
                        0 => InlineClipKind::Rect,
                        1 => InlineClipKind::RoundedRect,
                        _ => InlineClipKind::Path,
                    },
                    mode: if reader.u8_at(std::mem::offset_of!(DisplayListInlineClip, mode)) == 0 {
                        ClipMode::Intersect
                    } else {
                        ClipMode::Difference
                    },
                }
            })
            .collect()
    }

    #[test]
    fn a_confining_clip_narrows_the_header_and_appends_a_tail_entry() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append_with_inline_clips(
            &fill_rect(0, 0, 100, 100),
            &[],
            root,
            &[PendingInlineClip::intersecting_device_rect(IntRect::new(
                10, 10, 20, 20,
            ))],
        );
        builder.append(&fill_rect(50, 50, 10, 10), &[], root);
        let header = read_header(builder.bytes());
        assert_eq!(header.inline_clip_count, 1);
        assert!(header.has_bounding_rect);
        assert_eq!(header.bounding_rect, IntRect::new(10, 10, 20, 20));
        let entries = inline_clip_entries(&builder, 0);
        assert_eq!(entries[0].kind, InlineClipKind::Rect);
        assert_eq!(entries[0].mode, ClipMode::Intersect);
        assert_eq!(
            entries[0].clip_rect_or_path_device_bounds,
            FloatRect::new(10.0, 10.0, 20.0, 20.0)
        );
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].ink_bounds, IntRect::new(10, 10, 50, 50));
    }

    #[test]
    fn a_difference_entry_keeps_the_command_bounds_and_serializes_its_own_rect() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append_with_inline_clips(
            &fill_rect(0, 0, 100, 100),
            &[],
            root,
            &[
                PendingInlineClip::intersecting_float_rect(FloatRect::new(5.0, 5.0, 200.0, 200.0)),
                PendingInlineClip::subtracting_rect(FloatRect::new(20.0, 20.0, 10.0, 10.0)),
            ],
        );
        let header = read_header(builder.bytes());
        assert_eq!(header.inline_clip_count, 2);
        assert_eq!(header.bounding_rect, IntRect::new(5, 5, 95, 95));
        let entries = inline_clip_entries(&builder, 0);
        assert_eq!(
            entries[0].clip_rect_or_path_device_bounds,
            FloatRect::new(5.0, 5.0, 200.0, 200.0)
        );
        assert_eq!(entries[1].mode, ClipMode::Difference);
        assert_eq!(
            entries[1].clip_rect_or_path_device_bounds,
            FloatRect::new(20.0, 20.0, 10.0, 10.0)
        );
    }

    #[test]
    fn spliced_records_keep_inline_clip_entries_byte_identical() {
        let mut source = DisplayListBuilder::new();
        let recorded = context(4, None);
        source.append_with_inline_clips(
            &fill_rect(0, 0, 100, 100),
            &[1, 2, 3],
            recorded,
            &[PendingInlineClip::intersecting_device_rect(IntRect::new(
                10, 10, 20, 20,
            ))],
        );

        let mut builder = DisplayListBuilder::new();
        let current = context(7, Some(1));
        let destination_offset =
            builder.append_command_range(source.bytes(), whole_tape(&source), Some(rewrite(recorded, current)));
        let source_header = read_header(source.bytes());
        let spliced_header = read_header(&builder.bytes()[destination_offset as usize..]);
        assert_eq!(spliced_header.context, current);
        assert_eq!(spliced_header.inline_clip_count, source_header.inline_clip_count);
        let payload_of = |bytes: &[u8], offset: usize| {
            let header = read_header(&bytes[offset..]);
            bytes[offset + HEADER_SIZE..offset + HEADER_SIZE + header.payload_size as usize].to_vec()
        };
        assert_eq!(
            payload_of(source.bytes(), 0),
            payload_of(builder.bytes(), destination_offset as usize)
        );
    }

    #[test]
    fn a_rewritten_splice_merges_into_the_current_run() {
        let mut source = DisplayListBuilder::new();
        let recorded = context(4, None);
        source.append(&fill_rect(0, 0, 10, 10), &[], recorded);
        source.append(&fill_rect(50, 50, 10, 10), &[], recorded);

        let mut builder = DisplayListBuilder::new();
        let current = context(7, Some(1));
        builder.append(&fill_rect(100, 100, 10, 10), &[], current);
        let destination_offset =
            builder.append_command_range(source.bytes(), whole_tape(&source), Some(rewrite(recorded, current)));
        assert_ne!(destination_offset, 0);
        assert_runs_cover_tape(&builder);
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].context, current);
        assert_eq!(runs[0].ink_bounds, IntRect::new(0, 0, 110, 110));
    }

    #[test]
    fn a_spliced_capture_leaves_frames_other_than_the_phase_frame_untouched() {
        let mut source = DisplayListBuilder::new();
        source.append(&fill_rect(0, 0, 10, 10), &[], context(2, Some(5)));
        source.append(&fill_rect(20, 20, 10, 10), &[], context(2, Some(6)));
        source.append(&fill_rect(40, 40, 10, 10), &[], context(2, Some(1)));
        source.append(&fill_rect(60, 60, 10, 10), &[], context(2, None));

        let mut builder = DisplayListBuilder::new();
        builder.append_command_range(
            source.bytes(),
            whole_tape(&source),
            Some(rewrite(context(2, Some(1)), context(3, Some(7)))),
        );
        assert_eq!(
            header_contexts(&builder),
            vec![
                context(3, Some(5)),
                context(3, Some(6)),
                context(3, Some(7)),
                context(3, None)
            ]
        );
        assert_runs_cover_tape(&builder);
        assert_eq!(builder.command_runs().len(), 4);
    }

    #[test]
    fn a_verbatim_splice_reproduces_the_source_runs_rebased() {
        let mut source = DisplayListBuilder::new();
        let a = context(2, None);
        let b = context(3, Some(0));
        source.append(&fill_rect(0, 0, 10, 10), &[], a);
        source.append(&fill_rect(20, 20, 10, 10), &[], b);
        source.append(&fill_rect(40, 40, 10, 10), &[], b);
        let source_runs = source.command_runs().to_vec();
        assert_eq!(source_runs.len(), 2);

        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0, 0, 1, 1), &[], context(9, None));
        let destination_offset = builder.append_command_range(source.bytes(), whole_tape(&source), None);
        assert_runs_cover_tape(&builder);
        let spliced_runs = &builder.command_runs()[1..];
        assert_eq!(spliced_runs.len(), source_runs.len());
        for (spliced, original) in spliced_runs.iter().zip(&source_runs) {
            assert_eq!(spliced.offset, original.offset + destination_offset);
            assert_eq!(
                DisplayListCommandRun {
                    offset: original.offset,
                    ..*spliced
                },
                *original
            );
        }
    }
}
