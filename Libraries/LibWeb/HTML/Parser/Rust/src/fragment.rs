/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The scanner of the fragment fast path.
//!
//! Markup given to innerHTML is very often a small subset of what goes in a body: a few kinds of elements, properly
//! nested, with quoted attribute values and no character references. The scanner reads such markup once and writes
//! down what the tree builder would have made of it as a flat list of operations, which the host then builds nodes
//! from without a tokenizer or a tree builder. Anything else is turned down before the host has made a single node,
//! and goes to the general parser.

use crate::code_units::RustFfiHtmlUtf16View;
use crate::known_names::known_attribute_name;
use crate::known_names::tag_id;
use crate::known_names::tag_id_for_name;
use crate::scan;
use crate::scan::CodeUnit;
use crate::token::TagId;
use crate::token::TagSet;
use crate::token::tag_set;
use std::cell::Cell;

#[repr(u8)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum RustFfiHtmlFragmentOperationKind {
    /// Make an element and the attributes at `start..start + length` of the attribute list, and append it to the
    /// innermost open element. It is then the innermost open element.
    OpenElement,
    /// The same for an element that takes no children, which is not left open.
    AppendVoidElement,
    /// Close the innermost open element.
    CloseElement,
    /// Append a text node with the code units at `start..start + length` of the input.
    AppendText,
}

#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RustFfiHtmlFragmentOperation {
    pub kind: RustFfiHtmlFragmentOperationKind,
    /// Whether the text of `AppendText` is all ASCII.
    pub is_ascii: bool,
    pub start: u32,
    pub length: u32,
    /// The identity of the `Utf16FlyString` with the local name of the element to make.
    pub tag_name: usize,
}

/// An attribute of an element to make. The name and the value are ranges of code units of the input.
#[repr(C)]
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct RustFfiHtmlFragmentAttribute {
    /// The identity of the `Utf16FlyString` with the name if it is one the engine knows, and otherwise 0.
    pub known_name: usize,
    pub name_start: u32,
    pub name_length: u32,
    pub value_start: u32,
    pub value_length: u32,
    pub value_is_ascii: bool,
}

/// What the scanner made of a fragment. `tape` is null if the fragment is not for the fast path, and otherwise must
/// be given to `rust_html_fragment_tape_release` once the operations have been read.
#[repr(C)]
pub struct RustFfiHtmlFragmentScan {
    pub tape: *mut RustHtmlFragmentTape,
    pub operations: *const RustFfiHtmlFragmentOperation,
    pub operation_count: usize,
    pub attributes: *const RustFfiHtmlFragmentAttribute,
    pub attribute_count: usize,
}

#[derive(Default)]
pub struct RustHtmlFragmentTape {
    operations: Vec<RustFfiHtmlFragmentOperation>,
    attributes: Vec<RustFfiHtmlFragmentAttribute>,
    open_elements: Vec<TagId>,
}

thread_local! {
    // NB: One tape is kept between fragments, so that scanning a fragment does not allocate.
    static SPARE_TAPE: Cell<Option<Box<RustHtmlFragmentTape>>> = const { Cell::new(None) };
}

// NB: A tape that an unusually large fragment grew past these sizes is freed, so that the spare one stays small.
const MAX_SPARE_TAPE_OPERATIONS: usize = 4096;
const MAX_SPARE_TAPE_ATTRIBUTES: usize = 4096;

fn keep_as_spare_tape(tape: Box<RustHtmlFragmentTape>) {
    if tape.operations.capacity() <= MAX_SPARE_TAPE_OPERATIONS
        && tape.attributes.capacity() <= MAX_SPARE_TAPE_ATTRIBUTES
    {
        SPARE_TAPE.set(Some(tape));
    }
}

/// Scans a fragment for the fast path.
///
/// # Safety
/// `input` must point at `input.length` readable code units.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_fragment_scan(input: RustFfiHtmlUtf16View) -> RustFfiHtmlFragmentScan {
    let mut tape = SPARE_TAPE.take().unwrap_or_default();
    // SAFETY: The caller promises that `input` points at `input.length` code units.
    let accepted = unsafe {
        if input.length == 0 {
            scan::<u8>(&[], &mut tape)
        } else if !input.latin1.is_null() {
            scan(ak::slice_from_raw_parts(input.latin1, input.length), &mut tape)
        } else {
            scan(ak::slice_from_raw_parts(input.utf16, input.length), &mut tape)
        }
    };
    if !accepted {
        keep_as_spare_tape(tape);
        return RustFfiHtmlFragmentScan {
            tape: std::ptr::null_mut(),
            operations: std::ptr::null(),
            operation_count: 0,
            attributes: std::ptr::null(),
            attribute_count: 0,
        };
    }
    RustFfiHtmlFragmentScan {
        operations: tape.operations.as_ptr(),
        operation_count: tape.operations.len(),
        attributes: tape.attributes.as_ptr(),
        attribute_count: tape.attributes.len(),
        tape: Box::into_raw(tape),
    }
}

