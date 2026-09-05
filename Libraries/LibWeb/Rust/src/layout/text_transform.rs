/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::rendered_text::{RenderedTextBoundary, RenderedTextEdit, rendered_text_offset_for_dom_offset};
use super::text_chunker::{code_point_at, code_unit_length_for_code_point};
use crate::css::css_enums::{text_transform, white_space_collapse};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct TextRenderingOptions {
    pub text_transform: u8,
    pub white_space_collapse: u8,
    pub is_password_input: bool,
    pub dom_start_offset: usize,
    pub dom_length_in_code_units: usize,
}

#[derive(Default)]
pub(super) struct TransformedText {
    pub text: Vec<u16>,
    pub edits: Vec<RenderedTextEdit>,
}

impl TransformedText {
    fn append_edit(&mut self, dom_start: usize, dom_length: usize, rendered_start: usize, rendered_length: usize) {
        if dom_length == rendered_length {
            return;
        }
        if rendered_length == 0
            && let Some(previous) = self.edits.last_mut()
            && previous.rendered_length_in_code_units == 0
            && previous.dom_start_offset + previous.dom_length_in_code_units == dom_start
            && previous.rendered_start_offset == rendered_start
        {
            previous.dom_length_in_code_units += dom_length;
            return;
        }
        self.edits.push(RenderedTextEdit {
            dom_start_offset: dom_start,
            dom_length_in_code_units: dom_length,
            rendered_start_offset: rendered_start,
            rendered_length_in_code_units: rendered_length,
        });
    }

    fn slice(&mut self, dom_start: usize, dom_length: usize) {
        let dom_end = dom_start + dom_length;
        let rendered_start =
            rendered_text_offset_for_dom_offset(&self.edits, 0, dom_start, RenderedTextBoundary::Start);
        let rendered_end = rendered_text_offset_for_dom_offset(&self.edits, 0, dom_end, RenderedTextBoundary::End);
        self.text.copy_within(rendered_start..rendered_end, 0);
        self.text.truncate(rendered_end - rendered_start);
        self.edits.retain_mut(|edit| {
            let edit_dom_end = edit.dom_start_offset + edit.dom_length_in_code_units;
            if edit_dom_end <= dom_start || edit.dom_start_offset >= dom_end {
                return false;
            }
            let edit_rendered_end = edit.rendered_start_offset + edit.rendered_length_in_code_units;
            let sliced_dom_start = edit.dom_start_offset.max(dom_start);
            let sliced_dom_end = edit_dom_end.min(dom_end);
            let sliced_rendered_start = edit.rendered_start_offset.max(rendered_start);
            let sliced_rendered_end = edit_rendered_end.min(rendered_end);
            *edit = RenderedTextEdit {
                dom_start_offset: sliced_dom_start,
                dom_length_in_code_units: sliced_dom_end - sliced_dom_start,
                rendered_start_offset: sliced_rendered_start - rendered_start,
                rendered_length_in_code_units: sliced_rendered_end - sliced_rendered_start,
            };
            edit.dom_length_in_code_units != edit.rendered_length_in_code_units
        });
    }
}

// This sink lends Rust-owned buffers to the Unicode library for one synchronous
// transform. The library does not know about layout nodes, styles, or the arena.
#[repr(C)]
struct UnicodeTextMappingOutput {
    context: *mut c_void,
    allocate_text: unsafe extern "C" fn(*mut c_void, usize) -> *mut u16,
    append_edit: unsafe extern "C" fn(*mut c_void, usize, usize, usize, usize),
}

