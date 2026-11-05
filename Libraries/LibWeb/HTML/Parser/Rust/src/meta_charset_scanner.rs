/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Finds a document's encoding declaration before the HTML parser starts, by tokenizing the document's first bytes as
//! the parser will: the prescan proposed in https://github.com/whatwg/html/issues/6962.

use crate::known_names::attribute_name;
use crate::known_names::tag_name;
use crate::preload_scanner::tokenizer_state_after_start_tag;
use crate::token::Attribute;
use crate::token::HtmlName;
use crate::token::KnownName;
use crate::token::TokenPayload;
use crate::token::TokenType;
use crate::tokenizer::HtmlTokenizer;
use std::ffi::c_void;

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiMetaCharsetScanResult {
    /// The bytes so far hold no declaration, but the bytes still to come may.
    NeedMoreInput = 0,
    /// A declaration names an encoding.
    Found = 1,
    /// No declaration can still come.
    NotFound = 2,
}

/// How many bytes the scan covers even once the head has ended: It looks at every token that ends in them.
const UNCONDITIONAL_SCAN_LENGTH: usize = 1024;

/// Tokenizes a document's bytes as they arrive, as the HTML parser will, and finds the first meta element that the tree
/// builder would take as an encoding declaration: one with a charset attribute, or an http-equiv of Content-Type and a
/// content attribute. It looks at every token in the first 1024 bytes, and past them for as long as the head can still
/// hold a declaration: until the tree builder would leave the head, or start a template.
pub struct MetaCharsetScanner {
    tokenizer: HtmlTokenizer,
    scripting_enabled: bool,
    // Whether the tokenizer is in an element whose contents are text, which ends with its end tag.
    in_text_element: bool,
    // Whether the tree builder is in a noscript element in the head, with scripting disabled: in its "in head noscript"
    // insertion mode.
    in_head_noscript: bool,
    // Whether the tree builder is in a frameset, which ignores a meta element.
    in_frameset: bool,
    // Whether the tree builder has left the head, or started a template: what ends the scan past the first 1024 bytes.
    head_ended: bool,
    // The document offset of the tokenizer's input, which the scanner drops once the tokenizer has consumed it.
    input_offset: usize,
    finished: bool,
}

impl MetaCharsetScanner {
    pub fn new(scripting_enabled: bool) -> Self {
        // The bytes arrive in pieces, so the end of the ones so far isn't the end of the input until the caller says so.
        let mut tokenizer = HtmlTokenizer::new(Vec::new());
        tokenizer.set_input_stream_closed(false);
        Self {
            tokenizer,
            scripting_enabled,
            in_text_element: false,
            in_head_noscript: false,
            in_frameset: false,
            head_ended: false,
            input_offset: 0,
            finished: false,
        }
    }

