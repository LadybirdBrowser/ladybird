/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::builder::{HEADER_SIZE, for_each_command, read_header};
use crate::painting::display_list::commands::*;
use crate::painting::record::RecordingOutput;
use crate::painting::record::trace::DamageSummary;

pub(crate) fn enabled_by_environment() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var_os("LADYBIRD_VERIFY_PAINT_CACHE").is_some_and(|value| value != "0"))
}

/// A range of the output attributed to one producer, or to one run of copied output.
#[derive(Clone, Copy, Debug)]
pub(crate) struct LoggedCapture {
    pub(crate) start: u32,
    pub(crate) length: u32,
    pub(crate) owner: NodeSlotId,
    pub(crate) label: &'static str,
    pub(crate) copied: bool,
}

#[derive(Default, Debug)]
pub struct CaptureLog {
    pub(crate) events: Vec<super::trace::Event>,
    pub(crate) open_events: Vec<usize>,
    pub(crate) damage: Option<DamageSummary>,
    pub(crate) command_byte_captures: Vec<LoggedCapture>,
    pub(crate) hit_test_item_captures: Vec<LoggedCapture>,
}

fn innermost_logged_capture_containing(records: &[LoggedCapture], position: usize) -> Option<LoggedCapture> {
    records
        .iter()
        .filter(|record| {
            let start = record.start as usize;
            position >= start && position < start + record.length as usize
        })
        .min_by_key(|record| record.length)
        .copied()
}

fn describe_enclosing_capture(records: &[LoggedCapture], position: usize) -> String {
    match innermost_logged_capture_containing(records, position) {
        Some(record) => format!(
            "{} of paintable {:?} ({}) at {}+{}",
            record.label,
            record.owner,
            if record.copied {
                "copied from the published frame"
            } else {
                "recorded"
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

fn zero_resource_ids_inside_span(payload: &mut [u8], span: DisplayListDataSpan) {
    let records = &mut payload[span.offset as usize..(span.offset + span.size) as usize];
    let mut offset = 0;
    while offset < records.len() {
        let header = read_header(&records[offset..]);
        let payload_start = offset + HEADER_SIZE;
        let payload_end = payload_start + header.payload_size as usize;
        zero_resource_ids_minted_per_recording(header.command_type, &mut records[payload_start..payload_end]);
        offset = payload_end;
    }
}

fn zero_resource_ids_minted_per_recording(command_type: DisplayListCommandType, payload: &mut [u8]) {
    if command_type == DisplayListCommandType::PaintNestedDisplayList {
        zero_field(
            payload,
            std::mem::offset_of!(PaintNestedDisplayList, display_list_id),
            std::mem::size_of::<DisplayListResourceId>(),
        );
    }
    let mut nested_spans = Vec::new();
    crate::painting::display_list::nested_records::for_each_nested_record_span(command_type, payload, |_, span| {
        nested_spans.push(span);
    });
    for span in nested_spans {
        zero_resource_ids_inside_span(payload, span);
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

pub(crate) fn verify_assembled_recording_matches_fresh(
    assembled_recording: &RecordingOutput,
    recording_from_scratch: &RecordingOutput,
) {
    let log = assembled_recording
        .capture_log_for_verification
        .as_ref()
        .expect("verification needs the capture log of the assembled recording");
    let assembled_bytes = &assembled_recording.display_list.bytes;
    let assembled_commands = decode_commands(assembled_bytes);
    let from_scratch_commands = decode_commands(&recording_from_scratch.display_list.bytes);

    for (index, (assembled_command, from_scratch_command)) in
        assembled_commands.iter().zip(&from_scratch_commands).enumerate()
    {
        let mut assembled_payload = assembled_command.payload.to_vec();
        let mut from_scratch_payload = from_scratch_command.payload.to_vec();
        zero_resource_ids_minted_per_recording(assembled_command.header.command_type, &mut assembled_payload);
        zero_resource_ids_minted_per_recording(from_scratch_command.header.command_type, &mut from_scratch_payload);
        if assembled_command.header == from_scratch_command.header && assembled_payload == from_scratch_payload {
            continue;
        }
        let first_differing_byte = assembled_payload
            .iter()
            .zip(&from_scratch_payload)
            .position(|(a, b)| a != b)
            .map_or("(payload sizes differ)".to_string(), |byte| {
                format!("payload byte {byte}")
            });
        panic!(
            "assembled recording verification failed: command #{index} at offset {} (assembled {:?}, from scratch {:?}) differs at {}\n  enclosing capture: {}\n  header assembled:     {:?}\n  header from scratch:  {:?}\n  payload assembled:    {}\n  payload from scratch: {}",
            assembled_command.offset,
            assembled_command.header.command_type,
            from_scratch_command.header.command_type,
            first_differing_byte,
            describe_enclosing_capture(&log.command_byte_captures, assembled_command.offset),
            assembled_command.header,
            from_scratch_command.header,
            hexdump(&assembled_payload),
            hexdump(&from_scratch_payload),
        );
    }
    if assembled_commands.len() != from_scratch_commands.len() {
        let offset = assembled_commands
            .get(from_scratch_commands.len())
            .map_or(assembled_bytes.len(), |command| command.offset);
        panic!(
            "assembled recording verification failed: assembled recording has {} commands, recording from scratch has {}; first extra command at offset {} ({})",
            assembled_commands.len(),
            from_scratch_commands.len(),
            offset,
            describe_enclosing_capture(
                &log.command_byte_captures,
                offset.min(assembled_bytes.len().saturating_sub(HEADER_SIZE))
            ),
        );
    }

    let assembled_items = &assembled_recording.hit_test_list.items;
    let from_scratch_items = &recording_from_scratch.hit_test_list.items;
    for (index, (assembled_item, from_scratch_item)) in
        assembled_items.iter().zip(from_scratch_items.iter()).enumerate()
    {
        if assembled_item == from_scratch_item {
            continue;
        }
        panic!(
            "assembled recording verification failed: hit-test item #{index} differs\n  enclosing capture: {}\n  assembled:     {assembled_item:?}\n  from scratch: {from_scratch_item:?}",
            describe_enclosing_capture(&log.hit_test_item_captures, index)
        );
    }
    assert_eq!(
        assembled_items.len(),
        from_scratch_items.len(),
        "assembled recording verification failed: hit-test item counts differ"
    );

    let region_count = |commands: &[DecodedCommand]| {
        commands
            .iter()
            .filter(|command| command.header.command_type == DisplayListCommandType::CompositorBlockingWheelEventRegion)
            .count()
    };
    assert_eq!(
        assembled_recording.has_blocking_wheel_event_listeners,
        recording_from_scratch.has_blocking_wheel_event_listeners,
        "assembled recording verification failed: blocking wheel event listener flag differs (assembled tape holds {} region commands, tape from scratch {})",
        region_count(&assembled_commands),
        region_count(&from_scratch_commands)
    );
}
