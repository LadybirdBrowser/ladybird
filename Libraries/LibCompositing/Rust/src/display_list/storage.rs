/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A display list as the chunks it is cut into, in paint order: chunks another process sent that passed validation, or
//! the chunks of a recording this process made, with the visual context tree it was recorded against.

use super::builder::{COMMAND_ALIGNMENT, HEADER_SIZE, RecordedDisplayList, for_each_command, read_header};
use super::chunks::{
    Chunk, ChunkId, DisplayListChunk, TopLevelRecordReachingOutsideRunContext, chunk_id, chunks_of,
    top_level_records_reaching_outside_run_context,
};
use super::commands::{ContextRef, DisplayListCommandHeader, DisplayListCommandRun, DisplayListCommandType};
use super::effect_clip_plan::EffectClipPlan;
use super::placement::RunGroups;
use super::summary::{DisplayListSummary, summarize, summary_of_pieces};
use super::tile_keys::{ChunkPlacedInRasterSpace, ContentHash, RecordTable};
use super::validate::validate_tape;
use crate::visual_context::VisualContextTree;
use libipc_rust::wire::{Decode, Encode};
use std::sync::{Arc, Mutex, OnceLock, PoisonError};

// Room for the spaces a chunk shows in and those of a moment before, which a scroll that comes back needs again.
const PLACEMENTS_CACHED_PER_CHUNK: usize = 4;

// Backed by u64 words, as a Vec<u8> does not promise the alignment command structs need.
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

    fn copied(bytes: &[u8]) -> Self {
        let mut copy = Self::try_zeroed(bytes.len()).expect("a recorded chunk fits in memory");
        copy.as_mut_slice().copy_from_slice(bytes);
        copy
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

/// The index of a chunk among the chunks of a list, in paint order.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ChunkIndexInList(pub u32);

/// The index of a run among the runs of a list, in paint order.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RunIndexInList(pub u32);

/// The index of a run among the runs of the chunk that holds it.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RunIndexInChunk(pub u32);

/// Where a record is in a display list: the chunk that holds it and its offset in that chunk. Records are painted in
/// the order of their positions.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct ListPosition {
    pub chunk_index: ChunkIndexInList,
    pub offset: u32,
}

impl ListPosition {
    pub const fn new(chunk_index: ChunkIndexInList, offset: u32) -> Self {
        Self { chunk_index, offset }
    }
}

/// The records of a display list from `begin` up to `end`, which can lie in a later chunk than `begin`. Every record of
/// the chunks between the two is in the span.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct ListSpan {
    pub begin: ListPosition,
    pub end: ListPosition,
}

impl ListSpan {
    pub fn contains(&self, position: ListPosition) -> bool {
        self.begin <= position && position < self.end
    }
}

enum RecordsAndRuns {
    Copied {
        records: AlignedBytes,
        runs: Vec<DisplayListCommandRun>,
    },
    // Only where the records start on a boundary that every command struct may start on.
    SharedWith(Arc<Chunk>),
}

/// A chunk that display lists hold, named by what it holds: its records in memory aligned for every command struct,
/// and its runs, which name the records by their offsets from where the chunk starts. Every list that names a chunk
/// shares it, and what is known of the chunk alone is worked out once, for all of them.
pub struct ListChunk {
    pub id: ChunkId,
    records_and_runs: RecordsAndRuns,
    records_reaching_outside_run_context: OnceLock<Vec<TopLevelRecordReachingOutsideRunContext>>,
    summary: OnceLock<DisplayListSummary>,
    record_table: OnceLock<RecordTable>,
    cached_placements_least_recently_used_first: Mutex<Vec<Arc<ChunkPlacedInRasterSpace>>>,
}

impl ListChunk {
    fn new(id: ChunkId, records_and_runs: RecordsAndRuns) -> Self {
        Self {
            id,
            records_and_runs,
            records_reaching_outside_run_context: OnceLock::new(),
            summary: OnceLock::new(),
            record_table: OnceLock::new(),
            cached_placements_least_recently_used_first: Mutex::new(Vec::new()),
        }
    }

