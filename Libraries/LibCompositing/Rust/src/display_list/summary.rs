/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What is asked about the records of a display list: the resources its commands reference, whether reusing a raster
//! of it can differ from replaying it, its carets, where it draws live content, and where it declares the content of
//! masks.

use super::builder::{for_each_command, read_command};
use super::commands::*;
use super::nested_records::{for_each_nested_record_span, span_bytes};
use super::storage::{ChunkIndexInList, ListPosition};
use libgfx_rust::CompositingAndBlendingOperator;
use libgfx_rust::filter::Filter;
use libipc_rust::wire::{Decode, Encode};
use std::collections::HashSet;

/// A top-level record, by the index of its run among the runs of its list and its position in the list. In the summary
/// of a chunk alone, the run index counts the runs of that chunk only, and the position is in that chunk.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct IndexedRecord {
    pub run_index: u32,
    pub position: ListPosition,
}

impl IndexedRecord {
    fn shifted_into_list(self, first_run_index_of_chunk: u32, chunk_index: ChunkIndexInList) -> Self {
        Self {
            run_index: self.run_index + first_run_index_of_chunk,
            position: ListPosition::new(chunk_index, self.position.offset),
        }
    }
}

/// Content that changes under the list's immutable commands. A record that draws it can be a layer of its own.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Encode, Decode)]
pub enum LiveContent {
    Canvas(CanvasId),
    VideoFrame(VideoSinkHandle),
    ImageFrame(ImageFrameResourceId),
    CompositedContext(CompositorContextId),
    /// A caret, which blinks.
    Caret,
    /// The vertical or horizontal scroll bar of the scroller of `scroll_node`, which draws what the frame shows of the
    /// scroller.
    ScrollBar {
        scroll_node: SpatialNodeIndex,
        vertical: bool,
    },
}

impl LiveContent {
    /// Whether its record draws all of it, as that of a caret or a scroll bar does, rather than pixels from outside the
    /// list.
    pub fn is_drawn_by_its_record(self) -> bool {
        matches!(self, LiveContent::Caret | LiveContent::ScrollBar { .. })
    }
}

/// A top-level record that draws live content, itself or in a record nested in it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct LiveContentRecord {
    pub record: IndexedRecord,
    pub content: LiveContent,
}

/// A top-level record that declares the content of a mask, with the effect of the mask.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct MaskDeclarationRecord {
    pub record: IndexedRecord,
    pub mask_effect: EffectNodeIndex,
}

#[derive(Debug, Default, PartialEq, Eq)]
pub struct DisplayListSummary {
    pub font_ids: Vec<u64>,
    pub image_frame_ids: Vec<u64>,
    pub video_sink_handles: Vec<u64>,
    pub vector_image_ids: Vec<u64>,
    pub canvas_ids: Vec<u64>,
    pub composited_context_ids: Vec<u64>,
    pub live_content_records: Vec<LiveContentRecord>,
    pub destination_blending_records: Vec<IndexedRecord>,
    pub mask_declarations: Vec<MaskDeclarationRecord>,
}

#[derive(Default)]
struct ReferencedIds {
    fonts: HashSet<u64>,
    image_frames: HashSet<u64>,
    video_sinks: HashSet<u64>,
    vector_images: HashSet<u64>,
    canvases: HashSet<u64>,
    composited_contexts: HashSet<u64>,
}

pub(crate) fn live_content_of(command_type: DisplayListCommandType, payload: &[u8]) -> Option<LiveContent> {
    use DisplayListCommandType as T;
    match command_type {
        T::DrawCanvas => Some(LiveContent::Canvas(read_command::<DrawCanvas>(payload).canvas_id)),
        T::DrawVideoFrame => Some(LiveContent::VideoFrame(
            read_command::<DrawVideoFrame>(payload).video_sink_handle,
        )),
        T::DrawScaledDecodedImageFrame => Some(LiveContent::ImageFrame(
            read_command::<DrawScaledDecodedImageFrame>(payload).frame_id,
        )),
        T::DrawRepeatedDecodedImageFrame => Some(LiveContent::ImageFrame(
            read_command::<DrawRepeatedDecodedImageFrame>(payload).frame_id,
        )),
        T::DrawTiledDecodedImageFrame => Some(LiveContent::ImageFrame(
            read_command::<DrawTiledDecodedImageFrame>(payload).frame_id,
        )),
        T::DrawCompositedContext => Some(LiveContent::CompositedContext(
            read_command::<DrawCompositedContext>(payload).child_context_id,
        )),
        T::PaintCaret => Some(LiveContent::Caret),
        T::PaintScrollBar => {
            let scroll_bar = read_command::<PaintScrollBar>(payload);
            Some(LiveContent::ScrollBar {
                scroll_node: scroll_bar.scroll_node_index,
                vertical: scroll_bar.vertical,
            })
        }
        _ => None,
    }
}

