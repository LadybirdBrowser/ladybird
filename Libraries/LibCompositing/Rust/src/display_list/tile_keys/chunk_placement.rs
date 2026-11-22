/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The records of a chunk with what placing them and hashing tile content read of each, and how the records of a chunk
//! are placed in a raster space: the record ranges they are binned into tiles as, with the reach of each.

use super::{ContentHash, ContentHasher, EffectHashScope, TileKeyingOfRasterSpace};
use crate::display_list::builder::{
    HEADER_SIZE, bounding_rect_computed_from_payload, for_each_command, has_inline_clip_in_its_own_space, read_command,
};
use crate::display_list::commands::{
    ContextRef, DisplayListCommandRun, DisplayListCommandType, EffectNodeIndex, FillRect, FillRectWithRoundedCorners,
    FillRoundedRectRing,
};
use crate::display_list::nested_records::{
    NestedRecordsRole, for_each_command_including_nested, for_each_nested_record_span, span_bytes,
};
use crate::display_list::reach::Reach;
use crate::display_list::storage::{ChunkIndexInList, ListChunk, RunIndexInChunk};
use crate::display_list::summary::{command_blend, live_content_of};
use crate::display_list::window_server_placement::RoundedRect;
use libgfx_rust::{Color, CompositingAndBlendingOperator, CornerRadii, IntRect};
use std::sync::Arc;

/// The record draws live content, such as a caret or a scroll bar, itself or in a record nested in it, whose look the
/// frame decides. The frame state of the list hashes that of every such record that is in a band.
pub(super) const FRAME_STATE_LIVE_CONTENT: u8 = 1 << 0;
pub(super) const FRAME_STATE_SPACES_OF_INLINE_CLIPS: u8 = 1 << 1;
pub(super) const FRAME_STATE_SAMPLED_BACKGROUND_COLOR: u8 = 1 << 2;

/// A top-level record of a chunk, with what placing it and hashing tile content read of it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ChunkRecord {
    pub(crate) offset: u32,
    pub(crate) end_offset: u32,
    pub(crate) run_index: RunIndexInChunk,
    pub(crate) bounding_rect: IntRect,
    draws_something: bool,
    pub(super) frame_state_read_flags: u8,
    pub(super) can_lower_destination_alpha: bool,
    // Record bytes hold no offset into the chunk, so a record hashes alike wherever it is.
    pub(super) content_hash_of_bytes: ContentHash,
}

impl ChunkRecord {
    pub(super) fn reads_frame_state_hashed_for_the_list(&self) -> bool {
        self.frame_state_read_flags & FRAME_STATE_LIVE_CONTENT != 0
    }
}

/// The index of a top-level record in the `RecordTable` of its chunk.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct RecordIndexInChunk(pub u32);

/// The top-level records of a chunk, by run, with what hashing tile content needs of each.
#[derive(Debug, PartialEq, Eq)]
pub struct RecordTable {
    records: Vec<ChunkRecord>,
    first_record_index_of_each_run: Vec<RecordIndexInChunk>,
    run_reads_frame_state_hashed_for_the_list_by_run_index: Vec<bool>,
}

fn frame_state_read_flags_of_record(
    header: &crate::display_list::commands::DisplayListCommandHeader,
    payload: &[u8],
    record_bytes: &[u8],
) -> u8 {
    let mut flags = 0;
    if header.inline_clip_count != 0 && has_inline_clip_in_its_own_space(header, payload) {
        flags |= FRAME_STATE_SPACES_OF_INLINE_CLIPS;
    }
    for_each_command_including_nested(record_bytes, &mut |command_type, _, payload| {
        if live_content_of(command_type, payload).is_some() {
            flags |= FRAME_STATE_LIVE_CONTENT;
        }
        if !background_color_animation_effect_of(command_type, payload).is_none() {
            flags |= FRAME_STATE_SAMPLED_BACKGROUND_COLOR;
        }
    });
    flags
}

// Records nested in a group, a pattern tile or a mask draw into a layer of their own; only those of a vector image draw
// straight into the destination.
fn record_can_lower_destination_alpha(command_type: DisplayListCommandType, payload: &[u8]) -> bool {
    if command_blend(command_type, payload).is_some_and(|(operator, _)| operator.lowers_destination_alpha()) {
        return true;
    }
    let mut can_lower = false;
    for_each_nested_record_span(command_type, payload, |role, span| {
        if role == NestedRecordsRole::VectorImage {
            for_each_command(span_bytes(payload, span), |header, _, payload| {
                can_lower |= record_can_lower_destination_alpha(header.command_type, payload);
            });
        }
    });
    can_lower
}