    fn named_copy(records: AlignedBytes, runs: Vec<DisplayListCommandRun>) -> Self {
        Self::new(
            chunk_id(records.as_slice(), &runs, 0),
            RecordsAndRuns::Copied { records, runs },
        )
    }

    /// The chunk of a copy of `records`, which `runs` name by their offsets from `chunk_start`.
    pub fn copied(records: &[u8], runs: &[DisplayListCommandRun], chunk_start: u32) -> Self {
        Self::named_copy(
            AlignedBytes::copied(records),
            runs.iter()
                .map(|run| DisplayListCommandRun {
                    offset: run.offset - chunk_start,
                    ..*run
                })
                .collect(),
        )
    }

    /// The chunk of `chunk`, which this process made, with its id. It shares the records and runs of `chunk`, or holds
    /// a copy of them where the records do not start on a boundary that every command struct may start on.
    pub fn of_chunk_made_in_this_process(chunk: &Arc<Chunk>) -> Self {
        if !chunk.records.as_ptr().addr().is_multiple_of(COMMAND_ALIGNMENT) {
            return Self::new(
                chunk.id,
                RecordsAndRuns::Copied {
                    records: AlignedBytes::copied(&chunk.records),
                    runs: chunk.runs.clone(),
                },
            );
        }
        Self::new(chunk.id, RecordsAndRuns::SharedWith(chunk.clone()))
    }

    pub fn records(&self) -> &[u8] {
        match &self.records_and_runs {
            RecordsAndRuns::Copied { records, .. } => records.as_slice(),
            RecordsAndRuns::SharedWith(chunk) => &chunk.records,
        }
    }

    pub fn runs(&self) -> &[DisplayListCommandRun] {
        match &self.records_and_runs {
            RecordsAndRuns::Copied { runs, .. } => runs,
            RecordsAndRuns::SharedWith(chunk) => &chunk.runs,
        }
    }

    pub fn run_starting_at(&self, offset: u32) -> Option<&DisplayListCommandRun> {
        let runs = self.runs();
        runs.get(runs.partition_point(|run| run.offset < offset))
            .filter(|run| run.offset == offset)
    }

    /// The index of the first run whose records end past `offset`, which is the run that holds the record at `offset`
    /// where a run holds it.
    pub fn index_of_first_run_ending_after(&self, offset: u32) -> usize {
        self.runs()
            .partition_point(|run| run.offset.saturating_add(run.size) <= offset)
    }

    pub fn records_reaching_outside_run_context(&self) -> &[TopLevelRecordReachingOutsideRunContext] {
        self.records_reaching_outside_run_context
            .get_or_init(|| top_level_records_reaching_outside_run_context(self.records(), self.runs()))
    }

    pub fn summary(&self) -> &DisplayListSummary {
        self.summary.get_or_init(|| summarize(self.records(), self.runs()))
    }

    pub fn record_table(&self) -> &RecordTable {
        self.record_table
            .get_or_init(|| RecordTable::new(self.records(), self.runs()))
    }

    /// The cached placement of the chunk's records whose inputs hash to `placement_inputs_hash`, or else the one
    /// `place` makes, and whether it was made now.
    pub(crate) fn find_or_make_placement(
        &self,
        placement_inputs_hash: ContentHash,
        place: impl FnOnce() -> ChunkPlacedInRasterSpace,
    ) -> (Arc<ChunkPlacedInRasterSpace>, bool) {
        let mut placements = self
            .cached_placements_least_recently_used_first
            .lock()
            .unwrap_or_else(PoisonError::into_inner);
        if let Some(index) = placements
            .iter()
            .position(|placement| placement.placement_inputs_hash() == placement_inputs_hash)
        {
            let placement = placements.remove(index);
            placements.push(placement.clone());
            return (placement, false);
        }
        let placement = Arc::new(place());
        if placements.len() == PLACEMENTS_CACHED_PER_CHUNK {
            placements.remove(0);
        }
        placements.push(placement.clone());
        (placement, true)
    }