fn note_referenced_ids(command_type: DisplayListCommandType, payload: &[u8], ids: &mut ReferencedIds) {
    use DisplayListCommandType as T;
    match command_type {
        T::DrawGlyphRun => {
            ids.fonts.insert(read_command::<DrawGlyphRun>(payload).font_id.0);
        }
        T::PaintTextShadow => {
            ids.fonts.insert(read_command::<PaintTextShadow>(payload).font_id.0);
        }
        T::DrawScaledDecodedImageFrame => {
            ids.image_frames
                .insert(read_command::<DrawScaledDecodedImageFrame>(payload).frame_id.0);
        }
        T::DrawRepeatedDecodedImageFrame => {
            ids.image_frames
                .insert(read_command::<DrawRepeatedDecodedImageFrame>(payload).frame_id.0);
        }
        T::DrawTiledDecodedImageFrame => {
            ids.image_frames
                .insert(read_command::<DrawTiledDecodedImageFrame>(payload).frame_id.0);
        }
        T::DrawVideoFrame => {
            ids.video_sinks
                .insert(read_command::<DrawVideoFrame>(payload).video_sink_handle.0);
        }
        T::DrawCanvas => {
            ids.canvases.insert(read_command::<DrawCanvas>(payload).canvas_id.0);
        }
        T::PaintVectorImage => {
            ids.vector_images
                .insert(read_command::<PaintVectorImage>(payload).vector_image_id.0);
        }
        T::DrawCompositedContext => {
            ids.composited_contexts
                .insert(read_command::<DrawCompositedContext>(payload).child_context_id.0);
        }
        T::DrawIsolatedGroup => {
            let filter = read_command::<DrawIsolatedGroup>(payload).filter;
            if !filter.is_empty()
                && let Some(filter) = Filter::deserialize(span_bytes(payload, filter))
            {
                filter.for_each_image_frame_id(&mut |id| {
                    ids.image_frames.insert(id);
                });
            }
        }
        T::FillRect
        | T::PaintCaret
        | T::DrawRepeatedTile
        | T::PaintLinearGradient
        | T::PaintRadialGradient
        | T::PaintConicGradient
        | T::PaintOuterBoxShadow
        | T::PaintInnerBoxShadow
        | T::PaintPathBoxShadow
        | T::FillRectWithRoundedCorners
        | T::FillRoundedRectRing
        | T::FillPath
        | T::StrokePath
        | T::DrawEllipse
        | T::DrawLine
        | T::BackdropFilterRegion
        | T::DrawRect
        | T::DeclareMaskContent
        | T::CompositorWheelHitTestTarget
        | T::CompositorWheelHitTestTargetWithCornerRadii
        | T::CompositorMainThreadWheelEventRegion
        | T::CompositorScrollbar
        | T::CompositorBlockingWheelEventRegion
        | T::PaintScrollBar
        | T::CompositorNestedContextWheelRegion
        // The display list of a page holds none of the commands of the canvas 2D stream.
        | T::Canvas2DFillPath
        | T::Canvas2DStrokePath
        | T::Canvas2DDrawGlyphRuns
        | T::Canvas2DClearRect
        | T::Canvas2DDrawImageFrame
        | T::Canvas2DDrawCanvas
        | T::Canvas2DDrawVideoFrame
        | T::Canvas2DSetTransform
        | T::Canvas2DSave
        | T::Canvas2DRestore
        | T::Canvas2DClipPath
        | T::Canvas2DReset
        | T::Canvas2DClearCanvas
        | T::Canvas2DPutImageFrame => {}
    }
}

fn for_each_record_including_nested(records: &[u8], visit: &mut impl FnMut(DisplayListCommandType, &[u8])) {
    for_each_command(records, |header, _, payload| {
        visit(header.command_type, payload);
        for_each_nested_record_span(header.command_type, payload, |_, span| {
            for_each_record_including_nested(span_bytes(payload, span), visit);
        });
    });
}

fn sorted(ids: HashSet<u64>) -> Vec<u64> {
    let mut ids: Vec<u64> = ids.into_iter().collect();
    ids.sort_unstable();
    ids
}

