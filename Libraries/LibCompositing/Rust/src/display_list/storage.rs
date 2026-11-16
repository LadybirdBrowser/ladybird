/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tape of one display list: a recording this process made, with the visual context tree it was recorded against,
//! or a tape that came from another process and passed validation.

use super::builder::RecordedDisplayList;
use super::chunks::{
    Chunk, ChunkId, TapeChunks, TopLevelRecordReachingOutsideItsChunk, chunk_id,
    top_level_records_reaching_outside_their_chunk,
};
use super::commands::{ContextRef, DisplayListCommandRun};
use super::effect_clip_plan::EffectClipPlan;
use super::summary::{DisplayListSummary, summarize};
use super::validate::validate_tape;
use crate::visual_context::VisualContextTree;
use std::sync::{Arc, OnceLock};

// Tape bytes in memory aligned for every command struct, which a Vec<u8> does not promise.
struct AlignedBytes {
    words: Vec<u64>,
    size: usize,
}

impl AlignedBytes {
    fn try_zeroed(size: usize) -> Option<Self> {
        let mut words = Vec::new();
        words.try_reserve_exact(size.div_ceil(8)).ok()?;
        words.resize(size.div_ceil(8), 0);
        Some(Self { words, size })
    }

    fn as_slice(&self) -> &[u8] {
        // SAFETY: The words hold at least `size` initialized bytes.
        unsafe { ak::slice_from_raw_parts(self.words.as_ptr().cast(), self.size) }
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        // SAFETY: The words hold at least `size` initialized bytes, which this borrows uniquely.
        unsafe { ak::slice_from_raw_parts_mut(self.words.as_mut_ptr().cast(), self.size) }
    }
}

enum Tape {
    Empty,
    Recorded(Arc<RecordedDisplayList>),
    Received {
        bytes: AlignedBytes,
        command_runs: Vec<DisplayListCommandRun>,
    },
}

pub struct DisplayListStorage {
    tape: Tape,
    // The tree a list this process recorded was recorded against, which it replays against and goes
    // to the compositor with. A tape from another process comes without one.
    visual_context_tree: Option<Arc<VisualContextTree>>,
    // The placement of the runs' clips and effects. The list is replayed against trees of one structural epoch only, so
    // one plan serves every replay.
    effect_clip_plan: OnceLock<EffectClipPlan>,
    summary: OnceLock<DisplayListSummary>,
    chunks: OnceLock<TapeChunks>,
}

impl DisplayListStorage {
    fn new(tape: Tape, visual_context_tree: Option<Arc<VisualContextTree>>, chunks: OnceLock<TapeChunks>) -> Arc<Self> {
        Arc::new(Self {
            tape,
            visual_context_tree,
            effect_clip_plan: OnceLock::new(),
            summary: OnceLock::new(),
            chunks,
        })
    }

    pub fn bytes(&self) -> &[u8] {
        match &self.tape {
            Tape::Empty => &[],
            Tape::Recorded(recorded) => &recorded.bytes,
            Tape::Received { bytes, .. } => bytes.as_slice(),
        }
    }

    pub fn command_runs(&self) -> &[DisplayListCommandRun] {
        match &self.tape {
            Tape::Empty => &[],
            Tape::Recorded(recorded) => &recorded.command_runs,
            Tape::Received { command_runs, .. } => command_runs,
        }
    }

    pub fn visual_context_tree(&self) -> Option<&Arc<VisualContextTree>> {
        self.visual_context_tree.as_ref()
    }

    pub fn summary(&self) -> &DisplayListSummary {
        self.summary
            .get_or_init(|| summarize(self.bytes(), self.command_runs()))
    }

    pub fn effect_clip_plan(&self, tree: &VisualContextTree) -> &EffectClipPlan {
        self.effect_clip_plan.get_or_init(|| {
            EffectClipPlan::new(tree, self.command_runs()).expect("a display list's runs name live nodes of its tree")
        })
    }

    pub fn chunks(&self) -> &TapeChunks {
        self.chunks.get_or_init(|| {
            let recorded_chunks = match &self.tape {
                Tape::Recorded(recorded) => &recorded.chunks[..],
                Tape::Empty | Tape::Received { .. } => &[],
            };
            TapeChunks::of_tape(self.bytes(), self.command_runs(), recorded_chunks)
        })
    }
}

impl DisplayListStorage {
    /// A list this process recorded against `visual_context_tree`.
    pub fn recorded(recorded: Arc<RecordedDisplayList>, visual_context_tree: Arc<VisualContextTree>) -> Arc<Self> {
        Self::new(Tape::Recorded(recorded), Some(visual_context_tree), OnceLock::new())
    }