    /// The header and the payload of the record at `offset`.
    pub fn record_at(&self, offset: u32) -> (DisplayListCommandHeader, &[u8]) {
        let records = self.records();
        let offset = offset as usize;
        let header = read_header(&records[offset..]);
        let payload_start = offset + HEADER_SIZE;
        (
            header,
            &records[payload_start..payload_start + header.payload_size as usize],
        )
    }
}

pub struct DisplayListStorage {
    chunks_in_paint_order: Vec<Arc<ListChunk>>,
    first_run_index_of_each_chunk_then_run_count: Vec<u32>,
    tree_if_recorded_in_this_process: Option<Arc<VisualContextTree>>,
    // Lists replay against trees of one structural epoch only, so one plan serves every replay.
    effect_clip_plan: OnceLock<EffectClipPlan>,
    // One epoch fixes the tree structure run groups follow, but transforms decide which 3D sorting contexts sort their
    // planes by depth. The samples of a tree sort the same contexts unless a transform animation animates a plane of
    // one, so the groups are kept with the copy of the tree without the samples of the compositor's animations that
    // they were found to hold for, and found to hold again only for the samples of such an animation.
    cached_run_groups: Mutex<Option<(u64, Arc<RunGroups>)>>,
    summary: OnceLock<DisplayListSummary>,
}

impl DisplayListStorage {
    fn new(
        chunks_in_paint_order: Vec<Arc<ListChunk>>,
        tree_if_recorded_in_this_process: Option<Arc<VisualContextTree>>,
    ) -> Arc<Self> {
        let mut first_run_index_of_each_chunk_then_run_count = Vec::with_capacity(chunks_in_paint_order.len() + 1);
        let mut run_count = 0u32;
        for chunk in &chunks_in_paint_order {
            first_run_index_of_each_chunk_then_run_count.push(run_count);
            run_count += chunk.runs().len() as u32;
        }
        first_run_index_of_each_chunk_then_run_count.push(run_count);
        Arc::new(Self {
            chunks_in_paint_order,
            first_run_index_of_each_chunk_then_run_count,
            tree_if_recorded_in_this_process,
            effect_clip_plan: OnceLock::new(),
            cached_run_groups: Mutex::new(None),
            summary: OnceLock::new(),
        })
    }

