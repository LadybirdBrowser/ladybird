/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use encoding_rs::Encoding;
use encoding_rs::UTF_8;

use crate::textcodec::EncodeItem;
use crate::textcodec::encode_into as textcodec_encode_into;

#[allow(dead_code)]
#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(super) enum PercentEncodeSet {
    C0Control,
    Fragment,
    Query,
    SpecialQuery,
    Path,
    Userinfo,
    Component,
    ApplicationXWWWFormUrlencoded,
}

fn code_point_is_in_percent_encode_set(code_point: char, set: PercentEncodeSet) -> bool {
    let code_point_u32 = code_point as u32;
    match set {
        // https://url.spec.whatwg.org/#c0-control-percent-encode-set
        // The C0 control percent-encode set are the C0 controls and all code points greater than U+007E (~).
        PercentEncodeSet::C0Control => !(0x20..=0x7e).contains(&code_point_u32),
        // https://url.spec.whatwg.org/#fragment-percent-encode-set
        // The query percent-encode set is the C0 control percent-encode set and U+0020 SPACE, U+0022 ("), U+0023 (#), U+003C (<), and U+003E (>).
        PercentEncodeSet::Fragment => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::C0Control)
                || [' ', '"', '<', '>', '`'].contains(&code_point)
        }
        // https://url.spec.whatwg.org/#query-percent-encode-set
        // The query percent-encode set is the C0 control percent-encode set and U+0020 SPACE, U+0022 ("), U+0023 (#), U+003C (<), and U+003E (>).
        // NOTE: The query percent-encode set cannot be defined in terms of the fragment percent-encode set due to the omission of U+0060 (`).
        PercentEncodeSet::Query => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::C0Control)
                || [' ', '"', '#', '<', '>'].contains(&code_point)
        }
        // https://url.spec.whatwg.org/#special-query-percent-encode-set
        // The special-query percent-encode set is the query percent-encode set and U+0027 (').
        PercentEncodeSet::SpecialQuery => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Query) || code_point == '\''
        }
        // https://url.spec.whatwg.org/#path-percent-encode-set
        // The path percent-encode set is the query percent-encode set and U+003F (?), U+005E (^), U+0060 (`), U+007B ({), and U+007D (}).
        PercentEncodeSet::Path => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Query)
                || ['?', '^', '`', '{', '}'].contains(&code_point)
        }
        // https://url.spec.whatwg.org/#userinfo-percent-encode-set
        // The userinfo percent-encode set is the path percent-encode set and U+002F (/), U+003A (:), U+003B (;),
        // U+003D (=), U+0040 (@), U+005B ([) to U+005D (]), inclusive, and U+007C (|).
        PercentEncodeSet::Userinfo => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Path)
                || ['/', ':', ';', '=', '@', '[', '\\', ']', '|'].contains(&code_point)
        }
        // https://url.spec.whatwg.org/#component-percent-encode-set
        // The component percent-encode set is the userinfo percent-encode set and U+0024 ($) to U+0026 (&), inclusive, U+002B (+), and U+002C (,).
        // NOTE: This is used by HTML for registerProtocolHandler(), and could also be used by other standards to
        //       percent-encode data that can then be embedded in a URL’s path, query, or fragment; or in an opaque host.
        //       Using it with UTF-8 percent-encode gives identical results to JavaScript’s encodeURIComponent() [sic]. [HTML] [ECMA-262]
        PercentEncodeSet::Component => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Userinfo)
                || ['$', '%', '&', '+', ','].contains(&code_point)
        }
        // https://url.spec.whatwg.org/#application-x-www-form-urlencoded-percent-encode-set
        // The application/x-www-form-urlencoded percent-encode set is the component percent-encode set and U+0021 (!),
        // U+0027 (') to U+0029 RIGHT PARENTHESIS, inclusive, and U+007E (~).
        PercentEncodeSet::ApplicationXWWWFormUrlencoded => {
            code_point_is_in_percent_encode_set(code_point, PercentEncodeSet::Component)
                || ['!', '\'', '(', ')', '~'].contains(&code_point)
        }
    }
}

fn append_percent_encoded(builder: &mut String, code_point: char) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";

    let mut utf8 = [0u8; 4];
    for byte in code_point.encode_utf8(&mut utf8).as_bytes() {
        builder.push('%');
        builder.push(HEX[(byte >> 4) as usize] as char);
        builder.push(HEX[(byte & 0x0f) as usize] as char);
    }
}

