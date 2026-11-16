/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The chunks a tape is cut into.
//!
//! A producer of tapes ends a chunk where what it records next can change without what it recorded before. The tape
//! that replaces another is then mostly the same chunks, a recording reuses whole chunks of the one before it, and a
//! chunk is named by what it holds, so two tapes can be told apart chunk by chunk.

use super::builder::{for_each_command, has_inline_clip_in_its_own_space, read_command};
use super::commands::{
    CompositorScrollbar, DeclareMaskContent, DisplayListCommandRun, DisplayListCommandType, EffectNodeIndex,
    PaintScrollBar, SpatialNodeIndex,
};
use super::nested_records::{for_each_nested_record_span, span_bytes};
use super::storage::write_run;
use std::hash::{BuildHasher, Hasher};
use std::ops::Range;

/// Names a chunk by what it holds: chunks with one id hold the same records in the same runs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Hash)]
pub struct ChunkId(pub [u64; 2]);

impl ChunkId {
    /// The id of a chunk whose records can still change.
    pub const UNSET: Self = Self([0, 0]);
}

/// A stretch of whole records with the runs that cover them, which name the records by their offsets from where the
/// chunk starts. A chunk is immutable once named, so that every holder of one name draws the same.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct Chunk {
    pub id: ChunkId,
    pub records: Vec<u8>,
    pub runs: Vec<DisplayListCommandRun>,
}

impl Chunk {
    /// The chunk that holds `records` in `runs`, named by them.
    pub fn named(records: Vec<u8>, runs: Vec<DisplayListCommandRun>) -> Self {
        Self {
            id: chunk_id(&records, &runs, 0),
            records,
            runs,
        }
    }
}

/// How many of the latest display lists of a context both ends of a connection keep the chunks of.
pub const DISPLAY_LISTS_WHOSE_CHUNKS_ARE_KEPT: usize = 4;

/// The chunks both ends of a connection keep for one context: those its latest display lists name. A sender sends a
/// display list's chunks that are not kept, and both ends take in each list they send or receive in the same order,
/// so both always keep the same chunks without telling each other.
#[derive(Debug, Default)]
pub struct KeptChunks {
    latest_lists: std::collections::VecDeque<Box<[ChunkId]>>,
    lists_naming: std::collections::HashMap<ChunkId, u32>,
}

impl KeptChunks {
    pub fn keeps(&self, id: ChunkId) -> bool {
        self.lists_naming.contains_key(&id)
    }

    /// Takes in the display list the context presents next, which names `chunks`, and answers the chunks that no list
    /// still kept names.
    pub fn take_in(&mut self, chunks: &[ChunkId]) -> Vec<ChunkId> {
        let mut named_once: Vec<ChunkId> = chunks.to_vec();
        named_once.sort_unstable_by_key(|id| id.0);
        named_once.dedup();
        for id in &named_once {
            *self.lists_naming.entry(*id).or_default() += 1;
        }
        self.latest_lists.push_back(named_once.into_boxed_slice());
        let mut dropped = Vec::new();
        while self.latest_lists.len() > DISPLAY_LISTS_WHOSE_CHUNKS_ARE_KEPT {
            let oldest = self.latest_lists.pop_front().expect("more lists than are kept");
            for id in oldest {
                let count = self.lists_naming.get_mut(&id).expect("a kept list names a kept chunk");
                *count -= 1;
                if *count == 0 {
                    self.lists_naming.remove(&id);
                    dropped.push(id);
                }
            }
        }
        dropped
    }
}

/// Where a chunk of a tape ends. Chunks cover a tape, each starting where the one before it ends, and hold whole
/// runs: a run never continues into the next chunk, even where that one begins in the same visual context.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct DisplayListChunk {
    pub id: ChunkId,
    pub byte_end: u32,
    pub run_end: u32,
}

const RUN_SIZE: usize = std::mem::size_of::<DisplayListCommandRun>();

