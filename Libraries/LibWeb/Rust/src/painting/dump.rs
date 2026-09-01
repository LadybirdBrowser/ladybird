/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::NodeKind;
use crate::layout::node_data::NodeSlotId;
use crate::layout::node_facts;
use crate::painting::fragment_ownership;
use crate::painting::paintable_data::FragmentRecord;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::text_fragment;
use libgfx_rust::{FloatPoint, FloatRect, FloatSize, IntPoint, IntRect, IntSize};
use std::fmt::Write;

fn push_class_name(out: &mut Vec<u8>, kind: NodeKind) {
    debug_assert!(kind != NodeKind::Unset, "Unset layout node kind has no class name");
    out.extend_from_slice(format!("{kind:?}").as_bytes());
}

fn push_usize(out: &mut Vec<u8>, value: usize) {
    out.extend_from_slice(value.to_string().as_bytes());
}

pub(crate) struct Utf8Sink<'a>(pub(crate) &'a mut Vec<u8>);

impl Write for Utf8Sink<'_> {
    fn write_str(&mut self, text: &str) -> std::fmt::Result {
        self.0.extend_from_slice(text.as_bytes());
        Ok(())
    }
}

pub(crate) fn push_css_pixels(output: &mut impl Write, value: CssPixels) {
    let raw = value.raw_value();
    if raw < 0 {
        let _ = output.write_char('-');
    }
    let magnitude = raw.unsigned_abs();
    let _ = write!(output, "{}", magnitude / 64);
    let frac = magnitude % 64;
    if frac != 0 {
        let mut digits = format!("{:06}", u64::from(frac) * 15625);
        while digits.ends_with('0') {
            digits.pop();
        }
        let _ = write!(output, ".{digits}");
    }
}