/// Gives back the tape of a scan whose operations have been read.
///
/// # Safety
/// `tape` must be the tape of a `RustFfiHtmlFragmentScan` that has not been released yet.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_html_fragment_tape_release(tape: *mut RustHtmlFragmentTape) {
    // SAFETY: The caller promises that `tape` came from `rust_html_fragment_scan` and is released once.
    keep_as_spare_tape(unsafe { Box::from_raw(tape) });
}

/// Defines the tags the fast path takes, and the fly string of each.
///
/// Keep this subset deliberately small. Adding a tag requires checking its in-body tree-builder rules, its insertion
/// steps, and interactions with every other supported tag. A supported tag must not have children changed steps,
/// since the host links children below a new element without running them.
macro_rules! supported_tags {
    ($($name:tt),+ $(,)?) => {
        const SUPPORTED_TAGS: &TagSet = tag_set![$($name),+];

        /// The identity of the `Utf16FlyString` of every supported tag, by tag id.
        #[cfg(not(test))]
        fn tag_names() -> &'static [usize; TAG_ID_COUNT] {
            static TAG_NAMES: std::sync::OnceLock<[usize; TAG_ID_COUNT]> = std::sync::OnceLock::new();
            TAG_NAMES.get_or_init(|| {
                let mut tag_names = [0; TAG_ID_COUNT];
                $(tag_names[tag_id!($name).index()] = crate::known_names::tag_name!($name).raw_identity();)+
                tag_names
            })
        }
    };
}

#[cfg(not(test))]
const TAG_ID_COUNT: usize = crate::known_names::TAG_SET_WORD_COUNT * 64;

supported_tags![
    "a", "article", "aside", "b", "big", "br", "button", "code", "div", "em", "footer", "header", "i", "input",
    "label", "li", "main", "nav", "ol", "s", "section", "small", "span", "strike", "strong", "tt", "u", "ul",
];

const MAX_OPEN_ELEMENTS: usize = 128;
const MAX_ATTRIBUTES: usize = 64;

/// The whitespace between the parts of a tag.
fn is_tag_whitespace(code_unit: u16) -> bool {
    matches!(code_unit, 0x09 | 0x0A | 0x0C | 0x20)
}

fn equals_ascii<T: CodeUnit>(code_units: &[T], ascii: &[u8]) -> bool {
    code_units.len() == ascii.len()
        && code_units
            .iter()
            .zip(ascii)
            .all(|(&code_unit, &byte)| code_unit.into() == u16::from(byte))
}

fn scan<T: CodeUnit>(input: &[T], tape: &mut RustHtmlFragmentTape) -> bool {
    tape.operations.clear();
    tape.attributes.clear();
    tape.open_elements.clear();
    // NB: The tape has offsets into the input as 32-bit numbers.
    if u32::try_from(input.len()).is_err() {
        return false;
    }
    Scanner {
        input,
        offset: 0,
        #[cfg(not(test))]
        tag_names: tag_names(),
        tape,
    }
    .scan()
}

struct Scanner<'a, T> {
    input: &'a [T],
    offset: usize,
    #[cfg(not(test))]
    tag_names: &'static [usize; TAG_ID_COUNT],
    tape: &'a mut RustHtmlFragmentTape,
}

impl<'a, T: CodeUnit> Scanner<'a, T> {
    fn scan(&mut self) -> bool {
        while self.offset < self.input.len() {
            let scanned = if self.peek() != u16::from(b'<') {
                self.scan_text()
            } else {
                self.offset += 1;
                if self.consume(b'/') {
                    self.scan_end_tag()
                } else {
                    self.scan_start_tag()
                }
            };
            if !scanned {
                return false;
            }
        }
        self.tape.open_elements.is_empty()
    }

