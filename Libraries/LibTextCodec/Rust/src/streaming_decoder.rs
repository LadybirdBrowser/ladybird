/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Decoding a stream of bytes with the decoders of the Encoding Standard, https://encoding.spec.whatwg.org/#encodings,
//! which the encoding_rs crate implements.

use encoding_rs::{CoderResult, Decoder, DecoderResult, Encoding};

#[derive(Debug, PartialEq, Eq)]
pub struct DecodingFailed;

/// The decoder of an encoding together with the error mode and the BOM handling that it decodes with, which keeps
/// the bytes of a character that a chunk of a stream ended in until the next chunk.
pub struct StreamingDecoder {
    encoding: &'static Encoding,
    fatal: bool,
    ignore_bom: bool,
    bom_seen: bool,
    decoder: Decoder,
}

impl StreamingDecoder {
    pub fn new(encoding: &'static Encoding, fatal: bool, ignore_bom: bool) -> Self {
        Self {
            encoding,
            fatal,
            ignore_bom,
            bom_seen: false,
            decoder: encoding.new_decoder_without_bom_handling(),
        }
    }

    pub fn encoding(&self) -> &'static Encoding {
        self.encoding
    }

    pub fn fatal(&self) -> bool {
        self.fatal
    }

    pub fn ignore_bom(&self) -> bool {
        self.ignore_bom
    }

    /// Decodes the next bytes of the input to UTF-16. Unless more of the input follows, the bytes are its end: the
    /// decoder then flushes what it kept, and starts over for the next input, whether or not decoding succeeded.
    /// A failure discards the rest of the bytes, and with them whatever the decoder kept of them.
    pub fn decode(&mut self, input: &[u8], more_input_follows: bool) -> Result<Vec<u16>, DecodingFailed> {
        let mut decoded = self.decode_with_current_decoder(input, !more_input_follows);
        match &mut decoded {
            Ok(output) => self.remove_bom_from_output(output),
            Err(DecodingFailed) => self.forget_rest_of_input(),
        }
        if !more_input_follows {
            self.start_over();
        }
        decoded
    }

    // https://encoding.spec.whatwg.org/#concept-td-serialize
    fn remove_bom_from_output(&mut self, output: &mut Vec<u16>) {
        // NB: The BOM is removed from the output of a call that succeeded, rather than from the input, so that a call
        //     that fails does not consume it.
        if self.bom_seen || output.is_empty() {
            return;
        }
        self.bom_seen = true;
        let is_unicode_encoding = self.encoding == encoding_rs::UTF_8
            || self.encoding == encoding_rs::UTF_16BE
            || self.encoding == encoding_rs::UTF_16LE;
        if !self.ignore_bom && is_unicode_encoding && output[0] == 0xFEFF {
            output.remove(0);
        }
    }

    fn start_over(&mut self) {
        // NB: encoding_rs finishes a decoder for good once it has seen the end of its input.
        self.decoder = self.encoding.new_decoder_without_bom_handling();
        self.bom_seen = false;
    }

    fn forget_rest_of_input(&mut self) {
        // NB: Every decoder but ISO-2022-JP's resets its own state on an error, so a new decoder only drops the bytes
        //     encoding_rs kept of the input that the error discards. ISO-2022-JP's mode outlasts its errors.
        if self.encoding != encoding_rs::ISO_2022_JP {
            self.decoder = self.encoding.new_decoder_without_bom_handling();
        }
    }

    fn decode_with_current_decoder(&mut self, input: &[u8], last: bool) -> Result<Vec<u16>, DecodingFailed> {
        let output_capacity = self
            .decoder
            .max_utf16_buffer_length(input.len())
            .ok_or(DecodingFailed)?;
        let mut output = vec![0; output_capacity];

        let (input_was_consumed, written) = if self.fatal {
            let (result, _, written) = self
                .decoder
                .decode_to_utf16_without_replacement(input, &mut output, last);
            (matches!(result, DecoderResult::InputEmpty), written)
        } else {
            let (result, _, written, _) = self.decoder.decode_to_utf16(input, &mut output, last);
            (matches!(result, CoderResult::InputEmpty), written)
        };
        if !input_was_consumed {
            return Err(DecodingFailed);
        }
        output.truncate(written);
        Ok(output)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf8_decoder(fatal: bool, ignore_bom: bool) -> StreamingDecoder {
        StreamingDecoder::new(encoding_rs::UTF_8, fatal, ignore_bom)
    }

    fn utf16(string: &str) -> Vec<u16> {
        string.encode_utf16().collect()
    }

    #[test]
    fn a_character_split_over_chunks() {
        let mut decoder = utf8_decoder(false, false);
        assert_eq!(decoder.decode(&[0x61, 0xE2, 0x82], true), Ok(utf16("a")));
        assert_eq!(decoder.decode(&[0xAC], false), Ok(utf16("\u{20AC}")));
    }

    #[test]
    fn the_end_of_the_input_flushes_and_starts_over() {
        let mut decoder = utf8_decoder(false, false);
        assert_eq!(decoder.decode(&[0x61, 0xE2, 0x82], false), Ok(utf16("a\u{FFFD}")));
        assert_eq!(decoder.decode(&[0xAC], false), Ok(utf16("\u{FFFD}")));
        assert_eq!(decoder.decode(&[], false), Ok(Vec::new()));
    }

    #[test]
    fn a_byte_order_mark_is_removed_once_for_every_input() {
        let mut decoder = utf8_decoder(false, false);
        assert_eq!(decoder.decode(&[0xEF, 0xBB, 0xBF, 0x61], true), Ok(utf16("a")));
        assert_eq!(decoder.decode(&[0xEF, 0xBB, 0xBF, 0x62], false), Ok(utf16("\u{FEFF}b")));
        assert_eq!(decoder.decode(&[0xEF, 0xBB, 0xBF, 0x63], false), Ok(utf16("c")));

        let mut keeping = utf8_decoder(false, true);
        assert_eq!(keeping.decode(&[0xEF, 0xBB, 0xBF, 0x61], false), Ok(utf16("\u{FEFF}a")));
    }

    #[test]
    fn fatal_decoding_fails_on_invalid_bytes() {
        let mut decoder = utf8_decoder(true, false);
        assert_eq!(decoder.decode(&[0x61, 0xFF], false), Err(DecodingFailed));
        assert_eq!(decoder.decode(&[0x62], false), Ok(utf16("b")));
        assert_eq!(decoder.decode(&[0xE2, 0x82], false), Err(DecodingFailed));
    }

    #[test]
    fn a_failure_in_the_middle_of_the_input_discards_the_rest_of_its_bytes() {
        let mut decoder = utf8_decoder(true, false);
        assert_eq!(decoder.decode(&[0x61, 0xFF, 0x62], true), Err(DecodingFailed));
        assert_eq!(decoder.decode(&[0x63], true), Ok(utf16("c")));
        assert_eq!(decoder.decode(&[0x64], false), Ok(utf16("d")));

        let mut gb18030 = StreamingDecoder::new(encoding_rs::GB18030, true, false);
        assert_eq!(gb18030.decode(&[0x81, 0x30, 0xFF], true), Err(DecodingFailed));
        assert_eq!(gb18030.decode(&[], false), Ok(Vec::new()));
    }

    #[test]
    fn a_failure_keeps_the_byte_order_mark_unseen() {
        let mut decoder = utf8_decoder(true, false);
        assert_eq!(decoder.decode(&[0x61, 0xFF], true), Err(DecodingFailed));
        assert_eq!(decoder.decode(&[0xEF, 0xBB, 0xBF, 0x62], true), Ok(utf16("b")));
    }

    #[test]
    fn a_byte_order_mark_split_over_chunks_is_removed() {
        let mut decoder = utf8_decoder(false, false);
        assert_eq!(decoder.decode(&[0xEF, 0xBB], true), Ok(Vec::new()));
        assert_eq!(decoder.decode(&[0xBF, 0x61], true), Ok(utf16("a")));
        assert_eq!(decoder.decode(&[0xEF, 0xBB, 0xBF], false), Ok(utf16("\u{FEFF}")));
    }
}
