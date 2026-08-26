/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::layout::node_data::NodeKind;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_facts;
use crate::painting::fragment_ownership;
use crate::painting::paintable_data::FragmentRecord;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::text_fragment;

fn push_class_name(out: &mut Vec<u8>, kind: NodeKind) {
    debug_assert!(kind != NodeKind::Unset, "Unset layout node kind has no class name");
    out.extend_from_slice(format!("{kind:?}").as_bytes());
}

fn push_usize(out: &mut Vec<u8>, value: usize) {
    out.extend_from_slice(value.to_string().as_bytes());
}

fn push_css_pixels(out: &mut Vec<u8>, value: CssPixels) {
    let raw = value.raw_value();
    if raw < 0 {
        out.push(b'-');
    }
    let magnitude = raw.unsigned_abs();
    out.extend_from_slice((magnitude / 64).to_string().as_bytes());
    let frac = magnitude % 64;
    if frac != 0 {
        let mut digits = format!("{:06}", u64::from(frac) * 15625).into_bytes();
        while digits.last() == Some(&b'0') {
            digits.pop();
        }
        out.push(b'.');
        out.extend_from_slice(&digits);
    }
}

fn push_code_point(out: &mut Vec<u8>, code_point: u32) {
    if code_point < 0x80 {
        out.push(code_point as u8);
    } else if code_point < 0x800 {
        out.push(0xC0 | (code_point >> 6) as u8);
        out.push(0x80 | (code_point & 0x3F) as u8);
    } else if code_point < 0x10000 {
        out.push(0xE0 | (code_point >> 12) as u8);
        out.push(0x80 | ((code_point >> 6) & 0x3F) as u8);
        out.push(0x80 | (code_point & 0x3F) as u8);
    } else {
        out.push(0xF0 | (code_point >> 18) as u8);
        out.push(0x80 | ((code_point >> 12) & 0x3F) as u8);
        out.push(0x80 | ((code_point >> 6) & 0x3F) as u8);
        out.push(0x80 | (code_point & 0x3F) as u8);
    }
}

fn push_text_wtf8(out: &mut Vec<u8>, code_units: &[u16]) {
    crate::css::serialize::for_each_code_point_utf16(code_units, |code_point| push_code_point(out, code_point));
}

fn push_indent(out: &mut Vec<u8>, indent: usize) {
    for _ in 0..indent {
        out.extend_from_slice(b"  ");
    }
}

fn dump_fragment(
    out: &mut Vec<u8>,
    layout_arena: &impl PaintableRowsRead,
    fragment: &FragmentRecord,
    fragment_index: usize,
    indent: usize,
    interactive: bool,
) {
    let (color_on, color_off): (&[u8], &[u8]) = if interactive {
        (b"\x1b[35;1m", b"\x1b[0m")
    } else {
        (b"", b"")
    };
    push_indent(out, indent);
    out.extend_from_slice(b"  ");
    out.extend_from_slice(color_on);
    out.extend_from_slice(b"frag ");
    push_usize(out, fragment_index);
    out.extend_from_slice(color_off);
    let Some(kind) = layout_arena.node_kind_if_live(fragment.layout_node) else {
        out.extend_from_slice(b" with detached layout node\n");
        return;
    };
    out.extend_from_slice(b" from ");
    push_class_name(out, kind);
    out.extend_from_slice(b" start: ");
    push_usize(out, fragment.start_offset);
    out.extend_from_slice(b", length: ");
    push_usize(out, fragment.length_in_code_units);
    out.extend_from_slice(b", rect: [");
    let rect = text_fragment::absolute_rect(layout_arena, fragment);
    push_css_pixels(out, rect.x);
    out.push(b',');
    push_css_pixels(out, rect.y);
    out.push(b' ');
    push_css_pixels(out, rect.width);
    out.push(b'x');
    push_css_pixels(out, rect.height);
    out.extend_from_slice(b"] baseline: ");
    push_css_pixels(out, fragment.baseline);
    out.push(b'\n');
    if fragment.length_in_code_units > 0 {
        push_indent(out, indent);
        out.extend_from_slice(b"      \"");
        if node_facts::kind_is_text(kind)
            && let Some(content) = layout_arena.text_content(fragment.layout_node)
        {
            let end = fragment
                .start_offset
                .saturating_add(fragment.length_in_code_units)
                .min(content.text.len());
            let start = fragment.start_offset.min(end);
            push_text_wtf8(out, &content.text[start..end]);
        }
        out.extend_from_slice(b"\"\n");
    }
}

pub(crate) fn dump_block_fragments(
    out: &mut Vec<u8>,
    layout_arena: &impl PaintableRowsRead,
    block: NodeSlotId,
    indent: usize,
    interactive: bool,
) {
    let mut fragment_index = 0usize;
    for fragment in &layout_arena.paintable_side_data(block).fragments {
        if layout_arena.node_kind_if_live(fragment.layout_node).is_some()
            && fragment_ownership::nearest_fragmented_inline_ancestor(layout_arena, fragment.layout_node).is_some()
        {
            continue;
        }
        dump_fragment(out, layout_arena, fragment, fragment_index, indent, interactive);
        fragment_index += 1;
    }
}

pub(crate) fn dump_inline_piece_fragments(
    out: &mut Vec<u8>,
    layout_arena: &impl PaintableRowsRead,
    inline_paintable: NodeSlotId,
    indent: usize,
    interactive: bool,
) {
    let Some(root) = layout_arena.inline_pieces_root(inline_paintable) else {
        return;
    };
    let root_side = layout_arena.paintable_side_data(root);
    for piece_index in &layout_arena.paintable_side_data(inline_paintable).piece_indices {
        let piece = &root_side.inline_box_pieces[*piece_index as usize];
        let mut fragment_index_within_piece = 0usize;
        for fragment_index in piece.first_fragment_index..piece.first_fragment_index + piece.fragment_count {
            let fragment = &root_side.fragments[fragment_index as usize];
            if layout_arena.node_kind_if_live(fragment.layout_node).is_some()
                && fragment_ownership::nearest_fragmented_inline_ancestor(layout_arena, fragment.layout_node)
                    != Some(piece.node)
            {
                continue;
            }
            dump_fragment(
                out,
                layout_arena,
                fragment,
                fragment_index_within_piece,
                indent,
                interactive,
            );
            fragment_index_within_piece += 1;
        }
    }
}