    /// An empty list, recorded against a tree with no node but the visual viewport.
    pub fn empty() -> Arc<Self> {
        Self::new(
            Tape::Empty,
            Some(Arc::new(VisualContextTree::create_identity())),
            OnceLock::new(),
        )
    }
}

// Writes a run field by field, so its padding is written too.
pub(super) fn write_run(run: &DisplayListCommandRun, out: &mut [u8]) {
    use std::mem::offset_of;
    out.fill(0);
    let mut put = |offset: usize, bytes: &[u8]| out[offset..offset + bytes.len()].copy_from_slice(bytes);
    let context = offset_of!(DisplayListCommandRun, context);
    let ink_bounds = offset_of!(DisplayListCommandRun, ink_bounds);
    put(offset_of!(DisplayListCommandRun, offset), &run.offset.to_ne_bytes());
    put(offset_of!(DisplayListCommandRun, size), &run.size.to_ne_bytes());
    put(
        context + offset_of!(ContextRef, spatial),
        &run.context.spatial.0.to_ne_bytes(),
    );
    put(
        context + offset_of!(ContextRef, clip),
        &run.context.clip.0.to_ne_bytes(),
    );
    put(
        context + offset_of!(ContextRef, effect),
        &run.context.effect.0.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, x),
        &run.ink_bounds.x.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, y),
        &run.ink_bounds.y.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, width),
        &run.ink_bounds.width.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, height),
        &run.ink_bounds.height.to_ne_bytes(),
    );
    put(
        offset_of!(DisplayListCommandRun, has_compositor_metadata),
        &[u8::from(run.has_compositor_metadata)],
    );
}

/// The size of one chunk packed for transport: its records, and then its runs.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct PackedChunkSize {
    pub record_bytes: u64,
    pub run_count: u64,
}

impl PackedChunkSize {
    pub fn of(chunk: &Chunk) -> Self {
        Self {
            record_bytes: chunk.records.len() as u64,
            run_count: chunk.runs.len() as u64,
        }
    }

    fn in_bytes(self) -> Option<(usize, usize)> {
        let record_bytes = usize::try_from(self.record_bytes).ok()?;
        let run_bytes = usize::try_from(self.run_count)
            .ok()?
            .checked_mul(std::mem::size_of::<DisplayListCommandRun>())?;
        Some((record_bytes, run_bytes))
    }
}

/// The size of `chunks` packed for transport one after another.
pub fn packed_size_of_chunks(chunks: &[&Chunk]) -> usize {
    chunks
        .iter()
        .map(|chunk| chunk.records.len() + chunk.runs.len() * std::mem::size_of::<DisplayListCommandRun>())
        .sum()
}

/// Writes `chunks` packed for transport one after another to `destination`, which is
/// `packed_size_of_chunks(chunks)` bytes long.
pub fn write_packed_chunks(chunks: &[&Chunk], destination: &mut [u8]) {
    assert_eq!(destination.len(), packed_size_of_chunks(chunks));
    let mut rest = destination;
    for chunk in chunks {
        let (records, after_records) = rest.split_at_mut(chunk.records.len());
        records.copy_from_slice(&chunk.records);
        let (runs, after_runs) =
            after_records.split_at_mut(chunk.runs.len() * std::mem::size_of::<DisplayListCommandRun>());
        let (run_destinations, _) = runs.as_chunks_mut::<{ std::mem::size_of::<DisplayListCommandRun>() }>();
        for (run, out) in chunk.runs.iter().zip(run_destinations) {
            write_run(run, out);
        }
        rest = after_runs;
    }
}

/// A chunk another process sent that passed validation, named by what it holds: its records in memory aligned for
/// every command struct, and its runs.
pub struct ReceivedChunk {
    pub id: ChunkId,
    records: AlignedBytes,
    runs: Vec<DisplayListCommandRun>,
    records_reaching_outside: Vec<TopLevelRecordReachingOutsideItsChunk>,
}

impl ReceivedChunk {
    pub fn records(&self) -> &[u8] {
        self.records.as_slice()
    }

    pub fn runs(&self) -> &[DisplayListCommandRun] {
        &self.runs
    }

    pub fn records_reaching_outside(&self) -> &[TopLevelRecordReachingOutsideItsChunk] {
        &self.records_reaching_outside
    }
}