    /// The list of `chunks`, which another process sent, in paint order. A list may name one chunk many times, and
    /// what is worked out of a list, such as its summary, holds what is worked out of each chunk once per time the list
    /// names it, so the records of the chunks, counted so, are limited as a list was limited when it was one tape.
    pub fn of_chunks(chunks: Vec<Arc<ListChunk>>) -> Result<Arc<Self>, &'static str> {
        let run_count = chunks.iter().map(|chunk| chunk.runs().len()).sum::<usize>();
        if u32::try_from(chunks.len()).is_err() || u32::try_from(run_count).is_err() {
            return Err("Display list has more chunks or runs than a list can hold");
        }
        let record_byte_count = chunks.iter().fold(0u64, |byte_count, chunk| {
            byte_count.saturating_add(chunk.records().len() as u64)
        });
        if record_byte_count > u64::from(u32::MAX) {
            return Err("Display list holds more bytes of records than a list can hold");
        }
        Ok(Self::new(chunks, None))
    }

    /// A list this process recorded against `visual_context_tree`.
    pub fn recorded(recorded: &RecordedDisplayList, visual_context_tree: Arc<VisualContextTree>) -> Arc<Self> {
        Self::new(
            chunks_of_recording(&recorded.bytes, &recorded.command_runs, &recorded.chunks),
            Some(visual_context_tree),
        )
    }

    /// An empty list, recorded against a tree with no node but the visual viewport.
    pub fn empty() -> Arc<Self> {
        Self::new(Vec::new(), Some(Arc::new(VisualContextTree::create_identity())))
    }

    /// The list of what `builder` recorded so far, cut into the chunks it ended, as tests build lists.
    #[cfg(test)]
    pub(crate) fn of_builder(builder: &super::builder::DisplayListBuilder) -> Arc<Self> {
        Self::new(
            chunks_of_recording(builder.bytes(), builder.command_runs(), builder.chunks()),
            None,
        )
    }

    pub fn chunks(&self) -> &[Arc<ListChunk>] {
        &self.chunks_in_paint_order
    }

    pub fn chunk(&self, index: ChunkIndexInList) -> &ListChunk {
        &self.chunks_in_paint_order[index.0 as usize]
    }

    /// Whether `other` holds chunks of the same ids in the same order.
    pub fn holds_same_chunks_in_same_order_as(&self, other: &Self) -> bool {
        self.chunks_in_paint_order.len() == other.chunks_in_paint_order.len()
            && self
                .chunks_in_paint_order
                .iter()
                .zip(&other.chunks_in_paint_order)
                .all(|(chunk, other_chunk)| chunk.id == other_chunk.id)
    }

    /// The tree a list this process recorded was recorded against, which it replays against and goes to the compositor
    /// with. A list from another process comes without one.
    pub fn visual_context_tree(&self) -> Option<&Arc<VisualContextTree>> {
        self.tree_if_recorded_in_this_process.as_ref()
    }

    pub fn byte_size(&self) -> usize {
        self.chunks_in_paint_order
            .iter()
            .map(|chunk| chunk.records().len())
            .sum()
    }

    pub fn run_count(&self) -> usize {
        *self
            .first_run_index_of_each_chunk_then_run_count
            .last()
            .expect("the run counts end with the total") as usize
    }

    /// The runs of the list in paint order.
    pub fn runs(&self) -> impl Iterator<Item = &DisplayListCommandRun> + Clone {
        self.chunks_in_paint_order.iter().flat_map(|chunk| chunk.runs().iter())
    }

    /// The index among the runs of the list of the first run of the chunk at `chunk_index`.
    pub fn first_run_of_chunk(&self, chunk_index: ChunkIndexInList) -> usize {
        self.first_run_index_of_each_chunk_then_run_count[chunk_index.0 as usize] as usize
    }

    /// The chunk that holds the run at `run_index` among the runs of the list, and the run's index in the chunk.
    pub fn chunk_of_run(&self, run_index: usize) -> (ChunkIndexInList, usize) {
        let chunk_index = self
            .first_run_index_of_each_chunk_then_run_count
            .partition_point(|&first_run_index| first_run_index as usize <= run_index)
            - 1;
        (
            ChunkIndexInList(chunk_index as u32),
            run_index - self.first_run_index_of_each_chunk_then_run_count[chunk_index] as usize,
        )
    }

    pub fn run(&self, run_index: usize) -> &DisplayListCommandRun {
        let (chunk_index, run_index_in_chunk) = self.chunk_of_run(run_index);
        &self.chunk(chunk_index).runs()[run_index_in_chunk]
    }

    /// The records of the run at `run_index`.
    pub fn span_of_run(&self, run_index: usize) -> ListSpan {
        let (chunk_index, run_index_in_chunk) = self.chunk_of_run(run_index);
        let run = &self.chunk(chunk_index).runs()[run_index_in_chunk];
        ListSpan {
            begin: ListPosition::new(chunk_index, run.offset),
            end: ListPosition::new(chunk_index, run.offset + run.size),
        }
    }

    /// The span of every record of the list.
    pub fn whole_span(&self) -> ListSpan {
        let end = match self.chunks_in_paint_order.last() {
            Some(last) => ListPosition::new(
                ChunkIndexInList(self.chunks_in_paint_order.len() as u32 - 1),
                last.records().len() as u32,
            ),
            None => ListPosition::default(),
        };
        ListSpan {
            begin: ListPosition::default(),
            end,
        }
    }

    /// The chunks that `span` reaches into, each with the offsets of the part of it in the span.
    pub fn parts_of_span(&self, span: ListSpan) -> impl Iterator<Item = (ChunkIndexInList, u32, u32)> + '_ {
        let first_chunk_index = span.begin.chunk_index.0;
        let end_chunk_index = (span.end.chunk_index.0 as usize + 1).min(self.chunks_in_paint_order.len()) as u32;
        (first_chunk_index..end_chunk_index.max(first_chunk_index)).filter_map(move |chunk_index| {
            let chunk_index = ChunkIndexInList(chunk_index);
            let begin = if chunk_index == span.begin.chunk_index {
                span.begin.offset
            } else {
                0
            };
            let end = if chunk_index == span.end.chunk_index {
                span.end.offset
            } else {
                self.chunk(chunk_index).records().len() as u32
            };
            (begin < end).then_some((chunk_index, begin, end))
        })
    }

    /// The header and the payload of the record at `position`.
    pub fn record_at(&self, position: ListPosition) -> (DisplayListCommandHeader, &[u8]) {
        self.chunk(position.chunk_index).record_at(position.offset)
    }

    pub fn summary(&self) -> &DisplayListSummary {
        self.summary.get_or_init(|| {
            summary_of_pieces(self.chunks_in_paint_order.iter().enumerate().map(|(index, chunk)| {
                (
                    chunk.summary(),
                    self.first_run_index_of_each_chunk_then_run_count[index],
                    ChunkIndexInList(index as u32),
                )
            }))
        })
    }

    pub fn effect_clip_plan(&self, tree: &VisualContextTree) -> &EffectClipPlan {
        self.effect_clip_plan.get_or_init(|| {
            EffectClipPlan::new(tree, self.runs()).expect("a display list's runs name live nodes of its tree")
        })
    }

    /// Whether `tree` can take the place of the tree the list was paired with: the plan of the clips of its effects
    /// that the list keeps, if it keeps one yet, is the plan of the list under `tree` too. A tree from another process
    /// can claim the structural epoch of the list and still nest its effects otherwise.
    pub fn effect_clip_plan_holds_under(&self, tree: &VisualContextTree) -> bool {
        let Some(kept_plan) = self.effect_clip_plan.get() else {
            return true;
        };
        EffectClipPlan::new(tree, self.runs()).is_some_and(|plan| plan.places_effects_as(kept_plan))
    }

    pub fn run_groups(&self, tree: &VisualContextTree) -> Arc<RunGroups> {
        let mut cached_run_groups = self.cached_run_groups.lock().unwrap_or_else(PoisonError::into_inner);
        let tree_copy_without_animation_samples = tree.copy_without_animation_samples();
        if let Some((tree_copy, run_groups)) = cached_run_groups.as_mut()
            && ((*tree_copy == tree_copy_without_animation_samples
                && !tree.transform_animation_animates_plane_of_sorting_context())
                || run_groups.match_depth_sorting_roots(&tree.roots_of_contexts_sorting_planes_by_depth()))
        {
            *tree_copy = tree_copy_without_animation_samples;
            return run_groups.clone();
        }
        cached_run_groups
            .insert((
                tree_copy_without_animation_samples,
                Arc::new(RunGroups::new(tree, self.runs())),
            ))
            .1
            .clone()
    }

    /// Visits the compositor metadata commands of the runs that hold any, with the context of their run and their
    /// payload, in paint order.
    pub fn for_each_compositor_metadata(&self, mut visit: impl FnMut(ContextRef, DisplayListCommandType, &[u8])) {
        for chunk in &self.chunks_in_paint_order {
            let records = chunk.records();
            for run in chunk.runs().iter().filter(|run| run.has_compositor_metadata) {
                for_each_command(
                    &records[run.offset as usize..(run.offset + run.size) as usize],
                    |header, _, payload| {
                        if header.command_type.is_compositor_metadata() {
                            visit(run.context, header.command_type, payload);
                        }
                    },
                );
            }
        }
    }
}