pub fn summarize(tape: &[u8], command_runs: &[DisplayListCommandRun]) -> DisplayListSummary {
    let mut ids = ReferencedIds::default();
    let mut summary = DisplayListSummary::default();
    for (run_index, run) in command_runs.iter().enumerate() {
        let run_start = run.offset as usize;
        let records = &tape[run_start..run_start + run.size as usize];
        for_each_command(records, |header, offset_in_run, payload| {
            let indexed_record = IndexedRecord {
                run_index: run_index as u32,
                position: ListPosition::new(ChunkIndexInList(0), (run_start + offset_in_run) as u32),
            };
            if header.command_type == DisplayListCommandType::DeclareMaskContent {
                summary.mask_declarations.push(MaskDeclarationRecord {
                    record: indexed_record,
                    mask_effect: read_command::<DeclareMaskContent>(payload).effect,
                });
            }
            if let Some((blend, blends_with_isolated_backdrop_color)) = command_blend(header.command_type, payload)
                && blend != CompositingAndBlendingOperator::Normal
                && !blends_with_isolated_backdrop_color
            {
                summary.destination_blending_records.push(indexed_record);
            }
            note_referenced_ids(header.command_type, payload, &mut ids);
            let mut note_live_content = |command_type, payload: &[u8]| {
                if let Some(content) = live_content_of(command_type, payload) {
                    summary.live_content_records.push(LiveContentRecord {
                        record: indexed_record,
                        content,
                    });
                }
            };
            note_live_content(header.command_type, payload);
            for_each_nested_record_span(header.command_type, payload, |_, span| {
                for_each_record_including_nested(span_bytes(payload, span), &mut |command_type, payload| {
                    note_referenced_ids(command_type, payload, &mut ids);
                    note_live_content(command_type, payload);
                });
            });
        });
    }
    summary.font_ids = sorted(ids.fonts);
    summary.image_frame_ids = sorted(ids.image_frames);
    summary.video_sink_handles = sorted(ids.video_sinks);
    summary.vector_image_ids = sorted(ids.vector_images);
    summary.canvas_ids = sorted(ids.canvases);
    summary.composited_context_ids = sorted(ids.composited_contexts);
    summary
}

/// The summary of a list made of chunks, each summarized alone: the summary of a chunk, the index of its first run
/// among the runs of the list, and its place among the chunks of the list.
pub fn summary_of_pieces<'a>(
    pieces: impl IntoIterator<Item = (&'a DisplayListSummary, u32, ChunkIndexInList)>,
) -> DisplayListSummary {
    let mut summary = DisplayListSummary::default();
    for (chunk_summary, first_run_index_of_chunk, chunk_index) in pieces {
        let shift_into_list = |record: &IndexedRecord| record.shifted_into_list(first_run_index_of_chunk, chunk_index);
        summary
            .live_content_records
            .extend(
                chunk_summary
                    .live_content_records
                    .iter()
                    .map(|live_content_record| LiveContentRecord {
                        record: shift_into_list(&live_content_record.record),
                        content: live_content_record.content,
                    }),
            );
        summary
            .destination_blending_records
            .extend(chunk_summary.destination_blending_records.iter().map(shift_into_list));
        summary
            .mask_declarations
            .extend(
                chunk_summary
                    .mask_declarations
                    .iter()
                    .map(|declaration| MaskDeclarationRecord {
                        record: shift_into_list(&declaration.record),
                        mask_effect: declaration.mask_effect,
                    }),
            );
        summary.font_ids.extend_from_slice(&chunk_summary.font_ids);
        summary
            .image_frame_ids
            .extend_from_slice(&chunk_summary.image_frame_ids);
        summary
            .video_sink_handles
            .extend_from_slice(&chunk_summary.video_sink_handles);
        summary
            .vector_image_ids
            .extend_from_slice(&chunk_summary.vector_image_ids);
        summary.canvas_ids.extend_from_slice(&chunk_summary.canvas_ids);
        summary
            .composited_context_ids
            .extend_from_slice(&chunk_summary.composited_context_ids);
    }
    for ids in [
        &mut summary.font_ids,
        &mut summary.image_frame_ids,
        &mut summary.video_sink_handles,
        &mut summary.vector_image_ids,
        &mut summary.canvas_ids,
        &mut summary.composited_context_ids,
    ] {
        ids.sort_unstable();
        ids.dedup();
    }
    summary
}

