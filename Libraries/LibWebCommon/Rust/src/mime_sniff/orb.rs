/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The byte sniffing of the opaque-response-safelist check (opaque-response blocking), from the draft of
//! https://github.com/whatwg/fetch/pull/1442

use super::resource::{
    SCRIPTABLE_PATTERN_TABLE, WHITESPACE_BYTES, match_an_audio_or_video_type_pattern, match_an_image_type_pattern,
    pattern_matching_algorithm,
};

// https://mimesniff.spec.whatwg.org/#image-type-pattern-matching-algorithm
pub fn matches_an_image_type_pattern(bytes: &[u8]) -> bool {
    match_an_image_type_pattern(bytes).is_some()
}

// https://mimesniff.spec.whatwg.org/#audio-or-video-type-pattern-matching-algorithm
pub fn matches_an_audio_or_video_type_pattern(bytes: &[u8]) -> bool {
    match_an_audio_or_video_type_pattern(bytes).is_some()
}

/// Whether `bytes`, the start of a response's body, show it to be HTML, XML, or JSON, or to start with a prefix that
/// keeps it from running as a script. None of these is a script that runs, so blocking such a response can't break
/// one: The check stands in for the full parse of the response as JavaScript that the opaque-response-safelist check
/// ends with, and answers false wherever the bytes don't settle it.
pub fn shows_a_resource_that_is_not_a_script(bytes: &[u8]) -> bool {
    // NB: Decoding a script drops a leading UTF-8 BOM, so what follows one is what would run.
    let bytes = bytes.strip_prefix(b"\xEF\xBB\xBF").unwrap_or(bytes);
    starts_with_a_script_breaking_prefix(bytes) || shows_html_or_xml(bytes) || shows_a_json_object(bytes)
}

fn skip_whitespace(bytes: &[u8]) -> &[u8] {
    let start = bytes
        .iter()
        .position(|byte| !WHITESPACE_BYTES.contains(byte))
        .unwrap_or(bytes.len());
    &bytes[start..]
}

// Prefixes that servers put before JSON so that it fails or never ends when included as a script.
fn starts_with_a_script_breaking_prefix(bytes: &[u8]) -> bool {
    const PREFIXES: [&[u8]; 7] = [
        b")]}'",
        b"{}&&",
        b"{} &&",
        b"for(;;);",
        b"for (;;);",
        b"while(1);",
        b"while (1);",
    ];
    let bytes = skip_whitespace(bytes);
    PREFIXES.iter().any(|prefix| bytes.starts_with(prefix))
}

// The HTML and XML rows of https://mimesniff.spec.whatwg.org/#rules-for-identifying-an-unknown-mime-type, after any
// complete comments. A comment alone doesn't show HTML: A script may start with one, as "<!--" opens a comment in
// classic scripts too, one that runs to the end of its line whatever "-->" it holds. So after a comment, a tag shows
// HTML only from a later line.
fn shows_html_or_xml(bytes: &[u8]) -> bool {
    let mut rest = skip_whitespace(bytes);
    while let Some(comment) = rest.strip_prefix(b"<!--") {
        let Some(end) = comment.windows(3).position(|window| window == b"-->") else {
            return false;
        };
        let after_comment = &comment[end + 3..];
        rest = skip_whitespace(after_comment);
        let skipped = &after_comment[..after_comment.len() - rest.len()];
        if !skipped.iter().any(|byte| *byte == b'\n' || *byte == b'\r') {
            return false;
        }
    }
    SCRIPTABLE_PATTERN_TABLE.iter().any(|row| {
        (row.mime_type == "text/html" || row.mime_type == "text/xml")
            && !row.byte_pattern.starts_with(b"<!--")
            && pattern_matching_algorithm(rest, row)
    })
}

// An object whose first member has a string name, as "{" followed by a JSON string and ":". Unlike an array, it is a
// syntax error as a script.
fn shows_a_json_object(bytes: &[u8]) -> bool {
    let Some(rest) = skip_whitespace(bytes).strip_prefix(b"{") else {
        return false;
    };
    let Some(mut string) = skip_whitespace(rest).strip_prefix(b"\"") else {
        return false;
    };
    loop {
        match string {
            [b'"', rest @ ..] => return skip_whitespace(rest).starts_with(b":"),
            [b'\\', _, rest @ ..] => string = rest,
            [byte, rest @ ..] if *byte >= 0x20 => string = rest,
            _ => return false,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn html_and_xml_are_not_scripts() {
        assert!(shows_a_resource_that_is_not_a_script(
            b"<!doctype html><title>x</title>"
        ));
        assert!(shows_a_resource_that_is_not_a_script(b"\n  <HTML>"));
        assert!(shows_a_resource_that_is_not_a_script(b"\xEF\xBB\xBF<!doctype html>"));
        assert!(shows_a_resource_that_is_not_a_script(b"<!-- note -->\n<p>text"));
        assert!(shows_a_resource_that_is_not_a_script(b"<?xml version=\"1.0\"?><a/>"));
    }

    #[test]
    fn json_objects_and_breaking_prefixes_are_not_scripts() {
        assert!(shows_a_resource_that_is_not_a_script(b"{\"key\": 1}"));
        assert!(shows_a_resource_that_is_not_a_script(b" { \"a\\\"b\" : [] }"));
        assert!(shows_a_resource_that_is_not_a_script(b")]}'\n{\"a\":1}"));
        assert!(shows_a_resource_that_is_not_a_script(b"for(;;);[1]"));
    }

    #[test]
    fn scripts_and_unsettled_bytes_are_allowed() {
        assert!(!shows_a_resource_that_is_not_a_script(b"var x = 1;"));
        assert!(!shows_a_resource_that_is_not_a_script(b"\xEF\xBB\xBFvar x = 1;"));
        assert!(!shows_a_resource_that_is_not_a_script(b"[1, 2, 3]"));
        assert!(!shows_a_resource_that_is_not_a_script(
            b"<!-- legacy comment\nalert(1);"
        ));
        assert!(!shows_a_resource_that_is_not_a_script(b"<!-- unterminated"));
        assert!(!shows_a_resource_that_is_not_a_script(b"<!-- note --> <p>\nalert(1);"));
        assert!(!shows_a_resource_that_is_not_a_script(b"{ a: 1 }"));
        assert!(!shows_a_resource_that_is_not_a_script(b"{\"unterminated"));
        assert!(!shows_a_resource_that_is_not_a_script(b""));
    }
}