    /// Scans the bytes that arrived since the last call. `is_encoding` tells whether an attribute's value names an
    /// encoding: a charset attribute's value, or a content attribute's (marked as such).
    pub fn scan(
        &mut self,
        bytes: &[u8],
        input_complete: bool,
        mut is_encoding: impl FnMut(&str, bool) -> bool,
    ) -> RustFfiMetaCharsetScanResult {
        if self.finished {
            return RustFfiMetaCharsetScanResult::NotFound;
        }
        // Bytes before a declaration only ever matter as ASCII, which every encoding a declaration can name reads the
        // same way, so reading them as Latin-1 tokenizes them as the parser will.
        self.tokenizer.append_latin1_input(bytes);
        if input_complete {
            self.tokenizer.set_input_stream_closed(true);
        }

        while !self.past_the_scan() {
            let Some(token) = self.tokenizer.next_token(false, false) else {
                break;
            };
            if self.past_the_scan() {
                break;
            }
            let ends_head = match token.token_type {
                TokenType::StartTag => {
                    let name = token.tag_name();
                    if *name == tag_name!("meta")
                        && !self.in_frameset
                        && let TokenPayload::Tag { attributes, .. } = &token.payload
                        && declares_encoding(attributes, &mut is_encoding)
                    {
                        self.finished = true;
                        return RustFfiMetaCharsetScanResult::Found;
                    }
                    if *name == tag_name!("frameset") {
                        self.in_frameset = true;
                    }
                    // The tree builder switches the tokenizer out of the data state for these elements, so their
                    // contents are text rather than markup.
                    if let Some(state) = tokenizer_state_after_start_tag(name, self.scripting_enabled) {
                        self.tokenizer.switch_to(state);
                        self.in_text_element = true;
                    }
                    if self.in_head_noscript && *name != tag_name!("head") && *name != tag_name!("noscript") {
                        // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inheadnoscript
                        // The tree builder handles these in the noscript, ignores head and noscript there, and for
                        // anything else leaves the noscript and handles the tag in the head.
                        if !start_tag_keeps_head_noscript_open(name) {
                            self.in_head_noscript = false;
                        }
                    } else if *name == tag_name!("noscript") && !self.scripting_enabled && !self.head_ended {
                        self.in_head_noscript = true;
                    }
                    !self.in_head_noscript && !start_tag_keeps_head_open(name)
                }
                TokenType::EndTag => {
                    self.in_text_element = false;
                    let name = token.tag_name();
                    if self.in_head_noscript {
                        // https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inheadnoscript
                        // The tree builder goes back to the head after this noscript, leaves the head at a br, and
                        // ignores any other end tag.
                        if *name == tag_name!("noscript") {
                            self.in_head_noscript = false;
                        }
                        *name == tag_name!("br")
                    } else {
                        end_tag_ends_head(name)
                    }
                }
                // Text that isn't whitespace has the tree builder leave the head, unless it's an element's contents.
                TokenType::Character => !self.in_text_element && !is_html_whitespace(token.code_point),
                TokenType::EndOfFile => break,
                _ => false,
            };
            if ends_head {
                self.head_ended = true;
            }
        }

        if input_complete || self.past_the_scan() {
            self.finished = true;
            return RustFfiMetaCharsetScanResult::NotFound;
        }

        // Drop the input the tokenizer has consumed, which it does only once it has consumed all of it.
        let consumed = self.tokenizer.current_offset;
        self.tokenizer.parser_did_run();
        if self.tokenizer.current_offset == 0 {
            self.input_offset += consumed;
        }
        RustFfiMetaCharsetScanResult::NeedMoreInput
    }