    fn scan_text(&mut self) -> bool {
        // NB: Character references and input preprocessing use the general parser, which also replaces a surrogate
        //     that isn't half of a pair with U+FFFD. The run ends at all of those, so it has to end at a tag or at the
        //     end of the input.
        let run = scan::data_run(&self.input[self.offset..]);
        let end = self.offset + run.length;
        if end < self.input.len() && self.input[end].into() != u16::from(b'<') {
            return false;
        }
        self.tape.operations.push(RustFfiHtmlFragmentOperation {
            kind: RustFfiHtmlFragmentOperationKind::AppendText,
            is_ascii: run.is_ascii,
            start: self.offset as u32,
            length: run.length as u32,
            tag_name: 0,
        });
        self.offset = end;
        true
    }

    fn scan_end_tag(&mut self) -> bool {
        let Some(tag) = self.scan_supported_tag() else {
            return false;
        };
        self.skip_whitespace();
        if !self.consume(b'>') || self.tape.open_elements.pop() != Some(tag) {
            return false;
        }
        self.tape.operations.push(RustFfiHtmlFragmentOperation {
            kind: RustFfiHtmlFragmentOperationKind::CloseElement,
            is_ascii: false,
            start: 0,
            length: 0,
            tag_name: 0,
        });
        true
    }

    fn scan_start_tag(&mut self) -> bool {
        let Some(tag) = self.scan_supported_tag() else {
            return false;
        };
        let open_elements = &self.tape.open_elements;

        // Leave implicit closing of anchors, buttons, and list items to the general parser.
        if (tag == tag_id!("a") || tag == tag_id!("button")) && open_elements.contains(&tag) {
            return false;
        }
        if tag == tag_id!("li") {
            for &ancestor in open_elements.iter().rev() {
                if ancestor == tag_id!("li") {
                    return false;
                }
                // A nested list stops the in-body rule from closing an outer list item.
                if ancestor == tag_id!("ul") || ancestor == tag_id!("ol") {
                    break;
                }
            }
        }
        if open_elements.len() >= MAX_OPEN_ELEMENTS {
            return false;
        }

        let first_attribute = self.tape.attributes.len();
        loop {
            self.skip_whitespace();
            if self.peek() == u16::from(b'>') || self.peek() == u16::from(b'/') {
                break;
            }
            if !self.scan_attribute(tag, first_attribute) {
                return false;
            }
        }

        let is_void = tag == tag_id!("br") || tag == tag_id!("input");
        if self.consume(b'/') && !is_void {
            return false;
        }
        if !self.consume(b'>') {
            return false;
        }

        self.tape.operations.push(RustFfiHtmlFragmentOperation {
            kind: if is_void {
                RustFfiHtmlFragmentOperationKind::AppendVoidElement
            } else {
                RustFfiHtmlFragmentOperationKind::OpenElement
            },
            is_ascii: false,
            start: first_attribute as u32,
            length: (self.tape.attributes.len() - first_attribute) as u32,
            tag_name: self.tag_name(tag),
        });
        if !is_void {
            self.tape.open_elements.push(tag);
        }
        true
    }

    /// Scans one attribute of a start tag of `tag`, whose attributes so far start at `first_attribute`.
    fn scan_attribute(&mut self, tag: TagId, first_attribute: usize) -> bool {
        let input = self.input;
        let name_start = self.offset;
        let name = self.scan_name();
        // NB: Customized built-in elements are made by the general parser.
        if name.is_empty()
            || equals_ascii(name, b"is")
            || self.tape.attributes.len() - first_attribute >= MAX_ATTRIBUTES
        {
            return false;
        }
        // NB: The tokenizer drops an attribute whose name the tag already has, which the general parser sees to.
        let is_duplicate = self.tape.attributes[first_attribute..].iter().any(|attribute| {
            let start = attribute.name_start as usize;
            input[start..start + attribute.name_length as usize] == *name
        });
        if is_duplicate {
            return false;
        }

        self.skip_whitespace();
        let mut value = scan::Run {
            length: 0,
            is_ascii: true,
        };
        let mut value_start = self.offset;
        if self.consume(b'=') {
            self.skip_whitespace();
            let quote = self.peek();
            if quote != u16::from(b'\'') && quote != u16::from(b'"') {
                return false;
            }
            self.offset += 1;
            value_start = self.offset;
            // NB: As with text, the run ends at everything that the general parser has to see to, so it has to end at
            //     the closing quote.
            value = scan::quoted_attribute_value_run(&input[value_start..], quote as u8);
            self.offset = value_start + value.length;
            if !self.consume(quote as u8) {
                return false;
            }
        }

        // NB: Other input types have insertion steps or children the fast path doesn't cover.
        let value_code_units = &input[value_start..value_start + value.length];
        if tag == tag_id!("input")
            && equals_ascii(name, b"type")
            && !equals_ascii(value_code_units, b"text")
            && !equals_ascii(value_code_units, b"checkbox")
        {
            return false;
        }

        self.tape.attributes.push(RustFfiHtmlFragmentAttribute {
            known_name: known_attribute_name_identity(name),
            name_start: name_start as u32,
            name_length: name.len() as u32,
            value_start: value_start as u32,
            value_length: value.length as u32,
            value_is_ascii: value.is_ascii,
        });
        true
    }