pub(super) fn background_color_animation_effect_of(
    command_type: DisplayListCommandType,
    payload: &[u8],
) -> EffectNodeIndex {
    match command_type {
        DisplayListCommandType::FillRect => read_command::<FillRect>(payload).background_color_animation_effect,
        DisplayListCommandType::FillRectWithRoundedCorners => {
            read_command::<FillRectWithRoundedCorners>(payload).background_color_animation_effect
        }
        _ => EffectNodeIndex::NONE,
    }
}

impl RecordTable {
    pub fn new(records_of_chunk: &[u8], command_runs: &[DisplayListCommandRun]) -> Self {
        let mut records = Vec::new();
        let mut first_record_index_of_each_run = Vec::with_capacity(command_runs.len() + 1);
        let mut run_reads_frame_state_hashed_for_the_list_by_run_index = Vec::with_capacity(command_runs.len());
        for (run_index, run) in command_runs.iter().enumerate() {
            let first_record_index_of_run = records.len();
            first_record_index_of_each_run.push(RecordIndexInChunk(first_record_index_of_run as u32));
            let run_bytes = &records_of_chunk[run.offset as usize..(run.offset + run.size) as usize];
            for_each_command(run_bytes, |header, offset_in_run, payload| {
                let offset = run.offset + offset_in_run as u32;
                let end_offset = offset + (HEADER_SIZE + payload.len()) as u32;
                let record_bytes = &records_of_chunk[offset as usize..end_offset as usize];
                let mut hasher = ContentHasher::new();
                hasher.write_bytes(record_bytes);
                let bounding_rect = bounding_rect_computed_from_payload(header, payload);
                records.push(ChunkRecord {
                    offset,
                    end_offset,
                    run_index: RunIndexInChunk(run_index as u32),
                    bounding_rect: bounding_rect.unwrap_or_default(),
                    draws_something: !header.command_type.is_compositor_metadata()
                        && bounding_rect.is_some_and(|rect| !rect.is_empty()),
                    frame_state_read_flags: frame_state_read_flags_of_record(header, payload, record_bytes),
                    can_lower_destination_alpha: record_can_lower_destination_alpha(header.command_type, payload),
                    content_hash_of_bytes: hasher.finish(),
                });
            });
            run_reads_frame_state_hashed_for_the_list_by_run_index.push(
                records[first_record_index_of_run..]
                    .iter()
                    .any(ChunkRecord::reads_frame_state_hashed_for_the_list),
            );
        }
        first_record_index_of_each_run.push(RecordIndexInChunk(records.len() as u32));
        Self {
            records,
            first_record_index_of_each_run,
            run_reads_frame_state_hashed_for_the_list_by_run_index,
        }
    }

    pub(super) fn run_reads_frame_state_hashed_for_the_list(&self, run_index: RunIndexInChunk) -> bool {
        self.run_reads_frame_state_hashed_for_the_list_by_run_index[run_index.0 as usize]
    }

    pub(crate) fn record(&self, index: RecordIndexInChunk) -> &ChunkRecord {
        &self.records[index.0 as usize]
    }

    pub(crate) fn records_in(&self, indices: std::ops::Range<RecordIndexInChunk>) -> &[ChunkRecord] {
        &self.records[indices.start.0 as usize..indices.end.0 as usize]
    }

    pub(crate) fn record_indices_of_run(&self, run_index: RunIndexInChunk) -> std::ops::Range<RecordIndexInChunk> {
        let run_index = run_index.0 as usize;
        self.first_record_index_of_each_run[run_index]..self.first_record_index_of_each_run[run_index + 1]
    }

    pub(crate) fn record_indices_between_offsets(
        &self,
        begin_offset: u32,
        end_offset: u32,
    ) -> std::ops::Range<RecordIndexInChunk> {
        let first = self.records.partition_point(|record| record.offset < begin_offset);
        let end = self.records.partition_point(|record| record.offset < end_offset);
        RecordIndexInChunk(first as u32)..RecordIndexInChunk(end as u32)
    }
}

#[derive(Clone, Copy, Debug)]
pub(super) struct DeviceAlignedFill {
    pub(super) rect: RoundedRect,
    pub(super) unfilled_inner_rect_of_ring: Option<RoundedRect>,
    pub(super) content_hash_except_rects: ContentHash,
    pub(super) color: Color,
    pub(super) draws_recorded_color_with_source_over: bool,
}

