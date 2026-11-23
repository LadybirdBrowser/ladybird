/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::RustFfiHtmlNamespace;
use super::RustFfiHtmlParserHandle;
use super::RustFfiHtmlParserRunResult;
use super::rust_html_parser_create;
use super::rust_html_parser_destroy;
use super::rust_html_parser_pop_all_open_elements;
use super::rust_html_parser_run_document;
use super::test_host;
use super::test_host::FakeInsertion;
use super::test_host::FakeInsertionCall;
use super::test_host::FakeNodeKind;
use crate::RustFfiTokenizerHandle;
use crate::make_handle;
use crate::rust_html_tokenizer_destroy;
use crate::tokenizer::HtmlTokenizer;
use ak::Utf16View;

const HTML_ELEMENT: FakeNodeKind = FakeNodeKind::Element(RustFfiHtmlNamespace::Html);

struct ScriptCreatedParser {
    parser: *mut RustFfiHtmlParserHandle,
    tokenizer: *mut RustFfiTokenizerHandle,
}

impl ScriptCreatedParser {
    fn open() -> Self {
        test_host::reset();
        let mut parser = Self {
            parser: rust_html_parser_create(),
            tokenizer: make_handle(HtmlTokenizer::new(Vec::<u8>::new())),
        };
        parser.tokenizer().set_input_stream_closed(false);
        parser.tokenizer().update_insertion_point();
        parser
    }

    fn tokenizer(&mut self) -> &mut HtmlTokenizer {
        unsafe { &mut (*self.tokenizer).tokenizer }
    }

    fn run(&mut self, stop_at_insertion_point: bool) {
        let result = unsafe {
            rust_html_parser_run_document(
                self.parser,
                self.tokenizer,
                test_host::fake_parser_host(),
                true,
                false,
                stop_at_insertion_point,
            )
        };
        assert_eq!(result, RustFfiHtmlParserRunResult::Ok);
    }

    fn write(&mut self, markup: &str) {
        let code_units: Vec<u16> = markup.encode_utf16().collect();
        self.tokenizer()
            .insert_input_at_insertion_point(Utf16View::Utf16(&code_units));
        self.run(true);
    }

    /// Gives the parser the rest of its input from the network rather than from a script.
    fn append_and_close(&mut self, markup: &str) {
        let code_units: Vec<u16> = markup.encode_utf16().collect();
        if code_units.iter().all(|code_unit| *code_unit <= 0xff) {
            let bytes: Vec<u8> = code_units.iter().map(|code_unit| *code_unit as u8).collect();
            self.tokenizer().append_input(Utf16View::Latin1(&bytes));
        } else {
            self.tokenizer().append_input(Utf16View::Utf16(&code_units));
        }
        self.close();
    }

    fn close(&mut self) {
        self.tokenizer().insert_eof();
        self.run(false);
        unsafe { rust_html_parser_pop_all_open_elements(self.parser) };
    }
}

impl Drop for ScriptCreatedParser {
    fn drop(&mut self) {
        unsafe {
            rust_html_parser_destroy(self.parser);
            rust_html_tokenizer_destroy(self.tokenizer);
        }
    }
}

fn body_element() -> usize {
    let html_element = *test_host::children(test_host::DOCUMENT).last().unwrap();
    test_host::children(html_element)[1]
}

fn kinds_of(nodes: &[usize]) -> Vec<FakeNodeKind> {
    nodes.iter().map(|&node| test_host::kind(node)).collect()
}

fn insertions_with_explicit_offset() -> Vec<FakeInsertion> {
    test_host::insertions()
        .into_iter()
        .filter(|insertion| insertion.explicit_offset.is_some())
        .collect()
}

