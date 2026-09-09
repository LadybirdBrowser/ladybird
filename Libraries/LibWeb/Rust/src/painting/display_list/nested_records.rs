/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::builder::{HEADER_SIZE, for_each_command, read_command};
use super::commands::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum NestedRecordsRole {
    IsolatedGroupContent,
    IsolatedGroupMask,
    RepeatedTile,
    MaskContent,
    PatternTile,
}

pub(crate) fn for_each_nested_record_span(
    command_type: DisplayListCommandType,
    payload: &[u8],
    mut visit: impl FnMut(NestedRecordsRole, DisplayListDataSpan),
) {
    let mut visit_pattern_tile = |paint_kind: PathPaintKind, paint_style: DisplayListPaintStyle| {
        if paint_kind == PathPaintKind::PaintStyle && paint_style.paint_style_type == DisplayListPaintStyleType::Pattern
        {
            visit(NestedRecordsRole::PatternTile, paint_style.pattern_tile);
        }
    };
    match command_type {
        DisplayListCommandType::DrawIsolatedGroup => {
            let command = read_command::<DrawIsolatedGroup>(payload);
            visit(NestedRecordsRole::IsolatedGroupContent, command.content);
            if command.mask.size != 0 {
                visit(NestedRecordsRole::IsolatedGroupMask, command.mask);
            }
        }
        DisplayListCommandType::DrawRepeatedTile => {
            visit(
                NestedRecordsRole::RepeatedTile,
                read_command::<DrawRepeatedTile>(payload).tile,
            );
        }
        DisplayListCommandType::DeclareMaskContent => {
            visit(
                NestedRecordsRole::MaskContent,
                read_command::<DeclareMaskContent>(payload).content,
            );
        }
        DisplayListCommandType::FillPath => {
            let command = read_command::<FillPath>(payload);
            visit_pattern_tile(command.paint_kind, command.paint_style);
        }
        DisplayListCommandType::StrokePath => {
            let command = read_command::<StrokePath>(payload);
            visit_pattern_tile(command.paint_kind, command.paint_style);
        }
        _ => {}
    }
}

pub(crate) fn span_bytes(payload: &[u8], span: DisplayListDataSpan) -> &[u8] {
    &payload[span.offset as usize..(span.offset + span.size) as usize]
}

pub(crate) fn for_each_command_including_nested(
    bytes: &[u8],
    visit: &mut impl FnMut(DisplayListCommandType, usize, &[u8]),
) {
    for_each_command_including_nested_at(bytes, 0, visit);
}

fn for_each_command_including_nested_at(
    bytes: &[u8],
    base_offset: usize,
    visit: &mut impl FnMut(DisplayListCommandType, usize, &[u8]),
) {
    for_each_command(bytes, |header, record_offset, payload| {
        let payload_offset = base_offset + record_offset + HEADER_SIZE;
        visit(header.command_type, payload_offset, payload);
        for_each_nested_record_span(header.command_type, payload, |_, span| {
            for_each_command_including_nested_at(
                span_bytes(payload, span),
                payload_offset + span.offset as usize,
                visit,
            );
        });
    });
}