/// The index of a range in the `record_ranges` of a `ChunkPlacedInRasterSpace`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct PlacedRecordRangeIndex(pub u32);

#[derive(Clone, Copy, Debug)]
pub(super) struct PlacedRecordRange {
    pub(super) run_index: RunIndexInChunk,
    pub(super) first_record_index: RecordIndexInChunk,
    pub(super) end_record_index: RecordIndexInChunk,
    pub(super) begin_offset: u32,
    pub(super) end_offset: u32,
    pub(super) reach: Reach<IntRect>,
    pub(super) context: ContextRef,
    pub(super) is_spread_by_no_filter: bool,
    pub(super) device_aligned_fill: Option<DeviceAlignedFill>,
}

/// The ranges of a run: those it is binned as, from `first_binned_range_index` up to the range of the whole run.
#[derive(Clone, Copy, Debug, Default)]
pub(super) struct PlacedRecordRangesOfRun {
    pub(super) first_binned_range_index: PlacedRecordRangeIndex,
    pub(super) whole_run_range_index: PlacedRecordRangeIndex,
    pub(super) reach_of_binned_ranges: Reach<IntRect>,
}

/// What the records of a chunk hold in one raster space, as the geometry of the space places the visual contexts of
/// the chunk's runs it holds: the record ranges the records are binned into tiles as, the reach of each, and the
/// content hash of what each draws. A chunk caches its latest placements for every list that names it.
pub struct ChunkPlacedInRasterSpace {
    placement_inputs_hash: ContentHash,
    pub(super) record_ranges: Vec<PlacedRecordRange>,
    pub(super) ranges_of_each_run: Vec<PlacedRecordRangesOfRun>,
}

impl ChunkPlacedInRasterSpace {
    pub fn placement_inputs_hash(&self) -> ContentHash {
        self.placement_inputs_hash
    }
}