#[test]
fn script_created_parser_appends_every_flat_sibling() {
    const SIBLING_COUNT: usize = 5000;
    let mut parser = ScriptCreatedParser::open();
    parser.write(&format!("{}<span>PASS</span>", "<span></span>".repeat(SIBLING_COUNT)));
    parser.close();

    let body = body_element();
    let insertions_into_body = test_host::insertions()
        .into_iter()
        .filter(|insertion| insertion.parent == body)
        .count();
    assert_eq!(insertions_into_body, SIBLING_COUNT + 1);
    let explicit_offset_insertions = insertions_with_explicit_offset();
    assert!(explicit_offset_insertions.is_empty(), "{explicit_offset_insertions:?}");

    let body_children = test_host::children(body);
    assert_eq!(kinds_of(&body_children), vec![HTML_ELEMENT; SIBLING_COUNT + 1]);
    assert_eq!(
        kinds_of(&test_host::children(body_children[SIBLING_COUNT])),
        [FakeNodeKind::Text("PASS".to_string())]
    );
}

#[test]
fn foster_parented_element_is_inserted_before_the_table_at_an_explicit_offset() {
    let mut parser = ScriptCreatedParser::open();
    parser.write("<table><b>fostered</b></table>");
    parser.close();

    let body = body_element();
    assert_eq!(
        insertions_with_explicit_offset(),
        [FakeInsertion {
            call: FakeInsertionCall::InsertNode,
            parent: body,
            explicit_offset: Some(0),
        }]
    );

    let [fostered_element, table] = test_host::children(body)[..] else {
        panic!("expected the fostered element and the table in body");
    };
    assert_eq!(kinds_of(&[fostered_element, table]), [HTML_ELEMENT, HTML_ELEMENT]);
    assert_eq!(
        kinds_of(&test_host::children(fostered_element)),
        [FakeNodeKind::Text("fostered".to_string())]
    );
    assert!(test_host::children(table).is_empty());
}

#[test]
fn foster_parented_text_is_inserted_before_the_table_at_an_explicit_offset() {
    let mut parser = ScriptCreatedParser::open();
    parser.write("<table>fostered</table>");
    parser.close();

    let body = body_element();
    let fostered_insertions = insertions_with_explicit_offset();
    assert!(!fostered_insertions.is_empty());
    assert!(
        fostered_insertions
            .iter()
            .all(|insertion| insertion.call == FakeInsertionCall::InsertText && insertion.parent == body)
    );

    let body_children = test_host::children(body);
    assert_eq!(
        kinds_of(&body_children),
        [FakeNodeKind::Text("fostered".to_string()), HTML_ELEMENT]
    );
    assert!(test_host::children(body_children[1]).is_empty());
}

fn text_of_each_paragraph(markup: &str) -> Vec<String> {
    let mut parser = ScriptCreatedParser::open();
    parser.append_and_close(markup);
    test_host::children(body_element())
        .into_iter()
        .map(|paragraph| match &test_host::children(paragraph)[..] {
            [text] => match test_host::kind(*text) {
                FakeNodeKind::Text(text) => text,
                kind => panic!("expected text in the paragraph, not {kind:?}"),
            },
            children => panic!("expected one text node in the paragraph, not {} nodes", children.len()),
        })
        .collect()
}

#[test]
fn text_is_inserted_as_one_node_however_the_tokenizer_emits_it() {
    // Text that is taken straight from the input, in both forms the input is stored in.
    assert_eq!(
        text_of_each_paragraph("<p>plain<p>caf\u{e9}\nau lait"),
        ["plain", "caf\u{e9}\nau lait"]
    );
    assert_eq!(
        text_of_each_paragraph("<p>plain<p>wide \u{2014} \u{1f600}!"),
        ["plain", "wide \u{2014} \u{1f600}!"]
    );
    // Text that is put together from runs of input and the characters between them that the tokenizer changes.
    assert_eq!(
        text_of_each_paragraph("<p>a &amp; b &lt;<p>x\r\ny\rz<p>&amp;<p>\u{1f600} first"),
        ["a & b <", "x\ny\nz", "&", "\u{1f600} first"]
    );
    // A surrogate without its other half becomes U+FFFD.
    let mut parser = ScriptCreatedParser::open();
    parser
        .tokenizer()
        .append_input(Utf16View::Utf16(&[0x3C, 0x70, 0x3E, 0x61, 0xD83D, 0x62]));
    parser.close();
    let paragraph = test_host::children(body_element())[0];
    assert_eq!(
        kinds_of(&test_host::children(paragraph)),
        [FakeNodeKind::Text("a\u{fffd}b".to_string())]
    );
}