    // Whether the tokenizer has gone past the first 1024 bytes once the head has ended.
    fn past_the_scan(&self) -> bool {
        self.head_ended && self.input_offset + self.tokenizer.current_offset > UNCONDITIONAL_SCAN_LENGTH
    }
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead (a start tag whose tag name is "meta")
fn declares_encoding(attributes: &[Attribute], is_encoding: &mut impl FnMut(&str, bool) -> bool) -> bool {
    // If the element has a charset attribute, and getting an encoding from its value results in an encoding, ...
    if let Some(charset) = attribute_value(attributes, attribute_name!("charset"))
        && is_encoding(charset, false)
    {
        return true;
    }
    // Otherwise, if the element has an http-equiv attribute whose value is an ASCII case-insensitive match for the
    // string "Content-Type", and the element has a content attribute, and applying the algorithm for extracting a
    // character encoding from a meta element to that attribute's value returns an encoding, ...
    let Some(http_equiv) = attribute_value(attributes, attribute_name!("http-equiv")) else {
        return false;
    };
    if !http_equiv.eq_ignore_ascii_case("content-type") {
        return false;
    }
    attribute_value(attributes, attribute_name!("content")).is_some_and(|content| is_encoding(content, true))
}

fn attribute_value(attributes: &[Attribute], name: KnownName) -> Option<&str> {
    attributes
        .iter()
        .find(|attribute| attribute.local_name == name)
        .map(|attribute| attribute.value.as_str())
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inhead
/// Whether the head can still be open after a start tag with this name: whether the tree builder handles it in the head.
/// It handles template there too, but a template ends the scan.
fn start_tag_keeps_head_open(name: &HtmlName) -> bool {
    *name == tag_name!("html")
        || *name == tag_name!("head")
        || *name == tag_name!("base")
        || *name == tag_name!("basefont")
        || *name == tag_name!("bgsound")
        || *name == tag_name!("link")
        || *name == tag_name!("meta")
        || *name == tag_name!("noframes")
        || *name == tag_name!("noscript")
        || *name == tag_name!("script")
        || *name == tag_name!("style")
        || *name == tag_name!("title")
}

// https://html.spec.whatwg.org/multipage/parsing.html#parsing-main-inheadnoscript
/// Whether the head can still be open after a start tag with this name, in a noscript element in the head with
/// scripting disabled.
fn start_tag_keeps_head_noscript_open(name: &HtmlName) -> bool {
    *name == tag_name!("html")
        || *name == tag_name!("basefont")
        || *name == tag_name!("bgsound")
        || *name == tag_name!("link")
        || *name == tag_name!("meta")
        || *name == tag_name!("noframes")
        || *name == tag_name!("style")
}

/// Whether an end tag with this name has the tree builder leave the head. It ignores any other end tag there.
fn end_tag_ends_head(name: &HtmlName) -> bool {
    *name == tag_name!("head") || *name == tag_name!("body") || *name == tag_name!("html") || *name == tag_name!("br")
}

fn is_html_whitespace(code_point: u32) -> bool {
    matches!(code_point, 0x09 | 0x0A | 0x0C | 0x0D | 0x20)
}

/// Creates a scanner, for rust_html_meta_charset_scanner_scan().
#[unsafe(no_mangle)]
pub extern "C" fn rust_html_meta_charset_scanner_create(scripting_enabled: bool) -> *mut MetaCharsetScanner {
    Box::into_raw(Box::new(MetaCharsetScanner::new(scripting_enabled)))
}

/// Destroys a scanner.
///
/// # Safety
/// `scanner` must be a valid pointer from `rust_html_meta_charset_scanner_create`, and must not be used after this
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_meta_charset_scanner_destroy(scanner: *mut MetaCharsetScanner) {
    if !scanner.is_null() {
        drop(unsafe { Box::from_raw(scanner) });
    }
}

/// Scans the document bytes that arrived since the last call.
///
/// # Safety
/// `scanner` must be a valid pointer from `rust_html_meta_charset_scanner_create`. `is_encoding` must not retain the
/// value pointer beyond the callback invocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_meta_charset_scanner_scan(
    scanner: *mut MetaCharsetScanner,
    input: ak::Span<'_, u8>,
    input_complete: bool,
    ctx: *mut c_void,
    is_encoding: unsafe extern "C" fn(ctx: *mut c_void, value: *const u8, value_len: usize, is_content: bool) -> bool,
) -> RustFfiMetaCharsetScanResult {
    let scanner = unsafe { &mut *scanner };
    scanner.scan(input.as_slice(), input_complete, |value, is_content| unsafe {
        is_encoding(ctx, value.as_ptr(), value.len(), is_content)
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    fn is_utf8(value: &str, _: bool) -> bool {
        value.to_ascii_lowercase().contains("utf-8")
    }

    fn scan_all(input: &[u8]) -> RustFfiMetaCharsetScanResult {
        MetaCharsetScanner::new(true).scan(input, true, is_utf8)
    }

    fn padding() -> Vec<u8> {
        let mut padding = b"<!--".to_vec();
        padding.extend(std::iter::repeat_n(b'a', 1500));
        padding.extend(b"-->");
        padding
    }

    #[test]
    fn finds_a_declaration_after_the_first_kilobyte_in_the_head() {
        let mut input = b"<!DOCTYPE html><head>".to_vec();
        input.extend(padding());
        input.extend(b"<title>caf\xe9</title><meta charset=\"utf-8\"></head>");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::Found);
    }

    #[test]
    fn finds_an_http_equiv_declaration() {
        let mut input = padding();
        input.extend(b"<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::Found);
    }

    #[test]
    fn ignores_a_declaration_after_the_head_ended() {
        let mut input = padding();
        input.extend(b"<p>text</p><meta charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);
    }

    #[test]
    fn ignores_markup_in_script_style_and_title_text() {
        for (open, close) in [
            ("<script>", "</script>"),
            ("<style>", "</style>"),
            ("<title>", "</title>"),
        ] {
            let mut input = padding();
            input.extend(format!("{open}'<meta charset=\"utf-8\">'{close}</head><body>").bytes());
            assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound, "{open}");
        }
    }

    #[test]
    fn text_ends_the_head_but_text_in_the_head_does_not() {
        let mut input = padding();
        input.extend(b"text<meta charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);

        let mut input = padding();
        input.extend(b" \n<title>text</title><script>text</script><meta charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::Found);
    }

    #[test]
    fn only_tags_the_head_can_hold_keep_it_open() {
        let head_tags = "<basefont><bgsound><noframes>'<p>'</noframes><link><base>";
        let mut input = padding();
        input.extend(format!("{head_tags}<meta charset=\"utf-8\">").bytes());
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::Found);

        let mut input = padding();
        input.extend(b"<object></object><meta charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);
    }

    #[test]
    fn noscript_with_scripting_disabled_has_rules_of_its_own() {
        let scan_without_scripting = |input: &[u8]| MetaCharsetScanner::new(false).scan(input, true, is_utf8);

        let mut input = padding();
        input.extend(b"<noscript><link></head></body></html></noscript><meta charset=\"utf-8\">");
        assert_eq!(scan_without_scripting(&input), RustFfiMetaCharsetScanResult::Found);

        let mut input = padding();
        input.extend(b"<noscript><meta charset=\"utf-8\"></noscript>");
        assert_eq!(scan_without_scripting(&input), RustFfiMetaCharsetScanResult::Found);

        // A start tag the noscript can't hold ends it, but not the head, if the head can hold it.
        let mut input = padding();
        input.extend(b"<noscript><title>title</title><meta charset=\"utf-8\">");
        assert_eq!(scan_without_scripting(&input), RustFfiMetaCharsetScanResult::Found);

        for leaves_head in ["<noscript></br>", "<noscript><p>", "<noscript>text"] {
            let mut input = padding();
            input.extend(format!("{leaves_head}<meta charset=\"utf-8\">").bytes());
            assert_eq!(
                scan_without_scripting(&input),
                RustFfiMetaCharsetScanResult::NotFound,
                "{leaves_head}"
            );
        }
    }

    #[test]
    fn only_some_end_tags_end_the_head() {
        let mut input = padding();
        input.extend(b"</bogus></p><meta charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::Found);

        for end_tag in ["</head>", "</body>", "</html>", "</br>"] {
            let mut input = padding();
            input.extend(format!("{end_tag}<meta charset=\"utf-8\">").bytes());
            assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound, "{end_tag}");
        }
    }

    #[test]
    fn counts_a_declaration_anywhere_in_the_first_kilobyte() {
        assert_eq!(
            scan_all(b"<p>text</p><meta charset=\"utf-8\">"),
            RustFfiMetaCharsetScanResult::Found
        );
        assert_eq!(
            scan_all(b"<template><meta charset=\"utf-8\"></template>"),
            RustFfiMetaCharsetScanResult::Found
        );
    }

    #[test]
    fn ignores_a_tag_that_ends_past_the_first_kilobyte_after_the_head() {
        let mut input = b"<body><meta data-padding=\"".to_vec();
        input.extend(std::iter::repeat_n(b'a', 2000));
        input.extend(b"\" charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);
    }

    #[test]
    fn stops_inside_a_comment_once_the_head_ended_past_the_first_kilobyte() {
        let mut input = b"<body><!--".to_vec();
        input.extend(std::iter::repeat_n(b'a', 1500));
        assert_eq!(
            MetaCharsetScanner::new(true).scan(&input, false, is_utf8),
            RustFfiMetaCharsetScanResult::NotFound
        );
    }

    #[test]
    fn a_template_ends_the_scan_past_the_first_kilobyte() {
        let mut input = padding();
        input.extend(b"<template></template><meta charset=\"utf-8\">");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);
    }

    #[test]
    fn stops_without_more_input_once_the_head_ended_past_the_first_kilobyte() {
        let mut input = padding();
        input.extend(b"<p>");
        assert_eq!(
            MetaCharsetScanner::new(true).scan(&input, false, is_utf8),
            RustFfiMetaCharsetScanResult::NotFound
        );
    }

    #[test]
    fn keeps_the_document_offset_across_scans() {
        // Each scan drops the input the tokenizer has consumed, but the 1024-byte boundary stays where it is.
        let mut comment = b"<!--".to_vec();
        comment.extend(std::iter::repeat_n(b'a', 400));
        comment.extend(b"-->");
        let mut scanner = MetaCharsetScanner::new(true);
        for _ in 0..3 {
            assert_eq!(
                scanner.scan(&comment, false, is_utf8),
                RustFfiMetaCharsetScanResult::NeedMoreInput
            );
        }
        assert_eq!(
            scanner.scan(b"<p>", false, is_utf8),
            RustFfiMetaCharsetScanResult::NotFound
        );
    }

    #[test]
    fn a_frameset_holds_no_declaration() {
        let mut input = b"<!DOCTYPE html><frameset><meta data-padding=\"".to_vec();
        input.extend(std::iter::repeat_n(b'a', 2000));
        input.extend(b"\" charset=\"utf-8\"></frameset>");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);
    }

    #[test]
    fn ignores_a_declaration_that_names_no_encoding() {
        let mut input = padding();
        input.extend(b"<meta charset=\"bogus\"></head><body>");
        assert_eq!(scan_all(&input), RustFfiMetaCharsetScanResult::NotFound);
    }

    #[test]
    fn waits_for_more_input_in_the_head() {
        let mut scanner = MetaCharsetScanner::new(true);
        assert_eq!(
            scanner.scan(&padding(), false, is_utf8),
            RustFfiMetaCharsetScanResult::NeedMoreInput
        );
        // A tag that hasn't ended yet doesn't count, until it does.
        assert_eq!(
            scanner.scan(b"<meta charset=\"utf-8\"", false, is_utf8),
            RustFfiMetaCharsetScanResult::NeedMoreInput
        );
        assert_eq!(scanner.scan(b">", false, is_utf8), RustFfiMetaCharsetScanResult::Found);
    }

    #[test]
    fn resumes_inside_a_comment() {
        for (after_comment, expected) in [
            (
                "<title>title</title><meta charset=\"utf-8\">",
                RustFfiMetaCharsetScanResult::Found,
            ),
            (
                "<p>text</p><meta charset=\"utf-8\">",
                RustFfiMetaCharsetScanResult::NotFound,
            ),
            (
                "<script>var markup = '<meta charset=\"utf-8\">';</script><p>",
                RustFfiMetaCharsetScanResult::NotFound,
            ),
        ] {
            let mut input = b"<!DOCTYPE html><!-- ".to_vec();
            input.extend("padding ".repeat(95).bytes());
            input.extend(b"caf\xe9 ");
            input.extend("padding ".repeat(250).bytes());
            input.extend(b"-->");
            input.extend(after_comment.bytes());
            let split = 1800;
            let mut scanner = MetaCharsetScanner::new(true);
            assert_eq!(
                scanner.scan(&input[..split], false, is_utf8),
                RustFfiMetaCharsetScanResult::NeedMoreInput
            );
            assert_eq!(
                scanner.scan(&input[split..], true, is_utf8),
                expected,
                "{after_comment}"
            );
        }
    }

    #[test]
    fn an_unfinished_tag_at_the_end_of_the_input_does_not_count() {
        let mut scanner = MetaCharsetScanner::new(true);
        assert_eq!(
            scanner.scan(&padding(), false, is_utf8),
            RustFfiMetaCharsetScanResult::NeedMoreInput
        );
        assert_eq!(
            scanner.scan(b"<meta charset=\"utf-8\"", true, is_utf8),
            RustFfiMetaCharsetScanResult::NotFound
        );
    }
}