fn append_percent_encoded_byte(builder: &mut String, byte: u8) {
    const HEX: &[u8; 16] = b"0123456789ABCDEF";

    builder.push('%');
    builder.push(HEX[(byte >> 4) as usize] as char);
    builder.push(HEX[(byte & 0x0f) as usize] as char);
}

pub(super) fn percent_encode_input_into(
    input: super::UrlInput<'_>,
    set: PercentEncodeSet,
    space_as_plus: bool,
    output: &mut String,
) {
    for code_point in input.chars() {
        if space_as_plus && code_point == ' ' {
            output.push('+');
        } else if code_point_is_in_percent_encode_set(code_point, set) {
            append_percent_encoded(output, code_point);
        } else {
            output.push(code_point);
        }
    }
}

pub(super) fn percent_decode_input(input: super::UrlInput<'_>) -> Vec<u8> {
    let mut output = Vec::with_capacity(input.len());
    let mut characters = input.chars();
    while let Some(character) = characters.next() {
        if character == '%' {
            let mut lookahead = characters.clone();
            if let (Some(high), Some(low)) = (lookahead.next(), lookahead.next())
                && high.is_ascii_hexdigit()
                && low.is_ascii_hexdigit()
            {
                output.push(((high.to_digit(16).unwrap() << 4) | low.to_digit(16).unwrap()) as u8);
                characters = lookahead;
                continue;
            }
        }
        output.extend_from_slice(character.encode_utf8(&mut [0; 4]).as_bytes());
    }
    output
}

// https://url.spec.whatwg.org/#string-percent-encode-after-encoding
pub(super) fn percent_encode_after_encoding_into(
    encoding: &'static Encoding,
    input: super::UrlInput<'_>,
    set: PercentEncodeSet,
    space_as_plus: bool,
    output: &mut String,
) {
    // 1. Let encoder be the result of getting an encoder from encoding.
    // 2. Let inputQueue be input converted to an I/O queue.
    // 3. Let output be the empty string.

    let is_appended_unchanged =
        |byte: u8| !(space_as_plus && byte == b' ') && !code_point_is_in_percent_encode_set(char::from(byte), set);
    let append_byte = |output: &mut String, byte: u8| {
        // 1. If spaceAsPlus is true and byte is 0x20 (SP), then append U+002B (+) to output and continue.
        if space_as_plus && byte == b' ' {
            output.push('+');
            return;
        }

        // 2. Let isomorph be a code point whose value is byte’s value.
        let code_point = char::from(byte);

        // 4. If isomorphic is not in percentEncodeSet, then append isomorph to output.
        if !code_point_is_in_percent_encode_set(code_point, set) {
            output.push(code_point);
        } else {
            append_percent_encoded_byte(output, byte);
        }
    };

    // OPTIMIZATION: The UTF-8 encoder turns each code point into its UTF-8 bytes and never reports an error, so skip
    //               encoding_rs and copy the run of ASCII bytes that are appended unchanged in bulk.
    if encoding == UTF_8 {
        let mut remaining = input;
        while let Some(code_point) = remaining.first_code_point() {
            let unchanged_length = remaining.ascii_prefix_length(is_appended_unchanged);
            if unchanged_length > 0 {
                remaining.slice(0..unchanged_length).append_ascii_to(output);
                remaining = remaining.suffix(unchanged_length);
                continue;
            }
            for &byte in code_point.encode_utf8(&mut [0; 4]).as_bytes() {
                append_byte(output, byte);
            }
            remaining = remaining.suffix(remaining.char_length(code_point));
        }

        // 6. Return output.
        return;
    }

    let on_item = |item| match item {
        EncodeItem::Byte(byte) => append_byte(output, byte),
        EncodeItem::Error(error) => {
            output.push_str("%26%23");
            output.push_str(&error.to_string());
            output.push_str("%3B");
        }
    };
    let did_succeed = match input {
        super::UrlInput::Utf8(input) => textcodec_encode_into(encoding, input, on_item),
        super::UrlInput::Utf16(input) => crate::textcodec::encode_utf16_into(encoding, input, on_item),
    };
    assert!(did_succeed, "encoding_rs should encode into any output encoding");

    // 6. Return output.
}

pub(super) fn append_percent_encoded_if_necessary(buffer: &mut String, code_point: char, set: PercentEncodeSet) {
    if code_point_is_in_percent_encode_set(code_point, set) {
        append_percent_encoded(buffer, code_point);
    } else {
        buffer.push(code_point);
    }
}