/// The id of the chunk that holds `bytes` in `runs`, which name the bytes by their offsets in the tape the chunk
/// starts at `chunk_start` of.
pub fn chunk_id(bytes: &[u8], runs: &[DisplayListCommandRun], chunk_start: u32) -> ChunkId {
    let write_runs = |hasher: &mut crate::fast_hash::FastHasher| {
        let mut run_bytes = [0; RUN_SIZE];
        for run in runs {
            write_run(
                &DisplayListCommandRun {
                    offset: run.offset - chunk_start,
                    ..*run
                },
                &mut run_bytes,
            );
            hasher.write(&run_bytes);
        }
    };
    // Two digests that differ in their seed and in the order they read the chunk in.
    let mut records_first = foldhash::fast::FixedState::with_seed(0x6368_756e_6b5f_6964).build_hasher();
    records_first.write(bytes);
    write_runs(&mut records_first);
    let mut runs_first = foldhash::fast::FixedState::with_seed(0x7461_7065_5f63_7574).build_hasher();
    write_runs(&mut runs_first);
    runs_first.write(bytes);
    ChunkId([records_first.finish(), runs_first.finish()])
}

/// The bytes and runs of each chunk of a tape, in tape order.
pub fn chunks_of<'a>(
    bytes: &'a [u8],
    runs: &'a [DisplayListCommandRun],
    chunks: &'a [DisplayListChunk],
) -> impl Iterator<Item = (&'a DisplayListChunk, &'a [u8], &'a [DisplayListCommandRun])> {
    let mut byte_start = 0;
    let mut run_start = 0;
    chunks.iter().map(move |chunk| {
        let chunk_bytes = &bytes[byte_start..chunk.byte_end as usize];
        let chunk_runs = &runs[run_start..chunk.run_end as usize];
        byte_start = chunk.byte_end as usize;
        run_start = chunk.run_end as usize;
        (chunk, chunk_bytes, chunk_runs)
    })
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ReachOutsideChunk {
    ScrollbarThumbFollowsScrollOffsetOf(SpatialNodeIndex),
    InlineClipInItsOwnSpace,
    DeclaresMaskContentOf(EffectNodeIndex),
    ScrollbarPaintedByCompositor,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct TopLevelRecordReachingOutsideItsChunk {
    pub record_offset: u32,
    pub run_index: u32,
    pub reach: ReachOutsideChunk,
}

pub fn top_level_records_reaching_outside_their_chunk(
    bytes: &[u8],
    runs: &[DisplayListCommandRun],
) -> Vec<TopLevelRecordReachingOutsideItsChunk> {
    let mut records = Vec::new();
    for (run_index, run) in runs.iter().enumerate() {
        let run_start = run.offset as usize;
        for_each_command(
            &bytes[run_start..run_start + run.size as usize],
            |header, offset_in_run, payload| {
                let mut note = |reach| {
                    records.push(TopLevelRecordReachingOutsideItsChunk {
                        record_offset: (run_start + offset_in_run) as u32,
                        run_index: run_index as u32,
                        reach,
                    });
                };
                match header.command_type {
                    DisplayListCommandType::PaintScrollBar => {
                        note(ReachOutsideChunk::ScrollbarThumbFollowsScrollOffsetOf(
                            read_command::<PaintScrollBar>(payload).scroll_node_index,
                        ));
                    }
                    DisplayListCommandType::CompositorScrollbar
                        if read_command::<CompositorScrollbar>(payload).is_painted_by_compositor =>
                    {
                        note(ReachOutsideChunk::ScrollbarPaintedByCompositor);
                    }
                    _ => {}
                }
                if header.inline_clip_count != 0 && has_inline_clip_in_its_own_space(header, payload) {
                    note(ReachOutsideChunk::InlineClipInItsOwnSpace);
                }
                for_each_mask_content_declared_in_record(header.command_type, payload, &mut |effect| {
                    note(ReachOutsideChunk::DeclaresMaskContentOf(effect));
                });
            },
        );
    }
    records
}

fn for_each_mask_content_declared_in_record(
    command_type: DisplayListCommandType,
    payload: &[u8],
    visit: &mut impl FnMut(EffectNodeIndex),
) {
    if command_type == DisplayListCommandType::DeclareMaskContent {
        visit(read_command::<DeclareMaskContent>(payload).effect);
    }
    for_each_nested_record_span(command_type, payload, |_, span| {
        for_each_command(span_bytes(payload, span), |nested_header, _, nested_payload| {
            for_each_mask_content_declared_in_record(nested_header.command_type, nested_payload, visit);
        });
    });
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct ChunkOfTape {
    id: ChunkId,
    run_end: u32,
    reaching_records_end: u32,
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct TapeChunks {
    chunks_in_paint_order: Vec<ChunkOfTape>,
    records_reaching_outside_their_chunks: Vec<TopLevelRecordReachingOutsideItsChunk>,
}

impl TapeChunks {
    pub fn len(&self) -> usize {
        self.chunks_in_paint_order.len()
    }

    pub fn is_empty(&self) -> bool {
        self.chunks_in_paint_order.is_empty()
    }

    pub fn id(&self, chunk_index: usize) -> ChunkId {
        self.chunks_in_paint_order[chunk_index].id
    }

    pub fn runs_of(&self, chunk_index: usize) -> Range<usize> {
        let start = chunk_index
            .checked_sub(1)
            .map_or(0, |previous| self.chunks_in_paint_order[previous].run_end as usize);
        start..self.chunks_in_paint_order[chunk_index].run_end as usize
    }

    pub fn records_reaching_outside(&self, chunk_index: usize) -> &[TopLevelRecordReachingOutsideItsChunk] {
        let start = chunk_index.checked_sub(1).map_or(0, |previous| {
            self.chunks_in_paint_order[previous].reaching_records_end as usize
        });
        &self.records_reaching_outside_their_chunks
            [start..self.chunks_in_paint_order[chunk_index].reaching_records_end as usize]
    }

    pub fn try_reserve(&mut self, chunk_count: usize, reaching_record_count: usize) -> Result<(), &'static str> {
        self.chunks_in_paint_order
            .try_reserve_exact(chunk_count)
            .and_then(|()| {
                self.records_reaching_outside_their_chunks
                    .try_reserve_exact(reaching_record_count)
            })
            .map_err(|_| "Display list has too many chunks to receive")
    }

    pub fn push_chunk(
        &mut self,
        id: ChunkId,
        first_run_index: u32,
        run_count: usize,
        first_record_offset: u32,
        records_reaching_outside: &[TopLevelRecordReachingOutsideItsChunk],
    ) {
        debug_assert_eq!(
            first_run_index as usize,
            self.chunks_in_paint_order
                .last()
                .map_or(0, |chunk| chunk.run_end as usize)
        );
        self.records_reaching_outside_their_chunks
            .extend(
                records_reaching_outside
                    .iter()
                    .map(|record| TopLevelRecordReachingOutsideItsChunk {
                        record_offset: record.record_offset + first_record_offset,
                        run_index: record.run_index + first_run_index,
                        ..*record
                    }),
            );
        self.chunks_in_paint_order.push(ChunkOfTape {
            id,
            run_end: first_run_index + run_count as u32,
            reaching_records_end: self.records_reaching_outside_their_chunks.len() as u32,
        });
    }

    pub fn of_tape(bytes: &[u8], runs: &[DisplayListCommandRun], chunks: &[DisplayListChunk]) -> Self {
        let mut tape_chunks = Self::default();
        let mut covered_run_count = 0;
        let mut covered_byte_count = 0;
        for (chunk, _, chunk_runs) in chunks_of(bytes, runs, chunks) {
            tape_chunks.push_chunk(
                chunk.id,
                covered_run_count,
                chunk_runs.len(),
                0,
                &top_level_records_reaching_outside_their_chunk(bytes, chunk_runs),
            );
            covered_run_count = chunk.run_end;
            covered_byte_count = chunk.byte_end;
        }
        let uncovered_runs = &runs[covered_run_count as usize..];
        if !uncovered_runs.is_empty() {
            tape_chunks.push_chunk(
                chunk_id(
                    &bytes[covered_byte_count as usize..],
                    uncovered_runs,
                    covered_byte_count,
                ),
                covered_run_count,
                uncovered_runs.len(),
                0,
                &top_level_records_reaching_outside_their_chunk(bytes, uncovered_runs),
            );
        }
        tape_chunks
    }

    #[cfg(test)]
    pub(crate) fn with_a_chunk_per_run(bytes: &[u8], runs: &[DisplayListCommandRun]) -> Self {
        let chunks: Vec<DisplayListChunk> = runs
            .iter()
            .enumerate()
            .map(|(run_index, run)| DisplayListChunk {
                id: chunk_id(
                    &bytes[run.offset as usize..(run.offset + run.size) as usize],
                    std::slice::from_ref(run),
                    run.offset,
                ),
                byte_end: run.offset + run.size,
                run_end: run_index as u32 + 1,
            })
            .collect();
        Self::of_tape(bytes, runs, &chunks)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::{DisplayListBuilder, PendingInlineClip, RecordedDisplayList};
    use crate::display_list::commands::{
        ClipNodeIndex, ContextRef, DisplayListDataSpan, DrawIsolatedGroup, EffectNodeIndex, FillRect, SpatialNodeIndex,
    };
    use libgfx_rust::{Color, CompositingAndBlendingOperator, FloatRect, IntRect, MaskKind};

    fn context(spatial: u32) -> ContextRef {
        ContextRef {
            spatial: SpatialNodeIndex(spatial),
            clip: ClipNodeIndex::NONE,
            effect: EffectNodeIndex::NONE,
        }
    }

    fn fill_rect(y: i32) -> FillRect {
        FillRect {
            rect: IntRect::new(0, y, 10, 10),
            color: Color::default(),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    // A tape of one chunk for each of `rows`, each a rect at that row in one context and one in another.
    fn tape_of_rows(rows: &[i32]) -> RecordedDisplayList {
        let mut builder = DisplayListBuilder::new();
        for &row in rows {
            builder.append(&fill_rect(row), &[], context(1));
            builder.append(&fill_rect(row + 1), &[], context(2));
            builder.end_chunk();
        }
        builder.finish()
    }

    #[test]
    fn a_chunk_is_kept_while_one_of_the_latest_lists_names_it() {
        let id = |value: u64| ChunkId([value, value]);
        let mut kept = KeptChunks::default();
        assert!(kept.take_in(&[id(1), id(2), id(1)]).is_empty());
        assert!(kept.keeps(id(1)) && kept.keeps(id(2)));
        for _ in 1..DISPLAY_LISTS_WHOSE_CHUNKS_ARE_KEPT {
            assert!(kept.take_in(&[id(2), id(3)]).is_empty());
        }
        // The list that named the first chunk twice leaves, and the one it named alone goes with it.
        assert_eq!(kept.take_in(&[id(3)]), vec![id(1)]);
        assert!(!kept.keeps(id(1)) && kept.keeps(id(2)) && kept.keeps(id(3)));
        for _ in 1..DISPLAY_LISTS_WHOSE_CHUNKS_ARE_KEPT {
            kept.take_in(&[id(3)]);
        }
        assert!(!kept.keeps(id(2)));
    }

    #[test]
    fn a_chunk_is_named_by_its_records_and_runs_wherever_it_is_in_a_tape() {
        let tape = tape_of_rows(&[10, 20, 10]);
        assert_eq!(tape.chunks.len(), 3);
        assert_eq!(tape.chunks[0].id, tape.chunks[2].id);
        assert_ne!(tape.chunks[0].id, tape.chunks[1].id);
        assert_ne!(tape.chunks[0].id, ChunkId::UNSET);

        // The same records in another visual context are another chunk.
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(10), &[], context(1));
        builder.append(&fill_rect(11), &[], context(3));
        assert_ne!(builder.finish().chunks[0].id, tape.chunks[0].id);
    }

    fn compositor_scrollbar(is_painted_by_compositor: bool) -> CompositorScrollbar {
        CompositorScrollbar {
            scroll_node_index: SpatialNodeIndex(1),
            gutter_rect: IntRect::new(90, 0, 10, 100),
            thumb_rect: IntRect::new(90, 0, 10, 20),
            track_rect: IntRect::new(90, 0, 10, 100),
            expanded_gutter_rect: IntRect::new(86, 0, 14, 100),
            expanded_thumb_rect: IntRect::new(86, 0, 14, 20),
            scroll_size: 0.5,
            expanded_scroll_size: 0.5,
            min_scroll_offset: 0.0,
            max_scroll_offset: 100.0,
            thumb_color: Color::default(),
            thumb_outline_color: Color::default(),
            track_color: Color::default(),
            vertical: true,
            is_painted_by_compositor,
        }
    }

    fn declare_mask_content(builder: &mut DisplayListBuilder, effect: EffectNodeIndex, context: ContextRef) {
        let declaration = builder.begin_group::<DeclareMaskContent>(Vec::new());
        builder.append(&fill_rect(0), &[], context);
        let content = builder.group_content_span(&declaration);
        builder.finish_group(
            declaration,
            &DeclareMaskContent {
                rect: IntRect::new(0, 0, 10, 10),
                effect,
                content,
            },
            context,
        );
    }

    #[test]
    fn records_that_reach_outside_their_chunk_are_found_where_they_are_or_where_they_are_nested() {
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0), &[], context(1));
        let paint_scrollbar_offset = builder.byte_size() as u32;
        builder.append(
            &PaintScrollBar {
                scroll_node_index: SpatialNodeIndex(5),
                gutter_rect: IntRect::new(90, 0, 10, 100),
                thumb_rect: IntRect::new(90, 0, 10, 20),
                track_rect: IntRect::new(90, 0, 10, 100),
                expanded_gutter_rect: IntRect::new(86, 0, 14, 100),
                scroll_size: 0.5,
                thumb_color: Color::default(),
                thumb_outline_color: Color::default(),
                track_color: Color::default(),
                vertical: true,
            },
            &[],
            context(1),
        );
        let compositor_scrollbar_offset = builder.byte_size() as u32;
        builder.append(&compositor_scrollbar(true), &[], context(1));
        builder.append(&compositor_scrollbar(false), &[], context(1));
        let own_space_clip_offset = builder.byte_size() as u32;
        let clip = PendingInlineClip::intersecting_float_rect(FloatRect::new(0.0, 0.0, 5.0, 5.0));
        builder.append_with_inline_clips(&fill_rect(20), &[], context(2), &[clip.in_space(SpatialNodeIndex(4))]);
        builder.append_with_inline_clips(&fill_rect(30), &[], context(2), &[clip]);
        let mask_offset = builder.byte_size() as u32;
        declare_mask_content(&mut builder, EffectNodeIndex(1), context(2));
        let group_offset = builder.byte_size() as u32;
        let group = builder.begin_group::<DrawIsolatedGroup>(Vec::new());
        declare_mask_content(&mut builder, EffectNodeIndex(2), context(2));
        let content = builder.group_content_span(&group);
        builder.finish_group(
            group,
            &DrawIsolatedGroup {
                clip_rect: FloatRect::new(0.0, 0.0, 10.0, 10.0),
                content,
                mask: DisplayListDataSpan::default(),
                filter: DisplayListDataSpan::default(),
                opacity: 1.0,
                compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
                mask_kind: MaskKind::Alpha,
            },
            context(2),
        );
        let chunk = builder.take_chunk();

        let record = |record_offset, run_index, reach| TopLevelRecordReachingOutsideItsChunk {
            record_offset,
            run_index,
            reach,
        };
        assert_eq!(
            top_level_records_reaching_outside_their_chunk(&chunk.records, &chunk.runs),
            [
                record(
                    paint_scrollbar_offset,
                    0,
                    ReachOutsideChunk::ScrollbarThumbFollowsScrollOffsetOf(SpatialNodeIndex(5))
                ),
                record(
                    compositor_scrollbar_offset,
                    0,
                    ReachOutsideChunk::ScrollbarPaintedByCompositor
                ),
                record(own_space_clip_offset, 1, ReachOutsideChunk::InlineClipInItsOwnSpace),
                record(
                    mask_offset,
                    1,
                    ReachOutsideChunk::DeclaresMaskContentOf(EffectNodeIndex(1))
                ),
                record(
                    group_offset,
                    1,
                    ReachOutsideChunk::DeclaresMaskContentOf(EffectNodeIndex(2))
                ),
            ]
        );
    }
}
