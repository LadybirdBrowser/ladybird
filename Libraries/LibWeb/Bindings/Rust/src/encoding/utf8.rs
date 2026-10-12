/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The UTF-8 encoder, https://encoding.spec.whatwg.org/#utf-8-encoder, encoding a string in AK's representation one
//! scalar value at a time.

use ak::Utf16StringUnits;

/// The scalar values of a string with UTF-16 storage, each with the number of code units it takes. A lone surrogate
/// reads as U+FFFD, which takes as many code units.
fn scalar_values(code_units: &[u16]) -> impl Iterator<Item = (char, usize)> + '_ {
    char::decode_utf16(code_units.iter().copied()).map(|scalar_value| match scalar_value {
        Ok(scalar_value) => (scalar_value, scalar_value.len_utf16()),
        Err(_) => (char::REPLACEMENT_CHARACTER, 1),
    })
}

/// The number of code units read and of bytes written by the encodeInto() algorithm,
/// https://encoding.spec.whatwg.org/#dom-textencoder-encodeinto
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct EncodeIntoProgress {
    pub(crate) read: usize,
    pub(crate) written: usize,
}

/// Encodes the scalar values of the string that fit in `capacity` bytes, in order, and stops at the first one that
/// does not. `write` stores bytes at the offset it is given.
pub(crate) fn encode_into(
    units: &Utf16StringUnits<'_>,
    capacity: usize,
    mut write: impl FnMut(usize, &[u8]),
) -> EncodeIntoProgress {
    match units {
        Utf16StringUnits::Ascii(bytes) => {
            // Every scalar value of ASCII storage is one code unit and one byte.
            let length = bytes.len().min(capacity);
            write(0, &bytes[..length]);
            EncodeIntoProgress {
                read: length,
                written: length,
            }
        }
        Utf16StringUnits::Utf16(code_units) => {
            let mut progress = EncodeIntoProgress { read: 0, written: 0 };
            let mut bytes = [0; 4];
            for (scalar_value, code_unit_count) in scalar_values(code_units) {
                let encoded = scalar_value.encode_utf8(&mut bytes);
                if capacity - progress.written < encoded.len() {
                    break;
                }
                write(progress.written, encoded.as_bytes());
                progress.read += code_unit_count;
                progress.written += encoded.len();
            }
            progress
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(string: &str) -> Vec<u16> {
        string.encode_utf16().collect()
    }

    fn encoded_into(units: &Utf16StringUnits<'_>, capacity: usize) -> (EncodeIntoProgress, Vec<u8>) {
        let mut output = vec![0; capacity];
        let progress = encode_into(units, capacity, |offset, bytes| {
            output[offset..offset + bytes.len()].copy_from_slice(bytes);
        });
        (progress, output)
    }

    #[test]
    fn encode_into_encodes_lone_surrogates_as_replacement_characters() {
        let string = [0x61, 0xD800, 0x62, 0xDC00];
        let (progress, output) = encoded_into(&Utf16StringUnits::Utf16(&string), 8);
        assert_eq!(progress, EncodeIntoProgress { read: 4, written: 8 });
        assert_eq!(output, [0x61, 0xEF, 0xBF, 0xBD, 0x62, 0xEF, 0xBF, 0xBD]);
    }

    #[test]
    fn encode_into_stops_before_a_scalar_value_that_does_not_fit() {
        let string = utf16("ab\u{1F600}c");
        let (progress, output) = encoded_into(&Utf16StringUnits::Utf16(&string), 5);
        assert_eq!(progress, EncodeIntoProgress { read: 2, written: 2 });
        assert_eq!(output, [0x61, 0x62, 0, 0, 0]);

        let (progress, output) = encoded_into(&Utf16StringUnits::Utf16(&string), 6);
        assert_eq!(progress, EncodeIntoProgress { read: 4, written: 6 });
        assert_eq!(output, [0x61, 0x62, 0xF0, 0x9F, 0x98, 0x80]);
    }

    #[test]
    fn encode_into_counts_code_units_read() {
        let string = utf16("\u{E9}\u{1F600}");
        let (progress, _) = encoded_into(&Utf16StringUnits::Utf16(&string), 16);
        assert_eq!(progress, EncodeIntoProgress { read: 3, written: 6 });

        let (progress, output) = encoded_into(&Utf16StringUnits::Ascii(b"xyz"), 2);
        assert_eq!(progress, EncodeIntoProgress { read: 2, written: 2 });
        assert_eq!(output, b"xy");
    }
}
