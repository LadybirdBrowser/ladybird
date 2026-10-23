/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Karol Kosek <krkk@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::{Base64Alphabet, LastChunkHandling, Span, Utf16StringUnits, decode_base64};
use liburl_rust::url::percent_decode_bytes;

use crate::mime_sniff::ffi::{OnMimeType, lend_mime_type};
use crate::mime_sniff::mime_type::MimeType;
use crate::rust_panic::abort_on_panic;

// https://fetch.spec.whatwg.org/#data-url-struct
pub struct DataUrl {
    pub mime_type: MimeType,
    pub body: Vec<u8>,
}

// https://infra.spec.whatwg.org/#forgiving-base64-decode
fn forgiving_base64_decode(string: &[u8]) -> Option<Vec<u8>> {
    // NB: A code point outside ASCII is never part of base64 data, nor ASCII whitespace.
    if !string.is_ascii() {
        return None;
    }
    let result = decode_base64(
        Utf16StringUnits::Ascii(string),
        Base64Alphabet::Base64,
        LastChunkHandling::Loose,
        None,
    );
    result.error.is_none().then_some(result.bytes)
}

// https://fetch.spec.whatwg.org/#data-url-processor
// NB: C++ runs steps 1 and 2, and passes the serialization of the URL as input.
pub fn process_data_url(input: &str) -> Option<DataUrl> {
    // 3. Remove the leading "data:" from input.
    let input = input.strip_prefix("data:").expect("the URL's scheme is data");

    // 4. Let position point at the start of input.
    // 5. Let mimeType be the result of collecting a sequence of code points that are not equal to U+002C (,), given
    //    position.
    let position = input.find(',');
    let mime_type = &input[..position.unwrap_or(input.len())];

    // 6. Strip leading and trailing ASCII whitespace from mimeType.
    let mut mime_type = mime_type.trim_ascii();

    // 7. If position is past the end of input, then return failure.
    let position = position?;

    // 8. Advance position by 1.
    // 9. Let encodedBody be the remainder of input.
    let encoded_body = &input[position + 1..];

    // 10. Let body be the percent-decoding of encodedBody.
    let mut body = percent_decode_bytes(encoded_body.as_bytes());

    // 11. If mimeType ends with U+003B (;), followed by zero or more U+0020 SPACE, followed by an ASCII
    //     case-insensitive match for "base64", then:
    let base64_suffix_start = mime_type.len().checked_sub("base64".len());
    if let Some(start) = base64_suffix_start
        && mime_type[start..].eq_ignore_ascii_case("base64")
        && mime_type[..start].trim_end_matches(' ').ends_with(';')
    {
        // 1. Let stringBody be the isomorphic decode of body.
        // 2. Set body to the forgiving-base64 decode of stringBody.
        // 3. If body is failure, then return failure.
        body = forgiving_base64_decode(&body)?;

        // 4. Remove the last 6 code points from mimeType.
        // 5. Remove trailing U+0020 SPACE code points from mimeType, if any.
        // 6. Remove the last U+003B (;) from mimeType.
        let without_base64 = mime_type[..start].trim_end_matches(' ');
        mime_type = &without_base64[..without_base64.len() - 1];
    }

    // 12. If mimeType starts with ";", then prepend "text/plain" to mimeType.
    let mime_type: Vec<char> = if mime_type.starts_with(';') {
        "text/plain".chars().chain(mime_type.chars()).collect()
    } else {
        mime_type.chars().collect()
    };

    // 13. Let mimeTypeRecord be the result of parsing mimeType.
    // 14. If mimeTypeRecord is failure, then set mimeTypeRecord to text/plain;charset=US-ASCII.
    let mime_type_record = MimeType::parse(&mime_type).unwrap_or_else(|| {
        let mut mime_type_record = MimeType::new("text".to_owned(), "plain".to_owned());
        mime_type_record.set_parameter("charset".to_owned(), "US-ASCII".to_owned());
        mime_type_record
    });

    // 15. Return a new data: URL struct whose MIME type is mimeTypeRecord and body is body.
    Some(DataUrl {
        mime_type: mime_type_record,
        body,
    })
}

type OnBytes = unsafe extern "C" fn(ctx: *mut c_void, bytes: Span<'_, u8>);

/// Processes the serialization of a data: URL, without its fragment. Calls `on_body` with the body of the data: URL
/// struct and `on_mime_type` with its MIME type, and returns whether it succeeded.
///
/// # Safety
///
/// `input` must be ASCII, and `on_body` and `on_mime_type` must be safe to call with `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn fetch_process_data_url(
    input: Span<'_, u8>,
    ctx: *mut c_void,
    on_body: OnBytes,
    on_mime_type: OnMimeType,
) -> bool {
    abort_on_panic(|| {
        let input = core::str::from_utf8(input.as_slice()).expect("C++ passes a URL serialization");
        let Some(data_url) = process_data_url(input) else {
            return false;
        };
        // SAFETY: The caller passes callbacks for `ctx`.
        unsafe {
            on_body(ctx, Span::new(&data_url.body));
            lend_mime_type(&data_url.mime_type, ctx, on_mime_type);
        }
        true
    })
}