    fn scan_supported_tag(&mut self) -> Option<TagId> {
        let name = self.scan_name();
        let tag = tag_id_for_name(scan::ascii_name_bytes(name, &mut [0; 32])?);
        (tag != TagId::UNKNOWN && SUPPORTED_TAGS.contains(tag)).then_some(tag)
    }

    /// Scans a tag name or an attribute name of lowercase ASCII letters, digits and hyphens. Other names are for the
    /// general parser, which is what finding no name where one has to be leads to.
    fn scan_name(&mut self) -> &'a [T] {
        let input = self.input;
        let start = self.offset;
        while matches!(self.peek(), 0x61..=0x7A | 0x30..=0x39 | 0x2D) {
            self.offset += 1;
        }
        &input[start..self.offset]
    }

    fn tag_name(&self, tag: TagId) -> usize {
        #[cfg(not(test))]
        {
            self.tag_names[tag.index()]
        }
        // NB: There are no fly strings in the standalone tests, which get the tag id in place of one.
        #[cfg(test)]
        {
            tag.index()
        }
    }

    /// The code unit at the offset, or U+0000 NULL at the end of the input. Nothing the scanner takes has a NULL in it.
    fn peek(&self) -> u16 {
        self.input.get(self.offset).map_or(0, |&code_unit| code_unit.into())
    }

    fn consume(&mut self, byte: u8) -> bool {
        if self.offset == self.input.len() || self.peek() != u16::from(byte) {
            return false;
        }
        self.offset += 1;
        true
    }

    fn skip_whitespace(&mut self) {
        while is_tag_whitespace(self.peek()) {
            self.offset += 1;
        }
    }
}

fn known_attribute_name_identity<T: CodeUnit>(name: &[T]) -> usize {
    #[cfg(not(test))]
    {
        scan::ascii_name_bytes(name, &mut [0; 32])
            .and_then(known_attribute_name)
            .map_or(0, |name| name.raw_identity())
    }
    // NB: There are no fly strings in the standalone tests, which only tell known names from other names.
    #[cfg(test)]
    {
        usize::from(
            scan::ascii_name_bytes(name, &mut [0; 32])
                .and_then(known_attribute_name)
                .is_some(),
        )
    }
}

#[cfg(test)]
mod tests {
    use super::RustFfiHtmlFragmentOperationKind::*;
    use super::*;

    /// What the scanner makes of `input`, as one line per operation, or `None` if it turns the input down.
    fn scan_to_text<T: CodeUnit>(input: &[T]) -> Option<String> {
        let mut tape = RustHtmlFragmentTape::default();
        if !scan(input, &mut tape) {
            return None;
        }
        let text_of = |start: u32, length: u32| {
            let code_units: Vec<u16> = input[start as usize..(start + length) as usize]
                .iter()
                .map(|&code_unit| code_unit.into())
                .collect();
            String::from_utf16(&code_units).unwrap()
        };
        let mut text = String::new();
        for operation in &tape.operations {
            match operation.kind {
                OpenElement | AppendVoidElement => {
                    text.push_str(if operation.kind == OpenElement { "open" } else { "void" });
                    text.push_str(&format!(" #{}", operation.tag_name));
                    let attributes = &tape.attributes[operation.start as usize..][..operation.length as usize];
                    for attribute in attributes {
                        text.push_str(&format!(
                            " {}{}={:?}{}",
                            text_of(attribute.name_start, attribute.name_length),
                            if attribute.known_name != 0 { "!" } else { "" },
                            text_of(attribute.value_start, attribute.value_length),
                            if attribute.value_is_ascii { "" } else { "~" },
                        ));
                    }
                }
                CloseElement => text.push_str("close"),
                AppendText => text.push_str(&format!(
                    "text {:?}{}",
                    text_of(operation.start, operation.length),
                    if operation.is_ascii { "" } else { "~" }
                )),
            }
            text.push('\n');
        }
        Some(text)
    }

