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
pub struct AppendContext {
    pub context: ContextRef,
    pub has_empty_effective_clip: bool,
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

    pub fn append<C: DisplayListCommand>(&mut self, command: &C, inline_data: &[u8], context: AppendContext) -> bool {
        if context.has_empty_effective_clip {
            return false;
        }
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        let payload_size = std::mem::size_of::<C>() + inline_data.len();
        let padded_record_size = (HEADER_SIZE + payload_size).next_multiple_of(COMMAND_ALIGNMENT);
        let header = DisplayListCommandHeader {
            command_type: C::COMMAND_TYPE,
            has_bounding_rect: command.bounding_rect().is_some(),
            is_clip: command.is_clip(),
            payload_size: u32::try_from(padded_record_size - HEADER_SIZE).expect("display list payload exceeds u32"),
            context: context.context,
            bounding_rect: command.bounding_rect().unwrap_or_default(),
        };
        let start = self.bytes.len();
        self.bytes.resize(start + padded_record_size, 0);
        header.write_ffi_bytes(&mut self.bytes[start..start + HEADER_SIZE]);
        let payload_start = start + HEADER_SIZE;
        command.write_ffi_bytes(&mut self.bytes[payload_start..payload_start + std::mem::size_of::<C>()]);
        let inline_start = payload_start + std::mem::size_of::<C>();
        self.bytes[inline_start..inline_start + inline_data.len()].copy_from_slice(inline_data);
        note_command(&mut self.runs, &header, start, padded_record_size);
        true
    }

    pub fn append_command_range(
        &mut self,
        source: &[u8],
        range: CommandRange,
        recorded_context: ContextRef,
        current_context: ContextRef,
    ) -> u32 {
        // Captures are save/restore balanced (verified at capture end), so splicing never shifts the save nesting level.
        debug_assert_eq!(self.bytes.len() % COMMAND_ALIGNMENT, 0);
        debug_assert_eq!(range.size as usize % COMMAND_ALIGNMENT, 0);
        let destination_offset = self.bytes.len();
        if range.is_empty() {
            return u32::try_from(destination_offset).expect("display list exceeds u32");
        }
        let source_range = &source[range.offset as usize..(range.offset + range.size) as usize];
        self.bytes.extend_from_slice(source_range);
        let context_override = (recorded_context != current_context).then_some(current_context);
        self.note_appended_records(destination_offset, context_override);
        u32::try_from(destination_offset).expect("display list exceeds u32")
    }