unsafe extern "C" {
    fn unicode_apply_case_mapping(
        text: *const u16,
        length: usize,
        mapping: u8,
        locale: *const u16,
        locale_length: usize,
        preserve_existing: bool,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_apply_fullwidth_mapping(text: *const u16, length: usize, output: UnicodeTextMappingOutput);
    fn unicode_text_may_require_bidi_processing(text: *const u16, length: usize) -> bool;
}

unsafe extern "C" fn allocate_text(context: *mut c_void, length: usize) -> *mut u16 {
    // SAFETY: unicode_mapping lends this result exclusively to its synchronous callbacks.
    let result = unsafe { &mut *context.cast::<TransformedText>() };
    result.text.resize(length, 0);
    result.text.as_mut_ptr()
}

unsafe extern "C" fn append_edit(
    context: *mut c_void,
    start: usize,
    length: usize,
    mapped_start: usize,
    mapped_length: usize,
) {
    // SAFETY: As above. This callback never moves or accesses the lent text buffer.
    let result = unsafe { &mut *context.cast::<TransformedText>() };
    result.append_edit(start, length, mapped_start, mapped_length);
}

fn unicode_mapping(source: &[u16], transform: u8, locale: Option<&[u16]>) -> TransformedText {
    let mut result = TransformedText::default();
    let output = UnicodeTextMappingOutput {
        context: std::ptr::from_mut(&mut result).cast(),
        allocate_text,
        append_edit,
    };
    if transform == text_transform::FULL_WIDTH {
        // SAFETY: Source and result remain live throughout this synchronous call.
        unsafe { unicode_apply_fullwidth_mapping(source.as_ptr(), source.len(), output) };
    } else {
        // These values mirror Unicode::CaseMapping in LibUnicode/TextMapping.h.
        let mapping = match transform {
            text_transform::LOWERCASE => 0,
            text_transform::UPPERCASE => 1,
            text_transform::CAPITALIZE => 2,
            _ => unreachable!("not a Unicode casing transform"),
        };
        let (locale_pointer, locale_length) =
            locale.map_or((std::ptr::null(), 0), |locale| (locale.as_ptr(), locale.len()));
        // SAFETY: Input views and the exclusive output stay live for the call.
        unsafe {
            unicode_apply_case_mapping(
                source.as_ptr(),
                source.len(),
                mapping,
                locale_pointer,
                locale_length,
                transform == text_transform::CAPITALIZE,
                output,
            );
        };
    }
    result
}

pub(super) fn may_require_bidi_processing(text: &[u16]) -> bool {
    if text.iter().all(|unit| *unit <= 0x7f) {
        return false;
    }
    // SAFETY: The Unicode lookup only reads this borrowed text during the call.
    unsafe { unicode_text_may_require_bidi_processing(text.as_ptr(), text.len()) }
}

fn map_code_points(source: &[u16], mapping: impl Fn(u32) -> u32) -> TransformedText {
    let mut result = TransformedText {
        text: Vec::with_capacity(source.len()),
        edits: Vec::new(),
    };
    let mut offset = 0;
    while offset < source.len() {
        let code_point = code_point_at(source, offset);
        let source_length = code_unit_length_for_code_point(code_point);
        let mapped = mapping(code_point);
        result.append_edit(
            offset,
            source_length,
            result.text.len(),
            code_unit_length_for_code_point(mapped),
        );
        if mapped < 0x10000 {
            // Preserve lone surrogates, as AK::Utf16View does.
            result.text.push(mapped as u16);
        } else {
            let value = mapped - 0x10000;
            result.text.push(0xd800 + (value >> 10) as u16);
            result.text.push(0xdc00 + (value & 0x3ff) as u16);
        }
        offset += source_length;
    }
    result
}

fn normalize_whitespace(text: &mut [u16], collapse: u8) {
    let convert_newlines = matches!(
        collapse,
        white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_SPACES
    );
    let convert_tabs = matches!(
        collapse,
        white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS | white_space_collapse::PRESERVE_SPACES
    );
    // Keep code-unit offsets unchanged. The chunker collapses repeated spaces.
    for unit in text {
        if (convert_newlines && *unit == u16::from(b'\n')) || (convert_tabs && *unit == u16::from(b'\t')) {
            *unit = u16::from(b' ');
        }
    }
}

pub(super) fn render_text(
    mut source: Vec<u16>,
    locale: Option<&[u16]>,
    options: TextRenderingOptions,
) -> TransformedText {
    let source_length = source.len();
    assert!(options.dom_start_offset <= source_length);
    assert!(options.dom_length_in_code_units <= source_length - options.dom_start_offset);
    let mut result = if options.is_password_input {
        map_code_points(&source, |_| 0x25cf)
    } else {
        match options.text_transform {
            text_transform::NONE | text_transform::FULL_SIZE_KANA => {
                // FIXME: Implement full-size-kana.
                TransformedText {
                    text: source,
                    edits: Vec::new(),
                }
            }
            text_transform::MATH_AUTO => map_code_points(&source, math_auto),
            text_transform::LOWERCASE | text_transform::UPPERCASE
                if locale.is_none() && source.iter().all(|unit| *unit <= 0x7f) =>
            {
                for unit in &mut source {
                    let byte = *unit as u8;
                    *unit = u16::from(if options.text_transform == text_transform::LOWERCASE {
                        byte.to_ascii_lowercase()
                    } else {
                        byte.to_ascii_uppercase()
                    });
                }
                TransformedText {
                    text: source,
                    edits: Vec::new(),
                }
            }
            text_transform::LOWERCASE
            | text_transform::UPPERCASE
            | text_transform::CAPITALIZE
            | text_transform::FULL_WIDTH => unicode_mapping(&source, options.text_transform, locale),
            _ => unreachable!("invalid text-transform value"),
        }
    };
    // Transform the full source before slicing so contextual casing sees the
    // same surrounding text for both ::first-letter and the remainder.
    if options.dom_start_offset != 0 || options.dom_length_in_code_units != source_length {
        result.slice(options.dom_start_offset, options.dom_length_in_code_units);
    }
    normalize_whitespace(&mut result.text, options.white_space_collapse);
    result
}

// https://w3c.github.io/mathml-core/#italic-mappings
fn math_auto(code_point: u32) -> u32 {
    match code_point {
        0x0041 => 0x1d434,
        0x0042 => 0x1d435,
        0x0043 => 0x1d436,
        0x0044 => 0x1d437,
        0x0045 => 0x1d438,
        0x0046 => 0x1d439,
        0x0047 => 0x1d43a,
        0x0048 => 0x1d43b,
        0x0049 => 0x1d43c,
        0x004a => 0x1d43d,
        0x004b => 0x1d43e,
        0x004c => 0x1d43f,
        0x004d => 0x1d440,
        0x004e => 0x1d441,
        0x004f => 0x1d442,
        0x0050 => 0x1d443,
        0x0051 => 0x1d444,
        0x0052 => 0x1d445,
        0x0053 => 0x1d446,
        0x0054 => 0x1d447,
        0x0055 => 0x1d448,
        0x0056 => 0x1d449,
        0x0057 => 0x1d44a,
        0x0058 => 0x1d44b,
        0x0059 => 0x1d44c,
        0x005a => 0x1d44d,
        0x0061 => 0x1d44e,
        0x0062 => 0x1d44f,
        0x0063 => 0x1d450,
        0x0064 => 0x1d451,
        0x0065 => 0x1d452,
        0x0066 => 0x1d453,
        0x0067 => 0x1d454,
        0x0068 => 0x0210e,
        0x0069 => 0x1d456,
        0x006a => 0x1d457,
        0x006b => 0x1d458,
        0x006c => 0x1d459,
        0x006d => 0x1d45a,
        0x006e => 0x1d45b,
        0x006f => 0x1d45c,
        0x0070 => 0x1d45d,
        0x0071 => 0x1d45e,
        0x0072 => 0x1d45f,
        0x0073 => 0x1d460,
        0x0074 => 0x1d461,
        0x0075 => 0x1d462,
        0x0076 => 0x1d463,
        0x0077 => 0x1d464,
        0x0078 => 0x1d465,
        0x0079 => 0x1d466,
        0x007a => 0x1d467,
        0x0131 => 0x1d6a4,
        0x0237 => 0x1d6a5,
        0x0391 => 0x1d6e2,
        0x0392 => 0x1d6e3,
        0x0393 => 0x1d6e4,
        0x0394 => 0x1d6e5,
        0x0395 => 0x1d6e6,
        0x0396 => 0x1d6e7,
        0x0397 => 0x1d6e8,
        0x0398 => 0x1d6e9,
        0x0399 => 0x1d6ea,
        0x039a => 0x1d6eb,
        0x039b => 0x1d6ec,
        0x039c => 0x1d6ed,
        0x039d => 0x1d6ee,
        0x039e => 0x1d6ef,
        0x039f => 0x1d6f0,
        0x03a0 => 0x1d6f1,
        0x03a1 => 0x1d6f2,
        0x03f4 => 0x1d6f3,
        0x03a3 => 0x1d6f4,
        0x03a4 => 0x1d6f5,
        0x03a5 => 0x1d6f6,
        0x03a6 => 0x1d6f7,
        0x03a7 => 0x1d6f8,
        0x03a8 => 0x1d6f9,
        0x03a9 => 0x1d6fa,
        0x2207 => 0x1d6fb,
        0x03b1 => 0x1d6fc,
        0x03b2 => 0x1d6fd,
        0x03b3 => 0x1d6fe,
        0x03b4 => 0x1d6ff,
        0x03b5 => 0x1d700,
        0x03b6 => 0x1d701,
        0x03b7 => 0x1d702,
        0x03b8 => 0x1d703,
        0x03b9 => 0x1d704,
        0x03ba => 0x1d705,
        0x03bb => 0x1d706,
        0x03bc => 0x1d707,
        0x03bd => 0x1d708,
        0x03be => 0x1d709,
        0x03bf => 0x1d70a,
        0x03c0 => 0x1d70b,
        0x03c1 => 0x1d70c,
        0x03c2 => 0x1d70d,
        0x03c3 => 0x1d70e,
        0x03c4 => 0x1d70f,
        0x03c5 => 0x1d710,
        0x03c6 => 0x1d711,
        0x03c7 => 0x1d712,
        0x03c8 => 0x1d713,
        0x03c9 => 0x1d714,
        0x2202 => 0x1d715,
        0x03f5 => 0x1d716,
        0x03d1 => 0x1d717,
        0x03f0 => 0x1d718,
        0x03d5 => 0x1d719,
        0x03f1 => 0x1d71a,
        0x03d6 => 0x1d71b,
        _ => code_point,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn password_masking_counts_code_points_and_preserves_dom_spans() {
        let source = [0x61, 0xd83d, 0xde00, 0xd800, 0x62];
        let masked = map_code_points(&source, |_| 0x25cf);
        assert_eq!(masked.text, [0x25cf; 4]);
        assert_eq!(
            masked.edits,
            [RenderedTextEdit {
                dom_start_offset: 1,
                dom_length_in_code_units: 2,
                rendered_start_offset: 1,
                rendered_length_in_code_units: 1,
            }]
        );
    }

    #[test]
    fn math_auto_expansion_and_slicing_keep_absolute_dom_offsets() {
        let mut mapped = map_code_points(&[u16::from(b'A'), u16::from(b'h'), 0xd800], math_auto);
        assert_eq!(mapped.text, [0xd835, 0xdc34, 0x210e, 0xd800]);
        mapped.slice(1, 2);
        assert_eq!(mapped.text, [0x210e, 0xd800]);
        assert!(mapped.edits.is_empty());
    }

    #[test]
    fn normalization_preserves_offsets_and_preserved_whitespace() {
        for (collapse, expected) in [
            (white_space_collapse::COLLAPSE, "a  b\r"),
            (white_space_collapse::PRESERVE_BREAKS, "a \nb\r"),
            (white_space_collapse::PRESERVE_SPACES, "a  b\r"),
            (white_space_collapse::PRESERVE, "a\t\nb\r"),
            (white_space_collapse::BREAK_SPACES, "a\t\nb\r"),
        ] {
            let mut text = "a\t\nb\r".encode_utf16().collect::<Vec<_>>();
            normalize_whitespace(&mut text, collapse);
            assert_eq!(text, expected.encode_utf16().collect::<Vec<_>>());
        }
    }

    #[test]
    fn unicode_output_writes_into_rust_storage_and_coalesces_deletions() {
        let mut output = TransformedText::default();
        let context = std::ptr::from_mut(&mut output).cast();
        // SAFETY: The local result outlives the callbacks and the buffer write.
        unsafe {
            let text = allocate_text(context, 1);
            text.write(u16::from(b'i'));
            append_edit(context, 1, 1, 1, 0);
            append_edit(context, 2, 1, 1, 0);
        }
        assert_eq!(output.text, [u16::from(b'i')]);
        assert_eq!(output.edits.len(), 1);
        assert_eq!(output.edits[0].dom_length_in_code_units, 2);
    }
}