/// The blend a command composites with, and whether it blends with a backdrop color of its own instead of the
/// destination.
pub(crate) fn command_blend(
    command_type: DisplayListCommandType,
    payload: &[u8],
) -> Option<(CompositingAndBlendingOperator, bool)> {
    use DisplayListCommandType as T;
    match command_type {
        T::FillRect => Some((
            read_command::<FillRect>(payload).compositing_and_blending_operator,
            false,
        )),
        T::DrawScaledDecodedImageFrame => {
            let command = read_command::<DrawScaledDecodedImageFrame>(payload);
            Some((
                command.compositing_and_blending_operator,
                command.isolated_backdrop_color.has_value,
            ))
        }
        T::DrawRepeatedDecodedImageFrame => {
            let command = read_command::<DrawRepeatedDecodedImageFrame>(payload);
            Some((
                command.compositing_and_blending_operator,
                command.isolated_backdrop_color.has_value,
            ))
        }
        T::DrawRepeatedTile => Some((
            read_command::<DrawRepeatedTile>(payload).compositing_and_blending_operator,
            false,
        )),
        T::PaintLinearGradient => Some((
            read_command::<PaintLinearGradient>(payload).compositing_and_blending_operator,
            false,
        )),
        T::PaintRadialGradient => Some((
            read_command::<PaintRadialGradient>(payload).compositing_and_blending_operator,
            false,
        )),
        T::PaintConicGradient => Some((
            read_command::<PaintConicGradient>(payload).compositing_and_blending_operator,
            false,
        )),
        T::FillPath => Some((
            read_command::<FillPath>(payload).compositing_and_blending_operator,
            false,
        )),
        T::DrawIsolatedGroup => Some((
            read_command::<DrawIsolatedGroup>(payload).compositing_and_blending_operator,
            false,
        )),
        T::DrawGlyphRun
        | T::PaintCaret
        | T::DrawTiledDecodedImageFrame
        | T::DrawCompositedContext
        | T::DrawCanvas
        | T::DrawVideoFrame
        | T::PaintOuterBoxShadow
        | T::PaintInnerBoxShadow
        | T::PaintPathBoxShadow
        | T::PaintTextShadow
        | T::FillRectWithRoundedCorners
        | T::FillRoundedRectRing
        | T::StrokePath
        | T::DrawEllipse
        | T::DrawLine
        | T::BackdropFilterRegion
        | T::DrawRect
        | T::PaintVectorImage
        | T::DeclareMaskContent
        | T::CompositorWheelHitTestTarget
        | T::CompositorWheelHitTestTargetWithCornerRadii
        | T::CompositorMainThreadWheelEventRegion
        | T::CompositorScrollbar
        | T::CompositorBlockingWheelEventRegion
        | T::PaintScrollBar
        | T::CompositorNestedContextWheelRegion
        // The display list of a page holds none of the commands of the canvas 2D stream.
        | T::Canvas2DFillPath
        | T::Canvas2DStrokePath
        | T::Canvas2DDrawGlyphRuns
        | T::Canvas2DClearRect
        | T::Canvas2DDrawImageFrame
        | T::Canvas2DDrawCanvas
        | T::Canvas2DDrawVideoFrame
        | T::Canvas2DSetTransform
        | T::Canvas2DSave
        | T::Canvas2DRestore
        | T::Canvas2DClipPath
        | T::Canvas2DReset
        | T::Canvas2DClearCanvas
        | T::Canvas2DPutImageFrame => None,
    }
}

fn draws_live_content(command_type: DisplayListCommandType) -> bool {
    matches!(
        command_type,
        DisplayListCommandType::DrawVideoFrame
            | DisplayListCommandType::DrawCanvas
            | DisplayListCommandType::DrawCompositedContext
    )
}

/// Whether reusing a raster of an SVG image's records can produce different pixels than replaying them in place. That
/// is the case when a destination-reading operation can see content painted before the records began: a non-normal
/// blend on a command, or on a group of them. A blend inside a group reads only content of the group. It is also the
/// case when the records draw live content that changes under them: video frames, canvases and composited child
/// contexts.
pub fn vector_image_records_require_direct_replay(records: &[u8]) -> bool {
    let mut requires_direct_replay = false;
    for_each_command(records, |header, _, payload| {
        if requires_direct_replay {
            return;
        }
        if draws_live_content(header.command_type) {
            requires_direct_replay = true;
            return;
        }
        if let Some((blend, blends_with_isolated_backdrop_color)) = command_blend(header.command_type, payload)
            && blend != CompositingAndBlendingOperator::Normal
            && !blends_with_isolated_backdrop_color
        {
            requires_direct_replay = true;
            return;
        }
        for_each_nested_record_span(header.command_type, payload, |_, span| {
            for_each_record_including_nested(span_bytes(payload, span), &mut |command_type, _| {
                requires_direct_replay |= draws_live_content(command_type);
            });
        });
    });
    requires_direct_replay
}
