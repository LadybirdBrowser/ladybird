/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Decoding with the decoders of the Encoding Standard, https://encoding.spec.whatwg.org/#encodings, which the
//! encoding_rs crate implements.

use ak::Utf16StringUnits;
use encoding_rs::{CoderResult, Decoder, DecoderResult, Encoding};

/// Getting an encoding from a label, https://encoding.spec.whatwg.org/#concept-encoding-get
pub(crate) fn encoding_for_label(label: Utf16StringUnits<'_>) -> Option<&'static Encoding> {
    match label {
        Utf16StringUnits::Ascii(label) => Encoding::for_label(label),
        Utf16StringUnits::Utf16(label) => {
            // NB: Every label is ASCII, so a label with any other code unit is not one.
            let label: Option<Vec<u8>> = label.iter().map(|&unit| u8::try_from(unit).ok()).collect();
            Encoding::for_label(&label.filter(|label| label.is_ascii())?)
        }
    }
}

#[derive(Debug, PartialEq, Eq)]
pub(crate) struct DecodingFailed;

/// The decoder of an encoding together with the error mode and the BOM handling that it decodes with, which keeps
/// the bytes of a character that a chunk of a stream ended in until the next chunk.
pub(crate) struct StreamingDecoder {
    encoding: &'static Encoding,
    fatal: bool,
    ignore_bom: bool,
    decoder: Decoder,
}

impl StreamingDecoder {
    pub(crate) fn new(encoding: &'static Encoding, fatal: bool, ignore_bom: bool) -> Self {
        Self {
            encoding,
            fatal,
            ignore_bom,
            decoder: Self::new_decoder(encoding, ignore_bom),
        }
    }

    fn new_decoder(encoding: &'static Encoding, ignore_bom: bool) -> Decoder {
        if ignore_bom {
            encoding.new_decoder_without_bom_handling()
        } else {
            encoding.new_decoder_with_bom_removal()
        }
    }

    pub(crate) fn encoding(&self) -> &'static Encoding {
        self.encoding
    }

    pub(crate) fn fatal(&self) -> bool {
        self.fatal
    }

    pub(crate) fn ignore_bom(&self) -> bool {
        self.ignore_bom
    }

    /// Decodes the next bytes of the input to UTF-16. Unless more of the input follows, the bytes are its end: the
    /// decoder then flushes what it kept, and starts over for the next input, whether or not decoding succeeded.
    pub(crate) fn decode(&mut self, input: &[u8], more_input_follows: bool) -> Result<Vec<u16>, DecodingFailed> {
        let decoded = self.decode_with_current_decoder(input, !more_input_follows);
        if !more_input_follows {
            // NB: encoding_rs finishes a decoder for good once it has seen the end of its input.
            self.decoder = Self::new_decoder(self.encoding, self.ignore_bom);
        }
        decoded
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
    fn labels() {
        let name = |label: Utf16StringUnits<'_>| encoding_for_label(label).map(Encoding::name);
        assert_eq!(name(Utf16StringUnits::Ascii(b" Latin1\n")), Some("windows-1252"));
        assert_eq!(name(Utf16StringUnits::Ascii(b"nonsense")), None);
        assert_eq!(name(Utf16StringUnits::Utf16(&utf16("utf-8"))), Some("UTF-8"));
        assert_eq!(name(Utf16StringUnits::Utf16(&utf16("\u{FC}tf-8"))), None);
        assert_eq!(name(Utf16StringUnits::Utf16(&utf16("\u{175}tf-8"))), None);
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
    fn a_failure_in_the_middle_of_the_input_keeps_the_decoder() {
        let mut decoder = utf8_decoder(true, false);
        assert_eq!(decoder.decode(&[0x61, 0xFF, 0x62], true), Err(DecodingFailed));
        assert_eq!(decoder.decode(&[0x63], true), Ok(utf16("c")));
        assert_eq!(decoder.decode(&[0x64], false), Ok(utf16("d")));
    }
}