/// The chunks of a recorded tape, which `chunks` cuts it into, each a copy. What the tape holds past its last chunk is a
/// chunk of its own.
pub fn chunks_of_recording(
    bytes: &[u8],
    runs: &[DisplayListCommandRun],
    chunks: &[DisplayListChunk],
) -> Vec<Arc<ListChunk>> {
    let mut list_chunks = Vec::with_capacity(chunks.len() + 1);
    let (mut covered_bytes, mut covered_runs) = (0usize, 0usize);
    for (chunk, chunk_bytes, chunk_runs) in chunks_of(bytes, runs, chunks) {
        if !chunk_runs.is_empty() {
            list_chunks.push(Arc::new(ListChunk::copied(
                chunk_bytes,
                chunk_runs,
                covered_bytes as u32,
            )));
        }
        covered_bytes = chunk.byte_end as usize;
        covered_runs = chunk.run_end as usize;
    }
    if covered_runs < runs.len() {
        list_chunks.push(Arc::new(ListChunk::copied(
            &bytes[covered_bytes..],
            &runs[covered_runs..],
            covered_bytes as u32,
        )));
    }
    list_chunks
}

pub(super) fn write_run_with_zeroed_padding(run: &DisplayListCommandRun, out: &mut [u8]) {
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
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Encode, Decode)]
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

    fn record_and_run_byte_counts(self) -> Option<(usize, usize)> {
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
            write_run_with_zeroed_padding(run, out);
        }
        rest = after_runs;
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
) -> Result<Vec<ListChunk>, &'static str> {
    let mut chunks = Vec::new();
    chunks
        .try_reserve_exact(sizes.len())
        .map_err(|_| "Display list has too many chunks to receive")?;
    let mut offset = 0usize;
    for size in sizes {
        let (record_bytes, run_bytes) = size
            .record_and_run_byte_counts()
            .ok_or("Display list chunk sizes overflow")?;
        let end = offset
            .checked_add(record_bytes)
            .and_then(|end| end.checked_add(run_bytes))
            .ok_or("Display list chunk sizes overflow")?;
        if end > packed_size_in_bytes {
            return Err("Display list chunks exceed the buffer that holds them");
        }
        let run_count = run_bytes / std::mem::size_of::<DisplayListCommandRun>();
        let largest_run_count_records_allow = record_bytes / super::builder::HEADER_SIZE;
        if run_count > largest_run_count_records_allow {
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
        chunks.push(ListChunk::named_copy(records, runs));
        offset = end;
    }
    if offset != packed_size_in_bytes {
        return Err("Display list chunks do not fill the buffer that holds them");
    }
    Ok(chunks)
}