/// Copies the chunks packed one after another in the `packed_size_in_bytes` bytes at `packed`, whose sizes `sizes` lists
/// in order, out of memory that another process can still write to, checks each copy and names it by what it holds.
///
/// # Safety
/// `packed` must address `packed_size_in_bytes` readable bytes, or be null with a size of zero.
pub unsafe fn receive_packed_chunks_from_shared_memory(
    packed: *const u8,
    packed_size_in_bytes: usize,
    sizes: &[PackedChunkSize],
) -> Result<Vec<ReceivedChunk>, &'static str> {
    let mut chunks = Vec::new();
    chunks
        .try_reserve_exact(sizes.len())
        .map_err(|_| "Display list has too many chunks to receive")?;
    let mut offset = 0usize;
    for size in sizes {
        let (record_bytes, run_bytes) = size.in_bytes().ok_or("Display list chunk sizes overflow")?;
        let end = offset
            .checked_add(record_bytes)
            .and_then(|end| end.checked_add(run_bytes))
            .ok_or("Display list chunk sizes overflow")?;
        if end > packed_size_in_bytes {
            return Err("Display list chunks exceed the buffer that holds them");
        }
        // Every run holds at least one record, so more runs than headers cannot describe the chunk.
        if run_bytes / std::mem::size_of::<DisplayListCommandRun>() > record_bytes / super::builder::HEADER_SIZE {
            return Err("Display list chunk has more runs than its records allow");
        }
        let mut records = AlignedBytes::try_zeroed(record_bytes).ok_or("Display list chunk is too large to receive")?;
        let mut packed_runs = Vec::new();
        packed_runs
            .try_reserve_exact(run_bytes)
            .map_err(|_| "Display list chunk is too large to receive")?;
        packed_runs.resize(run_bytes, 0);
        if end > offset {
            // SAFETY: The caller guarantees `packed` addresses `packed_size_in_bytes` bytes, which hold `offset..end`,
            // and both copies have room for their part.
            unsafe {
                std::ptr::copy_nonoverlapping(packed.add(offset), records.as_mut_slice().as_mut_ptr(), record_bytes);
                std::ptr::copy_nonoverlapping(packed.add(offset + record_bytes), packed_runs.as_mut_ptr(), run_bytes);
            }
        }
        let runs = validate_tape(records.as_slice(), &packed_runs)?;
        chunks.push(ReceivedChunk {
            id: chunk_id(records.as_slice(), &runs, 0),
            records_reaching_outside: top_level_records_reaching_outside_their_chunk(records.as_slice(), &runs),
            records,
            runs,
        });
        offset = end;
    }
    if offset != packed_size_in_bytes {
        return Err("Display list chunks do not fill the buffer that holds them");
    }
    Ok(chunks)
}

/// Copies the chunks packed one after another in `packed`, whose sizes `sizes` lists in order, out, checks each copy and
/// names it by what it holds.
pub fn receive_packed_chunks(packed: &[u8], sizes: &[PackedChunkSize]) -> Result<Vec<ReceivedChunk>, &'static str> {
    // SAFETY: The slice addresses its bytes.
    unsafe { receive_packed_chunks_from_shared_memory(packed.as_ptr(), packed.len(), sizes) }
}