impl TileKeyingOfRasterSpace<'_> {
    fn place_record_range(
        &self,
        chunk: &ListChunk,
        run_index: RunIndexInChunk,
        record_indices: std::ops::Range<RecordIndexInChunk>,
        reach: Reach<IntRect>,
    ) -> PlacedRecordRange {
        let table = chunk.record_table();
        let context = chunk.runs()[run_index.0 as usize].context;
        let is_spread_by_filters = !self
            .list
            .filters_of_effects()
            .nearest_spreading_effect_at_or_above(context.effect)
            .is_none();
        let is_one_record = (record_indices.start.0..record_indices.end.0).len() == 1;
        PlacedRecordRange {
            run_index,
            first_record_index: record_indices.start,
            end_record_index: record_indices.end,
            begin_offset: table.record(record_indices.start).offset,
            end_offset: table.record(RecordIndexInChunk(record_indices.end.0 - 1)).end_offset,
            reach,
            context,
            is_spread_by_no_filter: !is_spread_by_filters,
            device_aligned_fill: is_one_record
                .then(|| self.device_aligned_fill_of_record(chunk, table.record(record_indices.start), context))
                .flatten(),
        }
    }

    pub(super) fn device_aligned_fill_of_record(
        &self,
        chunk: &ListChunk,
        record: &ChunkRecord,
        context: ContextRef,
    ) -> Option<DeviceAlignedFill> {
        let (header, payload) = chunk.record_at(record.offset);
        if header.inline_clip_count != 0 || header.has_inline_transform {
            return None;
        }
        let mut hasher = ContentHasher::new();
        hasher.write_u32(header.command_type as u32);
        let color;
        let draws_recorded_color_with_source_over;
        let (rect, corner_radii, unfilled_inner_rect_of_ring) = match header.command_type {
            DisplayListCommandType::FillRect => {
                let fill = read_command::<FillRect>(payload);
                hasher.write_u32(fill.color.0);
                hasher.write_u32(fill.compositing_and_blending_operator as u32);
                hasher.write_u32(fill.background_color_animation_effect.0);
                color = fill.color;
                draws_recorded_color_with_source_over = fill.compositing_and_blending_operator
                    == CompositingAndBlendingOperator::Normal
                    && fill.background_color_animation_effect.is_none();
                (fill.rect, CornerRadii::default(), None)
            }
            DisplayListCommandType::FillRectWithRoundedCorners => {
                let fill = read_command::<FillRectWithRoundedCorners>(payload);
                hasher.write_u32(fill.color.0);
                hasher.write_u32(fill.background_color_animation_effect.0);
                color = fill.color;
                draws_recorded_color_with_source_over = fill.background_color_animation_effect.is_none();
                (fill.rect, fill.corner_radii, None)
            }
            DisplayListCommandType::FillRoundedRectRing => {
                let ring = read_command::<FillRoundedRectRing>(payload);
                hasher.write_u32(ring.color.0);
                color = ring.color;
                draws_recorded_color_with_source_over = true;
                for width in ring.widths_within_rect() {
                    hasher.write_i32(width);
                }
                (ring.rect, ring.corner_radii, Some(ring.unfilled_inner_rect()?))
            }
            _ => return None,
        };
        let unfilled_inner_rect_of_ring = match unfilled_inner_rect_of_ring {
            Some(unfilled_inner_rect) => Some(RoundedRect::device_aligned_of(
                &self.geometry.world,
                context.spatial,
                unfilled_inner_rect.to_float(),
                &CornerRadii::default(),
            )?),
            None => None,
        };
        Some(DeviceAlignedFill {
            rect: RoundedRect::device_aligned_of(
                &self.geometry.world,
                context.spatial,
                rect.to_float(),
                &corner_radii,
            )?,
            unfilled_inner_rect_of_ring,
            content_hash_except_rects: hasher.finish(),
            color,
            draws_recorded_color_with_source_over,
        })
    }

    fn place_runs_of_chunk(
        &self,
        chunk: &ListChunk,
        run_indices: &[RunIndexInChunk],
        placement_inputs_hash: ContentHash,
    ) -> ChunkPlacedInRasterSpace {
        let table = chunk.record_table();
        let mut record_ranges = Vec::new();
        let mut ranges_of_each_run = vec![PlacedRecordRangesOfRun::default(); chunk.runs().len()];
        for &run_index in run_indices {
            let run = &chunk.runs()[run_index.0 as usize];
            let record_indices_of_run = table.record_indices_of_run(run_index);
            let run_reach = self.reach_of_content_within(run.ink_bounds.to_float(), run.context);
            let first_binned_range_index = PlacedRecordRangeIndex(record_ranges.len() as u32);
            let mut reach_of_binned_ranges = Reach::Nothing;
            if run_reach != Reach::Nothing {
                if !self.geometry.world.draw_matrix(run.context.spatial).is_2d_affine() {
                    record_ranges.push(self.place_record_range(
                        chunk,
                        run_index,
                        record_indices_of_run.clone(),
                        run_reach,
                    ));
                    reach_of_binned_ranges = run_reach;
                } else {
                    for record_index in
                        (record_indices_of_run.start.0..record_indices_of_run.end.0).map(RecordIndexInChunk)
                    {
                        let record = table.record(record_index);
                        if !record.draws_something {
                            continue;
                        }
                        let record_reach = self.reach_of_content_within(record.bounding_rect.to_float(), run.context);
                        if record_reach == Reach::Nothing {
                            continue;
                        }
                        record_ranges.push(self.place_record_range(
                            chunk,
                            run_index,
                            record_index..RecordIndexInChunk(record_index.0 + 1),
                            record_reach,
                        ));
                        reach_of_binned_ranges = reach_of_binned_ranges.united(record_reach);
                    }
                }
            }
            let whole_run_range_index = PlacedRecordRangeIndex(record_ranges.len() as u32);
            record_ranges.push(self.place_record_range(chunk, run_index, record_indices_of_run, run_reach));
            ranges_of_each_run[run_index.0 as usize] = PlacedRecordRangesOfRun {
                first_binned_range_index,
                whole_run_range_index,
                reach_of_binned_ranges,
            };
        }
        ChunkPlacedInRasterSpace {
            placement_inputs_hash,
            record_ranges,
            ranges_of_each_run,
        }
    }

    pub(super) fn placement_of_runs_of_chunk(
        &self,
        chunk_index: ChunkIndexInList,
        run_indices: &[RunIndexInChunk],
    ) -> Arc<ChunkPlacedInRasterSpace> {
        let chunk = self.list.display_list.chunk(chunk_index);
        let mut hasher = ContentHasher::new();
        for &run_index in run_indices {
            hasher.write_u32(run_index.0);
            hasher.write_content_hash(self.context_content_hash(
                chunk.runs()[run_index.0 as usize].context,
                EffectHashScope::WhatPlacingReads,
            ));
        }
        let placement_inputs_hash = hasher.finish();
        let (placement, newly_placed) = chunk.find_or_make_placement(placement_inputs_hash, || {
            self.place_runs_of_chunk(chunk, run_indices, placement_inputs_hash)
        });
        if newly_placed {
            self.chunks_placed.set(self.chunks_placed.get() + 1);
        }
        placement
    }
}