/// Copies the chunks packed one after another in `packed`, whose sizes `sizes` lists in order, out, checks each copy and
/// names it by what it holds.
pub fn receive_packed_chunks(packed: &[u8], sizes: &[PackedChunkSize]) -> Result<Vec<ListChunk>, &'static str> {
    // SAFETY: The slice addresses its bytes.
    unsafe { receive_packed_chunks_from_shared_memory(packed.as_ptr(), packed.len(), sizes) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::display_list::builder::DisplayListBuilder;
    use crate::display_list::commands::{EffectNodeIndex, FillRect};
    use libgfx_rust::{Color, CompositingAndBlendingOperator, IntRect};

    fn fill_rect(y: i32) -> FillRect {
        FillRect {
            rect: IntRect::new(0, y, 10, 10),
            color: Color::default(),
            compositing_and_blending_operator: CompositingAndBlendingOperator::Normal,
            background_color_animation_effect: EffectNodeIndex::NONE,
        }
    }

    #[test]
    fn a_list_naming_chunks_whose_records_add_up_past_the_limit_is_refused() {
        let mut builder = DisplayListBuilder::new();
        let mut y = 0;
        while builder.byte_size() < 1 << 20 {
            builder.append(&fill_rect(y), &[], ContextRef::default());
            y += 1;
        }
        let chunk = Arc::new(ListChunk::copied(builder.bytes(), builder.command_runs(), 0));
        let occurrences_within_limit = (u32::MAX as usize) / chunk.records().len();
        assert!(DisplayListStorage::of_chunks(vec![chunk.clone(); occurrences_within_limit]).is_ok());
        assert!(DisplayListStorage::of_chunks(vec![chunk; occurrences_within_limit + 1]).is_err());
    }

    #[test]
    fn chunks_packed_for_transport_come_back_as_the_chunks_of_the_list_they_make() {
        use crate::display_list::chunks::ReachOutsideRunContext;
        use crate::display_list::commands::{CanvasId, ClipNodeIndex, DrawCanvas, PaintScrollBar, SpatialNodeIndex};
        use libgfx_rust::ScalingMode;
        let context = |spatial| ContextRef {
            spatial: SpatialNodeIndex(spatial),
            clip: ClipNodeIndex::NONE,
            effect: EffectNodeIndex::NONE,
        };
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0), &[], context(1));
        builder.append(
            &DrawCanvas {
                dst_rect: IntRect::new(0, 0, 30, 30),
                canvas_id: CanvasId(4),
                scaling_mode: ScalingMode::NearestNeighbor,
            },
            &[],
            context(1),
        );
        let first = builder.take_chunk();
        builder.append(&fill_rect(10), &[], context(1));
        builder.append(&fill_rect(20), &[], context(2));
        let canvas_offset_in_second_chunk = builder.byte_size() as u32;
        builder.append(
            &DrawCanvas {
                dst_rect: IntRect::new(40, 0, 30, 30),
                canvas_id: CanvasId(3),
                scaling_mode: ScalingMode::NearestNeighbor,
            },
            &[],
            context(2),
        );
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
        let received: Vec<Arc<ListChunk>> = received.into_iter().map(Arc::new).collect();
        let list = DisplayListStorage::of_chunks(received).unwrap();
        // The chunks of a recording of the same chunks are the same, and so is what is known of them.
        let tape = RecordedDisplayList::of_chunks(chunks);
        let recorded = DisplayListStorage::recorded(&tape, Arc::new(VisualContextTree::create_identity()));
        assert_eq!(list.summary(), recorded.summary());
        let ids = |list: &DisplayListStorage| list.chunks().iter().map(|chunk| chunk.id).collect::<Vec<_>>();
        assert_eq!(ids(&list), [first.id, second.id]);
        assert_eq!(ids(&recorded), [first.id, second.id]);
        // A canvas and a scroll bar are each alone in a run.
        assert_eq!(list.run_count(), 6);
        assert_eq!(list.chunk_of_run(0), (ChunkIndexInList(0), 0));
        assert_eq!(list.chunk_of_run(3), (ChunkIndexInList(1), 1));
        assert_eq!(
            list.span_of_run(3).begin,
            ListPosition::new(ChunkIndexInList(1), second.runs[1].offset)
        );
        // What the list knows of its records is where they are in their chunks.
        assert_eq!(
            list.summary().live_content_records[1].record.position,
            ListPosition::new(ChunkIndexInList(1), canvas_offset_in_second_chunk)
        );
        assert_eq!(list.summary().live_content_records[1].record.run_index, 4);
        assert!(
            list.chunk(ChunkIndexInList(0))
                .records_reaching_outside_run_context()
                .is_empty()
        );
        assert_eq!(
            list.chunk(ChunkIndexInList(1)).records_reaching_outside_run_context(),
            [TopLevelRecordReachingOutsideRunContext {
                record_offset: scrollbar_offset_in_second_chunk,
                run_index: 3,
                reach_outside_run_context: ReachOutsideRunContext::ScrollbarThumbFollowsScrollOffsetOf(
                    SpatialNodeIndex(3)
                ),
            }]
        );

        // Sizes that leave bytes over or claim more than the buffer holds are refused.
        assert!(receive_packed_chunks(&packed, &sizes[..1]).is_err());
        assert!(receive_packed_chunks(&packed[..packed.len() - 8], &sizes).is_err());
    }

    #[test]
    fn a_chunk_made_in_this_process_keeps_its_id_and_its_records_stay_aligned() {
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0), &[], ContextRef::default());
        builder.append(&fill_rect(10), &[], ContextRef::default());
        let chunk = Arc::new(builder.take_chunk());
        let list_chunk = ListChunk::of_chunk_made_in_this_process(&chunk);
        let copy = ListChunk::copied(&chunk.records, &chunk.runs, 0);
        assert_eq!(list_chunk.id, copy.id);
        assert_eq!(list_chunk.records(), copy.records());
        assert_eq!(list_chunk.runs(), copy.runs());
        assert_eq!(list_chunk.summary(), copy.summary());
        assert!(list_chunk.records().as_ptr().addr().is_multiple_of(COMMAND_ALIGNMENT));
        // Records that start where every command struct may are shared, not copied.
        if chunk.records.as_ptr().addr().is_multiple_of(COMMAND_ALIGNMENT) {
            assert_eq!(list_chunk.records().as_ptr(), chunk.records.as_ptr());
        }
    }

    #[test]
    fn a_recorded_list_survives_the_recorder_and_can_be_released_on_another_thread() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<RecordedDisplayList>();
        assert_send_sync::<DisplayListStorage>();
        let mut builder = DisplayListBuilder::new();
        builder.append(&fill_rect(0), &[], ContextRef::default());
        let mut recorded = Arc::new(builder.finish());
        let records = recorded.bytes.clone();
        let storage = DisplayListStorage::recorded(&recorded, Arc::new(VisualContextTree::create_identity()));
        Arc::make_mut(&mut recorded).bytes[0] = 9;
        assert_eq!(storage.chunk(ChunkIndexInList(0)).records(), &records[..]);
        drop(recorded);
        std::thread::spawn(move || {
            assert_eq!(storage.chunk(ChunkIndexInList(0)).records(), &records[..]);
            drop(storage);
        })
        .join()
        .unwrap();
    }

    #[test]
    fn a_plane_the_tree_turns_out_of_its_context_is_sorted_by_depth_in_a_sample_its_planar_animation_leaves_alone() {
        use crate::animation_timing::TimeValue;
        use crate::display_list::test_fixtures::{
            GREEN, ListOfOneChunk, fill, transform, transform_animation_of, tree_with_root_isolation_effect,
        };
        use crate::host::FfiVisualAnimationTransformOperationKind;
        use crate::visual_context::{SpatialData, TransformData};

        let element = IntRect::new(50, 50, 100, 100);
        let (mut tree, viewport) = tree_with_root_isolation_effect(1.0);
        let context_root = tree.append_spatial(
            SpatialData::Transform(TransformData {
                establishes_sorting_context: true,
                ..transform(element.to_float())
            }),
            viewport.spatial,
        );
        let turned_plane = tree.append_spatial(
            SpatialData::Transform(TransformData {
                matrix: libgfx_rust::rotation_matrix([0.0, 1.0, 0.0], 30f32.to_radians()),
                sorting_context_root_index: Some(context_root),
                ..transform(element.to_float())
            }),
            context_root,
        );
        let mut slide_in_the_plane = transform_animation_of(
            turned_plane,
            FfiVisualAnimationTransformOperationKind::TranslateX,
            0.0,
            100.0,
        );
        slide_in_the_plane.timing.start_delay = TimeValue::milliseconds(200.0);
        slide_in_the_plane.timing.iteration_count = 1.0;
        tree.set_visual_animations(vec![slide_in_the_plane]);
        let storage = ListOfOneChunk::default()
            .with(fill(IntRect::new(0, 0, 200, 200), GREEN), viewport)
            .with(
                fill(element, GREEN),
                ContextRef {
                    spatial: turned_plane,
                    ..viewport
                },
            )
            .storage();

        let sliding = tree.with_visual_animation_samples(450_000_000);
        assert!(!sliding.sorts_planes_by_depth());
        assert!(!storage.run_groups(&sliding).run_is_sorted_by_depth(1));

        let before_the_slide = tree.with_visual_animation_samples(0);
        assert!(before_the_slide.sorts_planes_by_depth());
        assert!(storage.run_groups(&before_the_slide).run_is_sorted_by_depth(1));
    }
}
