/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::commands::*;
use crate::painting::display_list::ffi_bytes::FfiBytes;

pub const COMMAND_ALIGNMENT: usize = 16;
pub const HEADER_SIZE: usize = std::mem::size_of::<DisplayListCommandHeader>();

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
    pub recorded_local_frame_range: (u32, u32),
    pub current_local_frame_range: (u32, u32),
}

impl ContextRewrite {
    fn is_identity(&self) -> bool {
        self.recorded_context == self.current_context
            && self.recorded_local_frame_range.0 == self.current_local_frame_range.0
    }

    fn rewrite(&self, context: ContextRef) -> ContextRef {
        let spatial = if context.spatial == self.recorded_context.spatial {
            self.current_context.spatial
        } else {
            context.spatial
        };
        let (recorded_begin, recorded_end) = self.recorded_local_frame_range;
        let frame =
            if context.frame != FrameNodeIndex::NONE && (recorded_begin..recorded_end).contains(&context.frame.0) {
                FrameNodeIndex(context.frame.0 - recorded_begin + self.current_local_frame_range.0)
            } else if context.frame == self.recorded_context.frame {
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
        self.append_confined_to_clip(command, inline_data, context, None);
    }

    pub fn append_confined_to_clip<C: DisplayListCommand>(
        &mut self,
        command: &C,
        inline_data: &[u8],
        context: ContextRef,
        confining_clip: Option<libgfx_rust::IntRect>,
    ) {
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        let payload_size = std::mem::size_of::<C>() + inline_data.len();
        let padded_record_size = (HEADER_SIZE + payload_size).next_multiple_of(COMMAND_ALIGNMENT);
        let mut bounding_rect = command.bounding_rect();
        if let Some(clip) = confining_clip {
            let rect = bounding_rect.expect("a draw clipped to its bounds has a bounding rect");
            bounding_rect = Some(rect.intersected(clip));
        }
        let header = DisplayListCommandHeader {
            command_type: C::COMMAND_TYPE,
            has_bounding_rect: bounding_rect.is_some(),
            clips_to_bounding_rect: confining_clip.is_some(),
            payload_size: u32::try_from(padded_record_size - HEADER_SIZE).expect("display list payload exceeds u32"),
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
        clips_to_bounding_rect: cursor.bool_at(std::mem::offset_of!(DisplayListCommandHeader, clips_to_bounding_rect)),
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
    use libgfx_rust::{Color, FloatRect, IntRect};

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
            recorded_local_frame_range: (0, 0),
            current_local_frame_range: (0, 0),
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

    #[test]
    fn a_confining_clip_narrows_the_header_and_flags_the_draw() {
        let mut builder = DisplayListBuilder::new();
        let root = context(0, None);
        builder.append_confined_to_clip(
            &fill_rect(0, 0, 100, 100),
            &[],
            root,
            Some(IntRect::new(10, 10, 20, 20)),
        );
        builder.append(&fill_rect(50, 50, 10, 10), &[], root);
        let header = read_header(builder.bytes());
        assert!(header.clips_to_bounding_rect);
        assert!(header.has_bounding_rect);
        assert_eq!(header.bounding_rect, IntRect::new(10, 10, 20, 20));
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].ink_bounds, IntRect::new(10, 10, 50, 50));
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
    fn a_spliced_capture_rebases_the_frames_of_its_own_range() {
        let mut source = DisplayListBuilder::new();
        source.append(&fill_rect(0, 0, 10, 10), &[], context(2, Some(5)));
        source.append(&fill_rect(20, 20, 10, 10), &[], context(2, Some(6)));
        source.append(&fill_rect(40, 40, 10, 10), &[], context(2, None));

        let mut builder = DisplayListBuilder::new();
        builder.append_command_range(
            source.bytes(),
            whole_tape(&source),
            Some(ContextRewrite {
                recorded_context: context(2, Some(5)),
                current_context: context(3, Some(11)),
                recorded_local_frame_range: (4, 8),
                current_local_frame_range: (10, 14),
            }),
        );
        assert_eq!(
            header_contexts(&builder),
            vec![context(3, Some(11)), context(3, Some(12)), context(3, None)]
        );
        assert_runs_cover_tape(&builder);
        assert_eq!(builder.command_runs().len(), 3);
    }

    #[test]
    fn a_spliced_phase_frame_outside_the_range_maps_to_the_current_phase_frame() {
        let mut source = DisplayListBuilder::new();
        source.append(&fill_rect(0, 0, 10, 10), &[], context(2, Some(1)));

        let mut builder = DisplayListBuilder::new();
        builder.append_command_range(
            source.bytes(),
            whole_tape(&source),
            Some(ContextRewrite {
                recorded_context: context(2, Some(1)),
                current_context: context(2, Some(3)),
                recorded_local_frame_range: (7, 7),
                current_local_frame_range: (9, 9),
            }),
        );
        assert_eq!(header_contexts(&builder), vec![context(2, Some(3))]);
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
