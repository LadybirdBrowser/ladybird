/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::{HEADER_SIZE, for_each_command};
use crate::painting::display_list::commands::*;
use crate::painting::record::RecordingOutput;
use crate::painting::record::cache::CaptureKind;

pub(crate) fn enabled_by_environment() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var_os("LADYBIRD_VERIFY_PAINT_CACHE").is_some_and(|value| value != "0"))
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct LoggedCapture {
    pub(crate) start: u32,
    pub(crate) length: u32,
    pub(crate) paintable: NodeSlotId,
    pub(crate) kind: CaptureKind,
    pub(crate) spliced_from_cache: bool,
}

#[derive(Default, Debug)]
pub(crate) struct CaptureLog {
    pub(crate) command_byte_captures: Vec<LoggedCapture>,
    pub(crate) hit_test_item_captures: Vec<LoggedCapture>,
}

fn innermost_logged_capture_containing(records: &[LoggedCapture], position: usize) -> Option<LoggedCapture> {
    let contains_position = |record: &&LoggedCapture| {
        let start = record.start as usize;
        position >= start && position < start + record.length as usize
    };
    records
        .iter()
        .filter(|record| matches!(record.kind, CaptureKind::BoxPhase(_)))
        .filter(contains_position)
        .min_by_key(|record| record.length)
        .or_else(|| {
            records
                .iter()
                .filter(contains_position)
                .min_by_key(|record| record.length)
        })
        .copied()
}

fn describe_enclosing_capture(records: &[LoggedCapture], position: usize) -> String {
    match innermost_logged_capture_containing(records, position) {
        Some(record) => format!(
            "{:?} of paintable {:?} ({}) at {}+{}",
            record.kind,
            record.paintable,
            if record.spliced_from_cache {
                "spliced from cache"
            } else {
                "recorded from scratch"
            },
            record.start,
            record.length
        ),
        None => "outside every capture".to_string(),
    }
}

fn zero_field(payload: &mut [u8], offset: usize, size: usize) {
    if offset + size <= payload.len() {
        payload[offset..offset + size].fill(0);
    }
}

fn zero_resource_ids_minted_per_recording(
    command_type: DisplayListCommandType,
    enclosing_capture_is_video: bool,
    payload: &mut [u8],
) {
    let id_size = std::mem::size_of::<DisplayListResourceId>();
    match command_type {
        DisplayListCommandType::PaintNestedDisplayList => {
            zero_field(
                payload,
                std::mem::offset_of!(PaintNestedDisplayList, display_list_id),
                id_size,
            );
        }
        DisplayListCommandType::DrawIsolatedDisplayList => {
            zero_field(
                payload,
                std::mem::offset_of!(DrawIsolatedDisplayList, display_list_id),
                id_size,
            );
            zero_field(
                payload,
                std::mem::offset_of!(DrawIsolatedDisplayList, mask_display_list_id),
                id_size,
            );
        }
        DisplayListCommandType::DrawRepeatedDisplayList => {
            zero_field(
                payload,
                std::mem::offset_of!(DrawRepeatedDisplayList, display_list_id),
                id_size,
            );
        }
        DisplayListCommandType::FillPath => {
            zero_field(
                payload,
                std::mem::offset_of!(FillPath, paint_style)
                    + std::mem::offset_of!(DisplayListPaintStyle, pattern_tile_display_list_id),
                id_size,
            );
        }
        DisplayListCommandType::StrokePath => {
            zero_field(
                payload,
                std::mem::offset_of!(StrokePath, paint_style)
                    + std::mem::offset_of!(DisplayListPaintStyle, pattern_tile_display_list_id),
                id_size,
            );
        }
        DisplayListCommandType::DrawScaledDecodedImageFrame if enclosing_capture_is_video => {
            zero_field(
                payload,
                std::mem::offset_of!(DrawScaledDecodedImageFrame, frame_id),
                std::mem::size_of::<ImageFrameResourceId>(),
            );
        }
        _ => {}
    }
}

struct DecodedCommand<'a> {
    header: DisplayListCommandHeader,
    offset: usize,
    payload: &'a [u8],
}

fn decode_commands(bytes: &[u8]) -> Vec<DecodedCommand<'_>> {
    let mut commands = Vec::new();
    for_each_command(bytes, |header, offset, payload| {
        commands.push(DecodedCommand {
            header: *header,
            offset,
            payload,
        });
    });
    commands
}

fn hexdump(bytes: &[u8]) -> String {
    bytes
        .iter()
        .map(|byte| format!("{byte:02x}"))
        .collect::<Vec<_>>()
        .join(" ")
}