impl DisplayListStorage {
    /// The tape of `chunks`, which another process sent, one after another.
    pub fn of_received_chunks(chunks: &[Arc<ReceivedChunk>]) -> Result<Arc<Self>, &'static str> {
        let tape_size = chunks.iter().map(|chunk| chunk.records().len()).sum::<usize>();
        if u32::try_from(tape_size).is_err() {
            return Err("Display list exceeds the size of a tape");
        }
        let mut bytes = AlignedBytes::try_zeroed(tape_size).ok_or("Display list is too large to receive")?;
        let mut command_runs = Vec::new();
        command_runs
            .try_reserve_exact(chunks.iter().map(|chunk| chunk.runs().len()).sum())
            .map_err(|_| "Display list is too large to receive")?;
        let mut tape_chunks = TapeChunks::default();
        tape_chunks.try_reserve(
            chunks.len(),
            chunks.iter().map(|chunk| chunk.records_reaching_outside().len()).sum(),
        )?;
        let mut chunk_start = 0;
        for chunk in chunks {
            let records = chunk.records();
            bytes.as_mut_slice()[chunk_start..chunk_start + records.len()].copy_from_slice(records);
            tape_chunks.push_chunk(
                chunk.id,
                command_runs.len() as u32,
                chunk.runs().len(),
                chunk_start as u32,
                chunk.records_reaching_outside(),
            );
            command_runs.extend(chunk.runs().iter().map(|run| DisplayListCommandRun {
                offset: run.offset + chunk_start as u32,
                ..*run
            }));
            chunk_start += records.len();
        }
        Ok(DisplayListStorage::new(
            Tape::Received { bytes, command_runs },
            None,
            OnceLock::from(tape_chunks),
        ))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn chunks_packed_for_transport_come_back_as_the_tape_they_make() {
        use super::super::builder::DisplayListBuilder;
        use super::super::chunks::ReachOutsideChunk;
        use super::super::commands::{ClipNodeIndex, EffectNodeIndex, FillRect, PaintScrollBar, SpatialNodeIndex};
        use libgfx_rust::{Color, CompositingAndBlendingOperator, IntRect};
        let context = |spatial| ContextRef {
            spatial: SpatialNodeIndex(spatial),
            clip: ClipNodeIndex::NONE,
            effect: EffectNodeIndex::NONE,
        };
        let fill_rect = |y| FillRect {
            rect: IntRect::new(0, y, 10, 10),
            color: Color::default(),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        };
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0), &[], context(1));
        let first = builder.take_chunk();
        builder.append(&fill_rect(10), &[], context(1));
        builder.append(&fill_rect(20), &[], context(2));
        let scrollbar_offset_in_second_chunk = builder.byte_size() as u32;
        builder.append(
            &PaintScrollBar {
                scroll_node_index: SpatialNodeIndex(3),
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
            context(2),
        );
        let second = builder.take_chunk();
        let chunks = [&first, &second];
        let mut packed = vec![0; packed_size_of_chunks(&chunks)];
        write_packed_chunks(&chunks, &mut packed);
        let sizes: Vec<PackedChunkSize> = chunks.iter().map(|chunk| PackedChunkSize::of(chunk)).collect();

        let received = receive_packed_chunks(&packed, &sizes).unwrap();
        for (received, sent) in received.iter().zip(chunks) {
            assert_eq!(received.id, sent.id);
            assert_eq!(received.records(), &sent.records[..]);
            assert_eq!(received.runs(), &sent.runs[..]);
        }
        let received: Vec<Arc<ReceivedChunk>> = received.into_iter().map(Arc::new).collect();
        let storage = DisplayListStorage::of_received_chunks(&received).unwrap();
        let tape = RecordedDisplayList::of_chunks(chunks);
        assert_eq!(storage.bytes(), &tape.bytes[..]);
        assert_eq!(storage.command_runs(), &tape.command_runs[..]);

        let tape_chunks = storage.chunks();
        assert_eq!(
            tape_chunks,
            &TapeChunks::of_tape(&tape.bytes, &tape.command_runs, &tape.chunks)
        );
        assert_eq!(
            (tape_chunks.len(), tape_chunks.id(0), tape_chunks.id(1)),
            (2, first.id, second.id)
        );
        assert_eq!((tape_chunks.runs_of(0), tape_chunks.runs_of(1)), (0..1, 1..3));
        assert!(tape_chunks.records_reaching_outside(0).is_empty());
        assert_eq!(
            tape_chunks.records_reaching_outside(1),
            [TopLevelRecordReachingOutsideItsChunk {
                record_offset: first.records.len() as u32 + scrollbar_offset_in_second_chunk,
                run_index: 2,
                reach: ReachOutsideChunk::ScrollbarThumbFollowsScrollOffsetOf(SpatialNodeIndex(3)),
            }]
        );

        // Sizes that leave bytes over or claim more than the buffer holds are refused.
        assert!(receive_packed_chunks(&packed, &sizes[..1]).is_err());
        assert!(receive_packed_chunks(&packed[..packed.len() - 8], &sizes).is_err());
    }

    #[test]
    fn published_bytes_survive_the_recorder_and_can_be_released_on_another_thread() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<RecordedDisplayList>();
        assert_send_sync::<DisplayListStorage>();
        let mut recorded = Arc::new(RecordedDisplayList {
            bytes: vec![1, 2, 3],
            ..Default::default()
        });
        let storage = DisplayListStorage::recorded(recorded.clone(), Arc::new(VisualContextTree::create_identity()));
        Arc::make_mut(&mut recorded).bytes[0] = 9;
        assert_eq!(storage.bytes(), &[1, 2, 3]);
        drop(recorded);
        std::thread::spawn(move || {
            assert_eq!(storage.bytes(), &[1, 2, 3]);
            drop(storage);
        })
        .join()
        .unwrap();
    }
}
