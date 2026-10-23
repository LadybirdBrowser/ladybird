/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use encoding_rs::Encoding;

use super::UrlInput;
use super::percent_encoding::{PercentEncodeSet, percent_decode_bytes, percent_encode_after_encoding_into};
use crate::textcodec::get_output_encoding;

// https://url.spec.whatwg.org/#concept-urlencoded-parser
// The application/x-www-form-urlencoded parser takes a byte sequence input, and then runs these steps:
pub fn parse(input: &[u8]) -> Vec<(String, String)> {
    // 1. Let sequences be the result of splitting input on 0x26 (&).
    let sequences = input.split(|byte| *byte == b'&');

    // 2. Let output be an initially empty list of name-value tuples where both name and value hold a string.
    let mut output = Vec::new();

    // 3. For each byte sequence bytes in sequences:
    for bytes in sequences {
        // 1. If bytes is the empty byte sequence, then continue.
        if bytes.is_empty() {
            continue;
        }

        let (name, value) = match bytes.iter().position(|byte| *byte == b'=') {
            // 2. If bytes contains a 0x3D (=), then let name be the bytes from the start of bytes up to but excluding its first 0x3D (=), and let value be the bytes, if any, after the first 0x3D (=) up to the end of bytes. If 0x3D (=) is the first byte, then name will be the empty byte sequence. If it is the last, then value will be the empty byte sequence.
            Some(index) => (&bytes[..index], &bytes[index + 1..]),
            // 3. Otherwise, let name have the value of bytes and let value be the empty byte sequence.
            None => (bytes, &[][..]),
        };

        // 4. Replace any 0x2B (+) in name and value with 0x20 (SP).
        // 5. Let nameString and valueString be the result of running UTF-8 decode without BOM on the percent-decoding of name and value, respectively.
        let decode = |bytes: &[u8]| {
            let space_decoded: Vec<u8> = bytes
                .iter()
                .map(|byte| if *byte == b'+' { b' ' } else { *byte })
                .collect();
            String::from_utf8_lossy(&percent_decode_bytes(&space_decoded)).into_owned()
        };

        // 6. Append (nameString, valueString) to output.
        output.push((decode(name), decode(value)));
    }

    // 4. Return output.
    output
}

// https://url.spec.whatwg.org/#concept-urlencoded-serializer
// The application/x-www-form-urlencoded serializer takes a list of name-value tuples tuples, with an optional encoding encoding (default UTF-8), and then runs these steps. They return an ASCII string.
pub fn serialize<'a>(
    tuples: impl IntoIterator<Item = (UrlInput<'a>, UrlInput<'a>)>,
    encoding: &'static Encoding,
) -> String {
    // 1. Set encoding to the result of getting an output encoding from encoding.
    let encoding = get_output_encoding(encoding);

    // 2. Let output be the empty string.
    let mut output = String::new();

    // 3. For each tuple of tuples:
    for (name, value) in tuples {
        // 1. Assert: tuple's name and tuple's value are scalar value strings.

        // 2. Let name be the result of running percent-encode after encoding with encoding, tuple's name, and the application/x-www-form-urlencoded percent-encode set.
        let mut encoded_name = String::new();
        percent_encode_after_encoding_into(
            encoding,
            name,
            PercentEncodeSet::ApplicationXWWWFormUrlencoded,
            true,
            &mut encoded_name,
        );

        // 3. Let value be the result of running percent-encode after encoding with encoding, tuple's value, and the application/x-www-form-urlencoded percent-encode set.
        let mut encoded_value = String::new();
        percent_encode_after_encoding_into(
            encoding,
            value,
            PercentEncodeSet::ApplicationXWWWFormUrlencoded,
            true,
            &mut encoded_value,
        );

        // 4. If output is not the empty string, then append U+0026 (&) to output.
        if !output.is_empty() {
            output.push('&');
        }

        // 5. Append name, followed by U+003D (=), followed by value, to output.
        output.push_str(&encoded_name);
        output.push('=');
        output.push_str(&encoded_value);
    }

    // 4. Return output.
    output
}