pub(crate) fn verify_spliced_recording_matches_fresh(
    arena: &LayoutNodeArena,
    recording_with_splices: &RecordingOutput,
    recording_from_scratch: &RecordingOutput,
) {
    let log = recording_with_splices
        .capture_log_for_verification
        .as_ref()
        .expect("verification needs the capture log of the recording with splices");
    let with_splices_bytes = &recording_with_splices.display_list.bytes;
    let with_splices_commands = decode_commands(with_splices_bytes);
    let from_scratch_commands = decode_commands(&recording_from_scratch.display_list.bytes);
    let enclosing_capture_is_video = |offset: usize| {
        innermost_logged_capture_containing(&log.command_byte_captures, offset)
            .is_some_and(|record| arena.node_kind_if_live(record.paintable) == Some(NodeKind::VideoBox))
    };

    for (index, (with_splices_command, from_scratch_command)) in
        with_splices_commands.iter().zip(&from_scratch_commands).enumerate()
    {
        let is_video = enclosing_capture_is_video(with_splices_command.offset);
        let mut with_splices_payload = with_splices_command.payload.to_vec();
        let mut from_scratch_payload = from_scratch_command.payload.to_vec();
        zero_resource_ids_minted_per_recording(
            with_splices_command.header.command_type,
            is_video,
            &mut with_splices_payload,
        );
        zero_resource_ids_minted_per_recording(
            from_scratch_command.header.command_type,
            is_video,
            &mut from_scratch_payload,
        );
        if with_splices_command.header == from_scratch_command.header && with_splices_payload == from_scratch_payload {
            continue;
        }
        let first_differing_byte = with_splices_payload
            .iter()
            .zip(&from_scratch_payload)
            .position(|(a, b)| a != b)
            .map_or("(payload sizes differ)".to_string(), |byte| {
                format!("payload byte {byte}")
            });
        panic!(
            "paint cache verification failed: command #{index} at offset {} (with splices {:?}, from scratch {:?}) differs at {}\n  enclosing capture: {}\n  header with splices:  {:?}\n  header from scratch:  {:?}\n  payload with splices: {}\n  payload from scratch: {}",
            with_splices_command.offset,
            with_splices_command.header.command_type,
            from_scratch_command.header.command_type,
            first_differing_byte,
            describe_enclosing_capture(&log.command_byte_captures, with_splices_command.offset),
            with_splices_command.header,
            from_scratch_command.header,
            hexdump(&with_splices_payload),
            hexdump(&from_scratch_payload),
        );
    }
    if with_splices_commands.len() != from_scratch_commands.len() {
        let offset = with_splices_commands
            .get(from_scratch_commands.len())
            .map_or(with_splices_bytes.len(), |command| command.offset);
        panic!(
            "paint cache verification failed: recording with splices has {} commands, recording from scratch has {}; first extra command at offset {} ({})",
            with_splices_commands.len(),
            from_scratch_commands.len(),
            offset,
            describe_enclosing_capture(
                &log.command_byte_captures,
                offset.min(with_splices_bytes.len().saturating_sub(HEADER_SIZE))
            ),
        );
    }

    let with_splices_items = &recording_with_splices.hit_test_list.items;
    let from_scratch_items = &recording_from_scratch.hit_test_list.items;
    for (index, (with_splices_item, from_scratch_item)) in
        with_splices_items.iter().zip(from_scratch_items.iter()).enumerate()
    {
        if with_splices_item == from_scratch_item {
            continue;
        }
        panic!(
            "paint cache verification failed: hit-test item #{index} differs\n  enclosing capture: {}\n  with splices:  {with_splices_item:?}\n  from scratch: {from_scratch_item:?}",
            describe_enclosing_capture(&log.hit_test_item_captures, index)
        );
    }
    assert_eq!(
        with_splices_items.len(),
        from_scratch_items.len(),
        "paint cache verification failed: hit-test item counts differ"
    );

    let with_splices_mask_frames: Vec<FrameNodeIndex> = recording_with_splices
        .mask_display_lists
        .iter()
        .map(|registration| registration.frame)
        .collect();
    let from_scratch_mask_frames: Vec<FrameNodeIndex> = recording_from_scratch
        .mask_display_lists
        .iter()
        .map(|registration| registration.frame)
        .collect();
    assert_eq!(
        with_splices_mask_frames, from_scratch_mask_frames,
        "paint cache verification failed: mask display list registrations differ"
    );
    let region_count = |commands: &[DecodedCommand]| {
        commands
            .iter()
            .filter(|command| command.header.command_type == DisplayListCommandType::CompositorBlockingWheelEventRegion)
            .count()
    };
    assert_eq!(
        recording_with_splices.has_blocking_wheel_event_listeners,
        recording_from_scratch.has_blocking_wheel_event_listeners,
        "paint cache verification failed: blocking wheel event listener flag differs (tape with splices holds {} region commands, tape from scratch {})",
        region_count(&with_splices_commands),
        region_count(&from_scratch_commands)
    );
}