    fn scan_str(input: &str) -> Option<String> {
        let utf16: Vec<u16> = input.encode_utf16().collect();
        let from_utf16 = scan_to_text(&utf16);
        if let Some(latin1) = utf16
            .iter()
            .map(|&code_unit| u8::try_from(code_unit).ok())
            .collect::<Option<Vec<u8>>>()
        {
            assert_eq!(scan_to_text(&latin1), from_utf16, "Latin-1 and UTF-16 input scan alike");
        }
        from_utf16
    }

    fn accepts(input: &str) -> bool {
        scan_str(input).is_some()
    }

    #[test]
    fn elements_attributes_and_text_become_operations() {
        let div = tag_id!("div").index();
        let input = tag_id!("input").index();
        assert_eq!(
            scan_str("a<div class=\"x y\" data-id='7' hidden>caf\u{E9}<input type = \"checkbox\" /></div >").unwrap(),
            format!(
                "text \"a\"\nopen #{div} class!=\"x y\" data-id=\"7\" hidden!=\"\"\ntext \"caf\u{E9}\"~\n\
                 void #{input} type!=\"checkbox\"\nclose\n"
            )
        );
        assert_eq!(scan_str("").unwrap(), "");
        assert_eq!(scan_str("x > y").unwrap(), "text \"x > y\"\n");
    }

    #[test]
    fn markup_the_tree_builder_takes_as_written_is_accepted() {
        for input in [
            "<br>",
            "<br/>",
            "<input>",
            "<li><ul><li></li></ul></li>",
            "<a><b></b></a><a></a>",
            "<div title='a<b>\"'></div>",
            "<div\n\tclass=''\x0C></div\n>",
        ] {
            assert!(accepts(input), "{input:?}");
        }
    }

    #[test]
    fn markup_for_the_general_parser_is_turned_down() {
        for input in [
            "a &amp; b",
            "a\rb",
            "a\0b",
            "<p>x</p>",
            "<my-element></my-element>",
            "<DIV></DIV>",
            "<div>",
            "</div>",
            "<div></span>",
            "<div/>",
            "</br>",
            "<div",
            "<div class='a",
            "<div class=a></div>",
            "<div Class='a'></div>",
            "<div class='a' class='b'></div>",
            "<div class='a&amp;b'></div>",
            "<div class='a\rb'></div>",
            "<div is='x'></div>",
            "<a><b><a></a></b></a>",
            "<button><button></button></button>",
            "<li><li></li></li>",
            "<ul><li><b><li></li></b></li></ul>",
            "<input type='radio'>",
            "<input type>",
            "<!-- comment -->",
        ] {
            assert!(!accepts(input), "{input:?}");
        }
        let too_deep = "<div>".repeat(MAX_OPEN_ELEMENTS + 1) + &"</div>".repeat(MAX_OPEN_ELEMENTS + 1);
        assert!(!accepts(&too_deep));
        assert!(accepts(&too_deep["<div>".len()..too_deep.len() - "</div>".len()]));
    }

    #[test]
    fn surrogates_are_taken_in_pairs_only() {
        let code_units = |text: &str, surrogate: &[u16]| {
            let mut code_units: Vec<u16> = text.encode_utf16().collect();
            let offset = code_units
                .iter()
                .position(|&code_unit| code_unit == u16::from(b'?'))
                .unwrap();
            code_units.splice(offset..offset + 1, surrogate.iter().copied());
            code_units
        };
        for text in ["a?b", "<div title='a?b'></div>"] {
            assert!(scan_to_text(&code_units(text, &[0xD83D, 0xDE00])).is_some());
            assert!(scan_to_text(&code_units(text, &[0xD83D])).is_none());
            assert!(scan_to_text(&code_units(text, &[0xDE00])).is_none());
            assert!(scan_to_text(&code_units(text, &[0xDE00, 0xD83D])).is_none());
        }
        assert_eq!(
            scan_to_text(&code_units("?", &[0xD83D, 0xDE00])).unwrap(),
            "text \"\u{1F600}\"~\n"
        );
    }
}
