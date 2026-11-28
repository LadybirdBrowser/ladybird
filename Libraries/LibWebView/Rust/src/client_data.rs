/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The client data of a ceremony, which the Relying Party reads to know what the client did and for which origin. The
//! browser process makes it from what it knows by itself about the document that asked.

use crate::ffi_bytes::FfiBytes;
use crate::rust_panic::abort_on_panic;
use std::ffi::c_void;
use std::fmt::Write;

/// <https://w3c.github.io/webauthn/#base64url-encoding>
fn base64url_encode(bytes: &[u8]) -> String {
    // The term Base64url Encoding refers to the base64 encoding using the URL- and filename-safe character set defined
    // in Section 5 of [RFC4648], with all trailing '=' characters omitted (as permitted by Section 3.2) and without the
    // inclusion of any line breaks, whitespace, or other additional characters.
    const ALPHABET: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    let mut encoded = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for chunk in bytes.chunks(3) {
        let group = chunk.iter().enumerate().fold(0u32, |group, (index, byte)| {
            group | u32::from(*byte) << (16 - 8 * index)
        });
        // NB: One, two or three bytes give two, three or four characters.
        for index in 0..=chunk.len() {
            encoded.push(char::from(ALPHABET[(group >> (18 - 6 * index) & 0x3f) as usize]));
        }
    }
    encoded
}

/// <https://w3c.github.io/webauthn/#ccdtostring>
fn ccd_to_string(encoded: &mut String, string: &str) {
    // 1. Let encoded be an empty byte string.
    // 2. Append 0x22 (") to encoded.
    encoded.push('"');

    // 3. Invoke ToString on the given object to convert to a string.
    // 4. For each code point in the resulting string, if the code point:
    for code_point in string.chars() {
        match code_point {
            // -> is U+0022
            //    Append 0x5c22 (\") to encoded.
            '"' => encoded.push_str("\\\""),
            // -> is U+005C
            //    Append 0x5c5c (\\) to encoded.
            '\\' => encoded.push_str("\\\\"),
            // -> otherwise
            //    Append 0x5c75 (\u) to encoded, followed by four, lower-case hex digits that, when interpreted as a
            //    base-16 number, represent that code point.
            '\0'..'\u{20}' => {
                write!(encoded, "\\u{:04x}", u32::from(code_point)).expect("a string takes what is written");
            }
            // -> is in the set {U+0020, U+0021, U+0023–U+005B, U+005D–U+10FFFF}
            //    Append the UTF-8 encoding of that code point to encoded.
            _ => encoded.push(code_point),
        }
    }

    // 5. Append 0x22 (") to encoded.
    encoded.push('"');

    // 6. The result of this function is the value of encoded.
}

/// <https://w3c.github.io/webauthn/#clientdatajson-serialization>
pub fn serialize(type_: &str, challenge: &[u8], origin: &str, cross_origin: bool, top_origin: Option<&str>) -> Vec<u8> {
    // 1. Let result be an empty byte string.
    let mut result = String::new();

    // 2. Append 0x7b2274797065223a ({"type":) to result.
    result.push_str("{\"type\":");

    // 3. Append CCDToString(type) to result.
    ccd_to_string(&mut result, type_);

    // 4. Append 0x2c226368616c6c656e6765223a (,"challenge":) to result.
    result.push_str(",\"challenge\":");

    // 5. Append CCDToString(challenge) to result.
    // NB: The challenge of the client data is the base64url encoding of the challenge of the options.
    ccd_to_string(&mut result, &base64url_encode(challenge));

    // 6. Append 0x2c226f726967696e223a (,"origin":) to result.
    result.push_str(",\"origin\":");

    // 7. Append CCDToString(origin) to result.
    ccd_to_string(&mut result, origin);

    // 8. Append 0x2c2263726f73734f726967696e223a (,"crossOrigin":) to result.
    result.push_str(",\"crossOrigin\":");

    // 9. If crossOrigin is not present, or is false:
    //    1. Append 0x66616c7365 (false) to result.
    // 10. Otherwise:
    //    1. Append 0x74727565 (true) to result.
    result.push_str(if cross_origin { "true" } else { "false" });

    // 11. If topOrigin is present:
    if let Some(top_origin) = top_origin {
        // 1. Append 0x2c22746f704f726967696e223a (,"topOrigin":) to result.
        result.push_str(",\"topOrigin\":");

        // 2. Append CCDToString(topOrigin) to result.
        ccd_to_string(&mut result, top_origin);
    }

    // 12. Create a temporary copy of the CollectedClientData and remove the fields type, challenge, origin,
    //     crossOrigin (if present), and topOrigin (if present).
    // 13. If no fields remain in the temporary copy then:
    //     1. Append 0x7d (}) to result.
    result.push('}');

    // 15. The result of the serialization is the value of result.
    result.into_bytes()
}