pub(crate) fn push_css_pixel_rect(output: &mut impl Write, rect: CssPixelRect) {
    let _ = output.write_char('[');
    push_css_pixels(output, rect.x);
    let _ = output.write_char(',');
    push_css_pixels(output, rect.y);
    let _ = output.write_char(' ');
    push_css_pixels(output, rect.width);
    let _ = output.write_char('x');
    push_css_pixels(output, rect.height);
    let _ = output.write_char(']');
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
    out.extend_from_slice(b", rect: ");
    push_css_pixel_rect(&mut Utf8Sink(out), text_fragment::absolute_rect(layout_arena, fragment));
    out.extend_from_slice(b" baseline: ");
    push_css_pixels(&mut Utf8Sink(out), fragment.baseline);
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

pub(crate) fn format_float_like_ak(value: f32) -> String {
    if value.is_nan() {
        return "nan".to_string();
    }
    if value.is_infinite() {
        return if value < 0.0 {
            "-inf".to_string()
        } else {
            "inf".to_string()
        };
    }
    if value == 0.0 {
        return "0".to_string();
    }
    // Dragonbox, which AK formats through, picks the same-length decimal closest to the value and
    // breaks an exact tie toward the even significand; Rust's shortest form breaks it the other
    // way. Re-rounding the shortest digit count through `{:.*e}` is correctly rounded ties-to-even,
    // so it reproduces Dragonbox's choice.
    let shortest = format!("{:e}", value.abs());
    let significant_digits = shortest
        .split_once('e')
        .expect("Rust scientific formatting always contains an exponent")
        .0
        .bytes()
        .filter(u8::is_ascii_digit)
        .count();
    let shortest_scientific = format!("{:.*e}", significant_digits - 1, value.abs());
    let (mantissa_text, exponent_text) = shortest_scientific
        .split_once('e')
        .expect("Rust scientific formatting always contains an exponent");
    let mantissa_digits: String = mantissa_text.chars().filter(|c| *c != '.').collect();
    let decimal_exponent: i32 = exponent_text.parse().expect("exponent is an integer");
    let exponent = decimal_exponent - (mantissa_digits.len() as i32 - 1);
    let n = exponent + mantissa_digits.len() as i32;
    let sign = if value < 0.0 { "-" } else { "" };

    // NOTE: Range from ECMA262, seems like an okay default.
    if !(-5..=21).contains(&n) {
        let exponent_sign = if n < 0 { '-' } else { '+' };
        let exponent_magnitude = (n - 1).abs();
        if mantissa_digits.len() == 1 {
            return format!("{sign}{mantissa_digits}e{exponent_sign}{exponent_magnitude}");
        }
        return format!(
            "{sign}{}.{}e{exponent_sign}{exponent_magnitude}",
            &mantissa_digits[..1],
            &mantissa_digits[1..]
        );
    }

    let mut text = String::from(sign);
    if exponent >= 0 {
        text.push_str(&mantissa_digits);
        for _ in 0..exponent {
            text.push('0');
        }
    } else if n > 0 {
        text.push_str(&mantissa_digits[..n as usize]);
        text.push('.');
        text.push_str(&mantissa_digits[n as usize..]);
    } else {
        text.push_str("0.");
        for _ in 0..(-n) {
            text.push('0');
        }
        text.push_str(&mantissa_digits);
    }
    text
}

pub(crate) fn push_float_like_ak(output: &mut String, value: f32) {
    output.push_str(&format_float_like_ak(value));
}

pub(crate) fn push_int_point(output: &mut String, point: IntPoint) {
    let _ = write!(output, "[{},{}]", point.x, point.y);
}

pub(crate) fn push_float_point(output: &mut String, point: FloatPoint) {
    output.push('[');
    push_float_like_ak(output, point.x);
    output.push(',');
    push_float_like_ak(output, point.y);
    output.push(']');
}

pub(crate) fn push_int_size(output: &mut String, size: IntSize) {
    let _ = write!(output, "[{}x{}]", size.width, size.height);
}

pub(crate) fn push_float_size(output: &mut String, size: FloatSize) {
    output.push('[');
    push_float_like_ak(output, size.width);
    output.push('x');
    push_float_like_ak(output, size.height);
    output.push(']');
}

pub(crate) fn push_int_rect_components(output: &mut String, rect: IntRect) {
    let _ = write!(output, "{},{} {}x{}", rect.x, rect.y, rect.width, rect.height);
}

pub(crate) fn push_int_rect(output: &mut String, rect: IntRect) {
    output.push('[');
    push_int_rect_components(output, rect);
    output.push(']');
}

pub(crate) fn push_float_rect(output: &mut String, rect: FloatRect) {
    output.push('[');
    push_float_like_ak(output, rect.x);
    output.push(',');
    push_float_like_ak(output, rect.y);
    output.push(' ');
    push_float_like_ak(output, rect.width);
    output.push('x');
    push_float_like_ak(output, rect.height);
    output.push(']');
}

#[cfg(test)]
mod tests {
    use super::format_float_like_ak;

    #[test]
    fn floats_format_like_ak() {
        assert_eq!(format_float_like_ak(0.8660254), "0.8660254");
        assert_eq!(format_float_like_ak(0.5), "0.5");
        assert_eq!(format_float_like_ak(20.0), "20");
        assert_eq!(format_float_like_ak(1.5), "1.5");
        assert_eq!(format_float_like_ak(-1.5), "-1.5");
        assert_eq!(format_float_like_ak(0.0), "0");
        assert_eq!(format_float_like_ak(-0.0), "0");
        assert_eq!(format_float_like_ak(100000.0), "100000");
        assert_eq!(format_float_like_ak(0.001), "0.001");
        assert_eq!(format_float_like_ak(1e-7), "1e-7");
        assert_eq!(format_float_like_ak(1.5e22), "1.5e+22");
        assert_eq!(format_float_like_ak(f32::INFINITY), "inf");
        assert_eq!(format_float_like_ak(f32::NEG_INFINITY), "-inf");
    }

    #[test]
    fn floats_break_ties_toward_the_even_significand_like_dragonbox() {
        assert_eq!(format_float_like_ak(134.390625), "134.39062");
        assert_eq!(format_float_like_ak(33555510.0), "33555510");
    }
}
