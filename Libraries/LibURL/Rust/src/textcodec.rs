/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use encoding_rs::EncoderResult;
use encoding_rs::Encoding;
use encoding_rs::REPLACEMENT;
use encoding_rs::UTF_8;
use encoding_rs::UTF_16BE;
use encoding_rs::UTF_16LE;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum EncodeItem {
    Byte(u8),
    Error(u32),
}

// https://encoding.spec.whatwg.org/#get-an-output-encoding
pub(crate) fn get_output_encoding(encoding: &'static Encoding) -> &'static Encoding {
    // 1. If encoding is replacement or UTF-16BE/LE, then return UTF-8.
    if encoding == REPLACEMENT || encoding == UTF_16BE || encoding == UTF_16LE {
        return UTF_8;
    }

    // 2. Return encoding.
    encoding
}

pub(crate) fn encode_into(encoding: &'static Encoding, input: &str, mut on_item: impl FnMut(EncodeItem)) -> bool {
    let mut encoder = encoding.new_encoder();
    let mut total_read = 0usize;
    let Some(output_capacity) = encoder.max_buffer_length_from_utf8_without_replacement(input.len()) else {
        return false;
    };
    let mut output = Vec::with_capacity(output_capacity);

    loop {
        let (result, read) =
            encoder.encode_from_utf8_to_vec_without_replacement(&input[total_read..], &mut output, true);
        total_read += read;

        for byte in output.drain(..) {
            on_item(EncodeItem::Byte(byte));
        }

        match result {
            EncoderResult::InputEmpty => return true,
            EncoderResult::OutputFull => return false,
            EncoderResult::Unmappable(unmappable) => on_item(EncodeItem::Error(unmappable as u32)),
        }
    }
}

pub(crate) fn encode_utf16_into(
    encoding: &'static Encoding,
    input: &[u16],
    mut on_item: impl FnMut(EncodeItem),
) -> bool {
    let mut encoder = encoding.new_encoder();
    let mut total_read = 0;
    let mut output = [0; 4096];
    loop {
        let (result, read, written) =
            encoder.encode_from_utf16_without_replacement(&input[total_read..], &mut output, true);
        total_read += read;
        for byte in &output[..written] {
            on_item(EncodeItem::Byte(*byte));
        }
        match result {
            EncoderResult::InputEmpty => return true,
            EncoderResult::OutputFull => {}
            EncoderResult::Unmappable(character) => on_item(EncodeItem::Error(character as u32)),
        }
    }
}