/// <https://w3c.github.io/webauthn/#clientdatajson-serialization>
///
/// The text is UTF-8. `on_client_data` gets the serialization, whose bytes live for the call.
///
/// # Safety
/// Every argument points to as many bytes as its length says. `on_client_data` takes `context`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn webview_serialize_client_data(
    type_: FfiBytes,
    challenge: FfiBytes,
    origin: FfiBytes,
    cross_origin: bool,
    has_top_origin: bool,
    top_origin: FfiBytes,
    context: *mut c_void,
    on_client_data: unsafe extern "C" fn(context: *mut c_void, client_data: FfiBytes),
) {
    abort_on_panic(|| {
        // SAFETY: The caller says so.
        let (type_, challenge, origin, top_origin) = unsafe {
            (
                type_.to_string_lossy(),
                challenge.as_slice(),
                origin.to_string_lossy(),
                has_top_origin.then(|| top_origin.to_string_lossy()),
            )
        };
        let client_data = serialize(&type_, challenge, &origin, cross_origin, top_origin.as_deref());
        // SAFETY: The callback takes the context, and the bytes live for the call.
        unsafe { on_client_data(context, FfiBytes::from_slice(&client_data)) };
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn serialized(type_: &str, challenge: &[u8], origin: &str, top_origin: Option<&str>) -> String {
        String::from_utf8(serialize(type_, challenge, origin, top_origin.is_some(), top_origin)).unwrap()
    }

    #[test]
    fn base64url_has_no_padding_and_the_url_safe_alphabet() {
        // https://www.rfc-editor.org/rfc/rfc4648#section-10
        let vectors = [
            ("", ""),
            ("f", "Zg"),
            ("fo", "Zm8"),
            ("foo", "Zm9v"),
            ("foob", "Zm9vYg"),
            ("fooba", "Zm9vYmE"),
            ("foobar", "Zm9vYmFy"),
        ];
        for (input, encoded) in vectors {
            assert_eq!(base64url_encode(input.as_bytes()), encoded);
        }
        assert_eq!(base64url_encode(&[0xfb, 0xff, 0xfe]), "-__-");
    }

    #[test]
    fn serializes_the_fields_in_the_order_that_relying_parties_can_rely_on() {
        assert_eq!(
            serialized("webauthn.create", b"foob", "https://example.com", None),
            r#"{"type":"webauthn.create","challenge":"Zm9vYg","origin":"https://example.com","crossOrigin":false}"#
        );
    }

    #[test]
    fn a_cross_origin_document_says_so_with_its_top_origin() {
        assert_eq!(
            serialized(
                "webauthn.get",
                &[],
                "https://frame.example",
                Some("https://example.com")
            ),
            concat!(
                r#"{"type":"webauthn.get","challenge":"","origin":"https://frame.example","#,
                r#""crossOrigin":true,"topOrigin":"https://example.com"}"#
            )
        );
    }

    #[test]
    fn a_cross_origin_document_without_a_top_origin_to_tell_has_none() {
        let client_data = serialize("webauthn.get", &[], "https://frame.example", true, None);
        assert!(client_data.ends_with(br#""crossOrigin":true}"#));
    }

    #[test]
    fn text_cannot_break_out_of_its_string() {
        let mut encoded = String::new();
        ccd_to_string(&mut encoded, "a\"b\\c\u{1}\n\u{1f} \u{7f}\u{e9}\u{1f600}");
        assert_eq!(encoded, "\"a\\\"b\\\\c\\u0001\\u000a\\u001f \u{7f}\u{e9}\u{1f600}\"");

        // NB: An origin with a quote in it stays one string, and adds no field.
        let client_data = serialized("webauthn.get", &[], "https://a\",\"crossOrigin\":true,\"x\":\"", None);
        assert!(client_data.ends_with(r#""origin":"https://a\",\"crossOrigin\":true,\"x\":\"","crossOrigin":false}"#));
    }
}
