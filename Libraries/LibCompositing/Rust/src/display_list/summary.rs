/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What C++ asks about a display list's tape: the resources its commands reference, whether reusing a raster of it
//! can differ from replaying it, its compositor metadata, and where it draws canvases and carets.

use super::builder::{HEADER_SIZE, for_each_command, read_command};
use super::commands::*;
use super::nested_records::{for_each_nested_record_span, span_bytes};
use crate::visual_context::VisualContextTree;
use libgfx_rust::CompositingAndBlendingOperator;
use libgfx_rust::filter::Filter;
use std::collections::HashSet;

// A top-level record, found by the run it belongs to and its offset in the tape.
#[derive(Clone, Copy, Debug)]
pub struct IndexedRecord {
    pub run_index: u32,
    pub record_offset: u32,
}

#[derive(Default)]
pub struct DisplayListSummary {
    pub font_ids: Vec<u64>,
    pub image_frame_ids: Vec<u64>,
    pub video_sink_ids: Vec<u64>,
    pub display_list_ids: Vec<u64>,
    pub drawn_canvases: Vec<IndexedRecord>,
    pub carets: Vec<IndexedRecord>,
}

#[derive(Default)]
struct ReferencedIds {
    fonts: HashSet<u64>,
    image_frames: HashSet<u64>,
    video_sinks: HashSet<u64>,
    display_lists: HashSet<u64>,
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
                .insert(read_command::<DrawVideoFrame>(payload).video_sink_id.0);
        }
        T::PaintNestedDisplayList => {
            ids.display_lists
                .insert(read_command::<PaintNestedDisplayList>(payload).display_list_id.0);
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
        | T::DrawCompositedContext
        | T::DrawCanvas
        | T::PaintLinearGradient
        | T::PaintRadialGradient
        | T::PaintConicGradient
        | T::PaintOuterBoxShadow
        | T::PaintInnerBoxShadow
        | T::FillRectWithRoundedCorners
        | T::FillRoundedRectRing
        | T::FillPath
        | T::StrokePath
        | T::DrawEllipse
        | T::DrawLine
        | T::BackdropFilterRegion
        | T::DrawRect
        | T::DeclareMaskContent
        | T::CompositorScrollNode
        | T::CompositorWheelHitTestTarget
        | T::CompositorWheelHitTestTargetWithCornerRadii
        | T::CompositorMainThreadWheelEventRegion
        | T::CompositorScrollbar
        | T::CompositorBlockingWheelEventRegion
        | T::PaintScrollBar
        | T::CompositorSnapContainer
        | T::CompositorSnapArea => {}
    }
}

// Visits every record of `records` and of the records nested in them, depth first.
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
        for_each_command(records, |header, offset, payload| {
            let indexed = IndexedRecord {
                run_index: run_index as u32,
                record_offset: (run_start + offset) as u32,
            };
            match header.command_type {
                DisplayListCommandType::DrawCanvas => summary.drawn_canvases.push(indexed),
                DisplayListCommandType::PaintCaret => summary.carets.push(indexed),
                _ => {}
            }
            note_referenced_ids(header.command_type, payload, &mut ids);
            for_each_nested_record_span(header.command_type, payload, |_, span| {
                for_each_record_including_nested(span_bytes(payload, span), &mut |command_type, payload| {
                    note_referenced_ids(command_type, payload, &mut ids);
                });
            });
        });
    }
    summary.font_ids = sorted(ids.fonts);
    summary.image_frame_ids = sorted(ids.image_frames);
    summary.video_sink_ids = sorted(ids.video_sinks);
    summary.display_list_ids = sorted(ids.display_lists);
    summary
}

// The blend a command composites with, and whether it blends with a backdrop color of its own instead of the
// destination.
fn command_blend(
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
        | T::PaintTextShadow
        | T::FillRectWithRoundedCorners
        | T::FillRoundedRectRing
        | T::StrokePath
        | T::DrawEllipse
        | T::DrawLine
        | T::BackdropFilterRegion
        | T::DrawRect
        | T::PaintNestedDisplayList
        | T::DeclareMaskContent
        | T::CompositorScrollNode
        | T::CompositorWheelHitTestTarget
        | T::CompositorWheelHitTestTargetWithCornerRadii
        | T::CompositorMainThreadWheelEventRegion
        | T::CompositorScrollbar
        | T::CompositorBlockingWheelEventRegion
        | T::PaintScrollBar
        | T::CompositorSnapContainer
        | T::CompositorSnapArea => None,
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

/// Whether reusing a raster of the list can produce different pixels than replaying it in place, leaving out the
/// display lists it nests. That is the case when a destination-reading operation can see content painted before the
/// list began: a non-normal blend on a command or a visual context effect, or a backdrop filter of an effect. A blend
/// inside a group recorded in the list reads only content of the list. It is also the case when the list draws live
/// content that changes under its immutable commands: video frames, canvases and composited child contexts.
pub fn requires_direct_replay_without_nested_lists(
    tape: &[u8],
    command_runs: &[DisplayListCommandRun],
    tree: &VisualContextTree,
) -> bool {
    if tree.has_unisolated_destination_reading_effect() {
        return true;
    }
    let mut requires_direct_replay = false;
    for run in command_runs {
        let records = &tape[run.offset as usize..(run.offset + run.size) as usize];
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
                && !tree.effect_is_isolated_by_layer(run.context.effect)
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
        if requires_direct_replay {
            return true;
        }
    }
    false
}

/// Visits the compositor metadata commands of the runs that hold any, in tape order.
pub fn for_each_compositor_metadata(
    tape: &[u8],
    command_runs: &[DisplayListCommandRun],
    mut visit: impl FnMut(ContextRef, DisplayListCommandType, &[u8]),
) {
    for run in command_runs.iter().filter(|run| run.has_compositor_metadata) {
        let records = &tape[run.offset as usize..(run.offset + run.size) as usize];
        for_each_command(records, |header, _, payload| {
            if header.command_type.is_compositor_metadata() {
                visit(run.context, header.command_type, payload);
            }
        });
    }
}

/// Visits indexed top-level records with their run's context, their header and their payload.
pub fn for_each_indexed_record(
    tape: &[u8],
    command_runs: &[DisplayListCommandRun],
    records: &[IndexedRecord],
    mut visit: impl FnMut(ContextRef, &DisplayListCommandHeader, &[u8]),
) {
    for record in records {
        let offset = record.record_offset as usize;
        let header = super::builder::read_header(&tape[offset..]);
        let payload_start = offset + HEADER_SIZE;
        visit(
            command_runs[record.run_index as usize].context,
            &header,
            &tape[payload_start..payload_start + header.payload_size as usize],
        );
    }
}