    // Folds the records appended from `start` on into the run table, first rewriting their
    // context when a spliced capture is replayed under a different one.
    fn note_appended_records(&mut self, start: usize, context_override: Option<ContextRef>) {
        let Self { bytes, runs } = self;
        let mut offset = start;
        while offset < bytes.len() {
            let mut header = read_header(&bytes[offset..]);
            if let Some(context) = context_override {
                header.context = context;
                let field_offset = offset + std::mem::offset_of!(DisplayListCommandHeader, context);
                context.write_ffi_bytes(&mut bytes[field_offset..field_offset + std::mem::size_of::<ContextRef>()]);
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
    let nesting_level_change = header.command_type.nesting_level_change();
    if header.is_clip && run.nesting_delta == 0 {
        run.has_unconfined_clip = true;
    }
    run.nesting_delta += nesting_level_change;
    run.min_relative_nesting = run.min_relative_nesting.min(run.nesting_delta);
    if header.command_type.is_compositor_metadata() {
        run.has_compositor_metadata = true;
    } else if nesting_level_change == 0 && !header.is_clip {
        if header.has_bounding_rect {
            run.ink_bounds = run.ink_bounds.united(header.bounding_rect);
        } else {
            run.has_unbounded_draw = true;
        }
    }
    run.is_self_contained = run.nesting_delta == 0 && run.min_relative_nesting == 0 && !run.has_unconfined_clip;
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
        is_clip: cursor.bool_at(std::mem::offset_of!(DisplayListCommandHeader, is_clip)),
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

    fn appendable(context: ContextRef) -> AppendContext {
        AppendContext {
            context,
            has_empty_effective_clip: false,
        }
    }

    fn fill_rect(x: i32, y: i32, width: i32, height: i32) -> FillRect {
        FillRect {
            rect: IntRect::new(x, y, width, height),
            color: Color::default(),
        }
    }

    fn clip_rect() -> AddClipRect {
        AddClipRect {
            rect: FloatRect::new(0.0, 0.0, 500.0, 500.0),
        }
    }

    fn whole_tape(builder: &DisplayListBuilder) -> CommandRange {
        CommandRange {
            offset: 0,
            size: u32::try_from(builder.byte_size()).unwrap(),
        }
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
        let root = appendable(context(0, None));
        builder.append(&fill_rect(0, 0, 10, 10), &[], root);
        builder.append(&fill_rect(20, 20, 10, 10), &[], root);
        assert_runs_cover_tape(&builder);
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].context, root.context);
        assert_eq!(runs[0].ink_bounds, IntRect::new(0, 0, 30, 30));
        assert!(runs[0].is_self_contained);
        assert!(!runs[0].has_unbounded_draw);
        assert!(!runs[0].has_compositor_metadata);
    }

    #[test]
    fn context_changes_start_new_runs() {
        let mut builder = DisplayListBuilder::new();
        let a = appendable(context(1, None));
        let b = appendable(context(1, Some(0)));
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        builder.append(&fill_rect(0, 0, 10, 10), &[], b);
        builder.append(&fill_rect(0, 0, 10, 10), &[], b);
        builder.append(&fill_rect(0, 0, 10, 10), &[], a);
        assert_runs_cover_tape(&builder);
        let contexts: Vec<_> = builder.command_runs().iter().map(|run| run.context).collect();
        assert_eq!(contexts, vec![a.context, b.context, a.context]);
    }

    #[test]
    fn ink_bounds_skip_clips_and_metadata() {
        let mut builder = DisplayListBuilder::new();
        let root = appendable(context(0, None));
        builder.append(&Save::default(), &[], root);
        builder.append(&clip_rect(), &[], root);
        builder.append(&fill_rect(5, 5, 10, 10), &[], root);
        builder.append(&Restore::default(), &[], root);
        let metadata = CompositorBlockingWheelEventRegion {
            rect: FloatRect::new(0.0, 0.0, 900.0, 900.0),
        };
        builder.append(&metadata, &[], root);
        let run = builder.command_runs()[0];
        assert_eq!(run.ink_bounds, IntRect::new(5, 5, 10, 10));
        assert!(run.has_compositor_metadata);
        assert!(!run.has_unbounded_draw);
        assert!(!run.has_unconfined_clip);
        assert!(run.is_self_contained);
    }

    #[test]
    fn nesting_fields_follow_saves_and_restores() {
        let root = appendable(context(0, None));

        let mut builder = DisplayListBuilder::new();
        builder.append(&Save::default(), &[], root);
        let run = builder.command_runs()[0];
        assert_eq!(run.nesting_delta, 1);
        assert_eq!(run.min_relative_nesting, 0);
        assert!(!run.is_self_contained);

        let mut builder = DisplayListBuilder::new();
        builder.append(&Restore::default(), &[], root);
        builder.append(&Save::default(), &[], root);
        let run = builder.command_runs()[0];
        assert_eq!(run.nesting_delta, 0);
        assert_eq!(run.min_relative_nesting, -1);
        assert!(!run.is_self_contained);
    }

    #[test]
    fn a_clip_at_the_base_nesting_is_unconfined() {
        let mut builder = DisplayListBuilder::new();
        let root = appendable(context(0, None));
        builder.append(&clip_rect(), &[], root);
        builder.append(&fill_rect(0, 0, 10, 10), &[], root);
        let run = builder.command_runs()[0];
        assert!(run.has_unconfined_clip);
        assert!(!run.is_self_contained);
    }

    #[test]
    fn commands_under_an_empty_effective_clip_leave_no_run() {
        let mut builder = DisplayListBuilder::new();
        let dropped = AppendContext {
            context: context(0, Some(3)),
            has_empty_effective_clip: true,
        };
        assert!(!builder.append(&fill_rect(0, 0, 10, 10), &[], dropped));
        assert!(builder.command_runs().is_empty());
        assert_eq!(builder.byte_size(), 0);
    }

    #[test]
    fn a_rewritten_splice_merges_into_the_current_run() {
        let mut source = DisplayListBuilder::new();
        let recorded = appendable(context(4, None));
        source.append(&fill_rect(0, 0, 10, 10), &[], recorded);
        source.append(&fill_rect(50, 50, 10, 10), &[], recorded);

        let mut builder = DisplayListBuilder::new();
        let current = appendable(context(7, Some(1)));
        builder.append(&fill_rect(100, 100, 10, 10), &[], current);
        let destination_offset =
            builder.append_command_range(source.bytes(), whole_tape(&source), recorded.context, current.context);
        assert_ne!(destination_offset, 0);
        assert_runs_cover_tape(&builder);
        let runs = builder.command_runs();
        assert_eq!(runs.len(), 1);
        assert_eq!(runs[0].context, current.context);
        assert_eq!(runs[0].ink_bounds, IntRect::new(0, 0, 110, 110));
    }

    #[test]
    fn a_verbatim_splice_reproduces_the_source_runs_rebased() {
        let mut source = DisplayListBuilder::new();
        let a = appendable(context(2, None));
        let b = appendable(context(3, Some(0)));
        source.append(&fill_rect(0, 0, 10, 10), &[], a);
        source.append(&Save::default(), &[], b);
        source.append(&fill_rect(20, 20, 10, 10), &[], b);
        source.append(&Restore::default(), &[], b);
        let source_runs = source.command_runs().to_vec();
        assert_eq!(source_runs.len(), 2);

        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0, 0, 1, 1), &[], appendable(context(9, None)));
        let destination_offset = builder.append_command_range(
            source.bytes(),
            whole_tape(&source),
            ContextRef::default(),
            ContextRef::default(),
        );
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
