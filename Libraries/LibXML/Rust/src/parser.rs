/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A non-validating parser for XML 1.0 (fifth edition) with Namespaces in XML 1.0.
//!
//! It takes a whole document as UTF-8 and writes a `Tape`. External entities are never read. Where the specifications
//! leave a choice, it does what libxml2 does when driven the way a browser drives it.

use std::collections::HashSet;
use std::rc::Rc;

use crate::chars;
use crate::chars::CLASS;
use crate::dtd::Dtd;
use crate::dtd::EntityKind;
use crate::names::Names;
use crate::names::Storage;
use crate::names::flag;
use crate::scan;
use crate::scan::Controls;
use crate::tape::NONE;
use crate::tape::Tape;
use crate::tape::kind;

pub const XML_NAMESPACE: &[u8] = b"http://www.w3.org/XML/1998/namespace";
pub const XMLNS_NAMESPACE: &[u8] = b"http://www.w3.org/2000/xmlns/";

/// How deep elements can nest.
const MAX_ELEMENT_DEPTH: usize = 5000;
/// How deep entity references can nest.
pub(crate) const MAX_ENTITY_DEPTH: u32 = 20;
/// Entity expansion is free up to this many bytes. Past it, the expansions of a document together can't be more than
/// `MAX_AMPLIFICATION` times its size.
const ALLOWED_EXPANSION: u64 = 1_000_000;
const MAX_AMPLIFICATION: u64 = 5;
/// What every expansion costs on top of its length, so that a flood of references to an empty entity has a cost too.
const ENTITY_FIXED_COST: u64 = 20;

/// The arena starts with these, so that the text of a predefined entity or a normalized line end is a row like any
/// other, without a copy.
const ARENA_CONSTANTS: &[u8] = b"\n<>&'\"";
const CONSTANT_NEWLINE: u32 = 0;
const CONSTANT_LT: u32 = 1;
const CONSTANT_GT: u32 = 2;
const CONSTANT_AMP: u32 = 3;
const CONSTANT_APOS: u32 = 4;
const CONSTANT_QUOT: u32 = 5;

/// Resolves the name of an HTML character entity, appending its text as UTF-8 and returning true if there is one.
pub type HtmlEntityResolver<'a> = &'a dyn Fn(&[u8], &mut Vec<u8>) -> bool;

#[derive(Clone, Copy)]
pub struct Options<'a> {
    /// Whether comments get rows. They are checked either way.
    pub preserve_comments: bool,
    /// Whether a CDATA section gets a CDATA row, as opposed to a TEXT row.
    pub preserve_cdata: bool,
    /// Used for references to undeclared entities in a document whose doctype has one of the XHTML public ids.
    pub resolve_html_entity: Option<HtmlEntityResolver<'a>>,
}

impl Default for Options<'_> {
    fn default() -> Self {
        Options {
            preserve_comments: false,
            preserve_cdata: true,
            resolve_html_entity: None,
        }
    }
}

pub struct Error {
    /// Where in the source the error is. For an error inside an entity's text, where the reference to the entity is.
    pub offset: u32,
    pub line: u32,
    pub column: u32,
    /// The row the error comes before: everything ahead of it was parsed before the error was found.
    pub row: u32,
    /// A fatal error is the last thing in a document: the tape ends there. After any other error (the namespace
    /// constraints) the parser carries on.
    pub fatal: bool,
    pub message: String,
}

pub struct Document {
    pub tape: Tape,
    /// The second half of the address space string operands point into. See `Tape`.
    pub arena: Vec<u8>,
    pub names: Names,
    pub errors: Vec<Error>,
    /// Whether the document is XML 1.0, as opposed to a later 1.x.
    pub version_1_0: bool,
    /// Whether the parser got as far as the document itself: there is something in it, and its XML declaration, if
    /// it has one, is in order.
    pub started: bool,
}

impl Document {
    pub fn storage<'a>(&'a self, source: &'a [u8]) -> Storage<'a> {
        Storage {
            source,
            arena: &self.arena,
        }
    }
}

/// The bytes being parsed: the document, or the replacement text of an entity.
#[derive(Clone, Copy)]
pub(crate) struct In<'i> {
    pub bytes: &'i [u8],
    /// Whether `bytes` is the document. If it isn't, nothing on the tape can point into it.
    pub main: bool,
}

/// A fatal error was reported.
pub(crate) struct Stop;
pub(crate) type R<T> = Result<T, Stop>;

#[derive(Clone, Copy)]
struct Open {
    name: u32,
    /// How many namespace bindings the element's start tag added.
    bindings: u32,
    /// Where the start tag is, for the error message of a mismatched end tag.
    offset: u32,
}

#[derive(Clone, Copy)]
struct Binding {
    prefix: u32,
    namespace: u32,
    /// The binding this one shadows, or NONE.
    previous: u32,
    /// The depth of the element that declares it.
    depth: u32,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Delimited {
    Comment,
    ProcessingInstruction,
    CData,
}

pub(crate) struct Parser<'s, 'o> {
    pub(crate) src: &'s [u8],
    pub(crate) options: Options<'o>,
    pub(crate) tape: Tape,
    pub(crate) arena: Vec<u8>,
    pub(crate) names: Names,
    pub(crate) errors: Vec<Error>,
    stack: Vec<Open>,
    bindings: Vec<Binding>,
    default_namespace: u32,
    /// Counts start tags. See `Name::stamp`.
    stamp: u32,
    /// While a start tag is being parsed, its row.
    tag_row: u32,
    pub(crate) scratch: Vec<u8>,
    rows_scratch: Vec<(u8, u32, u32)>,
    prefixed_scratch: Vec<(u32, u32)>,
    pub(crate) dtd: Option<Box<Dtd>>,
    pub(crate) has_external_subset: bool,
    pub(crate) has_parameter_entity_references: bool,
    pub(crate) standalone: bool,
    pub(crate) is_xhtml: bool,
    version_1_0: bool,
    started: bool,
    pub(crate) entity_depth: u32,
    expanded: u64,
    /// While an entity's text is being parsed, where in the document the outermost reference is.
    pub(crate) entity_reference_offset: usize,
    arena_overflow: bool,
    xmlns_name: u32,
    xml_namespace: u32,
    xmlns_namespace: u32,
}

#[inline(always)]
pub(crate) fn at(bytes: &[u8], pos: usize) -> u8 {
    if pos < bytes.len() { bytes[pos] } else { 0 }
}

#[inline(always)]
pub(crate) fn skip_spaces(bytes: &[u8], mut pos: usize) -> usize {
    while pos < bytes.len() && chars::is_space(bytes[pos]) {
        pos += 1;
    }
    pos
}

/// Returns where the Name that starts at `start` ends, which is `start` if there is no Name there.
#[inline(always)]
pub(crate) fn scan_name(bytes: &[u8], start: usize) -> usize {
    if start >= bytes.len() {
        return start;
    }
    let mut pos = start;
    let first = bytes[pos];
    if first < 0x80 {
        if CLASS[first as usize] & chars::NAME_START == 0 {
            return start;
        }
        pos += 1;
    } else {
        match chars::decode_utf8(bytes, pos) {
            Some((c, length)) if chars::is_name_start_char(c) => pos += length,
            _ => return start,
        }
    }
    // What follows the first character of a Name is an Nmtoken.
    scan_nmtoken(bytes, pos)
}

/// Returns where the Nmtoken that starts at `start` ends.
#[inline(always)]
pub(crate) fn scan_nmtoken(bytes: &[u8], start: usize) -> usize {
    let mut pos = start;
    loop {
        pos = scan::ascii_name_end(bytes, pos);
        if pos < bytes.len()
            && bytes[pos] >= 0x80
            && let Some((c, length)) = chars::decode_utf8(bytes, pos)
            && chars::is_name_char(c)
        {
            pos += length;
            continue;
        }
        return pos;
    }
}

pub(crate) fn lossy(bytes: &[u8]) -> std::borrow::Cow<'_, str> {
    String::from_utf8_lossy(bytes)
}

fn predefined_entity(name: &[u8]) -> Option<u32> {
    match name {
        b"lt" => Some(CONSTANT_LT),
        b"gt" => Some(CONSTANT_GT),
        b"amp" => Some(CONSTANT_AMP),
        b"apos" => Some(CONSTANT_APOS),
        b"quot" => Some(CONSTANT_QUOT),
        _ => None,
    }
}

impl<'s, 'o> Parser<'s, 'o> {
    pub(crate) fn new(src: &'s [u8], options: Options<'o>) -> Self {
        let mut arena = Vec::with_capacity(256);
        arena.extend_from_slice(ARENA_CONSTANTS);
        let mut parser = Parser {
            src,
            options,
            // OPTIMIZATION: Markup-heavy documents come to about one row per ten bytes. Starting near that saves the
            // columns from being copied over and over as they grow.
            tape: Tape::with_capacity(src.len() / 8),
            arena,
            names: Names::new(),
            errors: Vec::new(),
            stack: Vec::with_capacity(32),
            bindings: Vec::with_capacity(8),
            default_namespace: NONE,
            stamp: 0,
            tag_row: NONE,
            scratch: Vec::new(),
            rows_scratch: Vec::new(),
            prefixed_scratch: Vec::new(),
            dtd: None,
            has_external_subset: false,
            has_parameter_entity_references: false,
            standalone: false,
            is_xhtml: false,
            version_1_0: true,
            started: false,
            entity_depth: 0,
            expanded: 0,
            entity_reference_offset: 0,
            arena_overflow: false,
            xmlns_name: 0,
            xml_namespace: 0,
            xmlns_namespace: 0,
        };
        parser.xmlns_name = parser.intern_copied(b"xmlns");
        parser.xml_namespace = parser.intern_copied(XML_NAMESPACE);
        parser.xmlns_namespace = parser.intern_copied(XMLNS_NAMESPACE);
        parser
    }

    pub(crate) fn finish(mut self) -> Document {
        // Line and column are only worked out here, so that the parser never has to count lines. A document can have
        // an error in every tag (the namespace constraints), so the lines are counted on from one error to the next
        // and not from the start for each. The errors are nearly in the order of their offsets, but not quite (one
        // found at the end of a start tag is reported at its start), so they are visited in that order.
        let mut by_offset: Vec<u32> = (0..self.errors.len() as u32).collect();
        by_offset.sort_by_key(|&index| self.errors[index as usize].offset);
        let mut counted_to = 0;
        let mut line = 1;
        let mut line_start = 0;
        for index in by_offset {
            let error = &mut self.errors[index as usize];
            let offset = (error.offset as usize).min(self.src.len());
            let between = &self.src[counted_to..offset];
            line += between.iter().filter(|&&byte| byte == b'\n').count() as u32;
            if let Some(index) = between.iter().rposition(|&byte| byte == b'\n') {
                line_start = counted_to + index + 1;
            }
            counted_to = offset;
            error.line = line;
            error.column = (offset - line_start) as u32 + 1;
            error.row = error.row.min(self.tape.len() as u32);
        }
        Document {
            tape: self.tape,
            arena: self.arena,
            names: self.names,
            errors: self.errors,
            version_1_0: self.version_1_0,
            started: self.started,
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Errors
    // ------------------------------------------------------------------------------------------------------------

    fn report(&mut self, inp: In, pos: usize, fatal: bool, message: String) {
        let offset = if inp.main { pos } else { self.entity_reference_offset };
        let row = if self.tag_row != NONE {
            self.tag_row
        } else {
            self.tape.len() as u32
        };
        self.errors.push(Error {
            offset: offset as u32,
            line: 0,
            column: 0,
            row,
            fatal,
            message,
        });
    }

    #[cold]
    #[inline(never)]
    pub(crate) fn fail<T>(&mut self, inp: In, pos: usize, message: impl Into<String>) -> R<T> {
        self.report(inp, pos, true, message.into());
        Err(Stop)
    }

    /// Reports a broken namespace constraint. The document isn't namespace-well-formed, but parsing goes on.
    #[cold]
    #[inline(never)]
    pub(crate) fn namespace_error(&mut self, inp: In, pos: usize, message: String) {
        self.report(inp, pos, false, message);
    }

    // ------------------------------------------------------------------------------------------------------------
    // Strings
    // ------------------------------------------------------------------------------------------------------------

    #[inline(always)]
    pub(crate) fn storage(&self) -> Storage<'_> {
        Storage::new(self.src, &self.arena)
    }

    /// Appends `bytes` to the arena and returns the string operands for them.
    pub(crate) fn arena_push(&mut self, bytes: &[u8]) -> (u32, u32) {
        let offset = self.src.len() + self.arena.len();
        if offset + bytes.len() >= u32::MAX as usize {
            self.arena_overflow = true;
            return (self.src.len() as u32, 0);
        }
        self.arena.extend_from_slice(bytes);
        (offset as u32, bytes.len() as u32)
    }

    #[inline(always)]
    fn constant(&self, index: u32) -> u32 {
        self.src.len() as u32 + index
    }

    /// The string operands for `inp.bytes[start..end]`.
    #[inline(always)]
    pub(crate) fn string(&mut self, inp: In, start: usize, end: usize) -> (u32, u32) {
        if inp.main {
            (start as u32, (end - start) as u32)
        } else {
            self.arena_push(&inp.bytes[start..end])
        }
    }

    #[inline(always)]
    pub(crate) fn intern_name(&mut self, inp: In, start: usize, end: usize) -> u32 {
        if inp.main {
            self.names.intern_in_source(self.src, start, end, &self.arena)
        } else {
            self.intern_copied(&inp.bytes[start..end])
        }
    }

    /// Interns a name that isn't written anywhere in the document.
    pub(crate) fn intern_copied(&mut self, bytes: &[u8]) -> u32 {
        if let Some(id) = self.names.find(bytes, Storage::new(self.src, &self.arena)) {
            return id;
        }
        let (offset, _) = self.arena_push(bytes);
        self.names.intern(bytes, offset, Storage::new(self.src, &self.arena))
    }

    /// A name as an error message quotes it. A message can quote a name that isn't spelled out where the error is
    /// (that of the element an attribute is on), so a very long one is cut short, or the messages of a document
    /// could take far more memory than the document.
    pub(crate) fn name_string(&self, id: u32) -> String {
        const LONGEST_QUOTED_NAME: usize = 256;
        let bytes = self.names.bytes(id, self.storage());
        if bytes.len() <= LONGEST_QUOTED_NAME {
            return lossy(bytes).into_owned();
        }
        let mut end = LONGEST_QUOTED_NAME;
        while bytes[end] & 0xC0 == 0x80 {
            end -= 1;
        }
        format!("{}...", lossy(&bytes[..end]))
    }

    /// Checks the character that starts at `bytes[pos]`, a byte at or above 0x80, and returns its length.
    #[inline(always)]
    pub(crate) fn check_non_ascii(&mut self, inp: In, pos: usize) -> R<usize> {
        // OPTIMIZATION: Most text that isn't ASCII is in the two-byte range (Latin with diacritics, Greek, Cyrillic,
        // Hebrew, Arabic) or in the three-byte range below the surrogates (most of everything else). There a
        // sequence only has to have its continuation bytes to be a character XML allows.
        let bytes = inp.bytes;
        let lead = bytes[pos];
        if pos + 2 < bytes.len() {
            let second = bytes[pos + 1] & 0xC0 == 0x80;
            if (0xC2..0xE0).contains(&lead) && second {
                return Ok(2);
            }
            if (0xE1..0xED).contains(&lead) && second && bytes[pos + 2] & 0xC0 == 0x80 {
                return Ok(3);
            }
        }
        self.check_non_ascii_slow(inp, pos)
    }

    #[inline(never)]
    fn check_non_ascii_slow(&mut self, inp: In, pos: usize) -> R<usize> {
        match chars::decode_utf8(inp.bytes, pos) {
            Some((c, length)) if c < 0xFFFE => Ok(length),
            Some((c, length)) if chars::is_char(c) => Ok(length),
            Some((c, _)) => self.fail(inp, pos, format!("PCDATA invalid Char value {c}")),
            None => self.fail(inp, pos, "Input is not proper UTF-8, indicate encoding !"),
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // The document
    // ------------------------------------------------------------------------------------------------------------

    pub(crate) fn parse(&mut self) {
        if self.src.len() >= i32::MAX as usize {
            let _ = self.fail::<()>(
                In {
                    bytes: self.src,
                    main: true,
                },
                0,
                "Document too large",
            );
            return;
        }
        let _ = self.document();
        if self.arena_overflow && !self.errors.iter().any(|error| error.fatal) {
            let _ = self.fail::<()>(
                In {
                    bytes: self.src,
                    main: true,
                },
                self.src.len(),
                "Document too large",
            );
        }
    }

    fn document(&mut self) -> R<()> {
        let inp = In {
            bytes: self.src,
            main: true,
        };
        let bytes = self.src;
        let mut pos = 0;
        if bytes.starts_with(b"\xEF\xBB\xBF") {
            pos = 3;
        }
        if pos >= bytes.len() {
            return self.fail(inp, pos, "Document is empty");
        }
        if bytes[pos..].starts_with(b"<?xml") && chars::is_space(at(bytes, pos + 5)) {
            pos = self.xml_declaration(inp, pos)?;
        }
        self.started = true;

        let mut seen_doctype = false;
        loop {
            pos = skip_spaces(bytes, pos);
            if at(bytes, pos) != b'<' {
                return self.fail(inp, pos, "Start tag expected, '<' not found");
            }
            match at(bytes, pos + 1) {
                b'?' => pos = self.processing_instruction(inp, pos, true)?,
                b'!' => {
                    if bytes[pos..].starts_with(b"<!--") {
                        pos = self.comment(inp, pos, true)?;
                    } else if !seen_doctype && bytes[pos..].starts_with(b"<!DOCTYPE") {
                        seen_doctype = true;
                        pos = self.doctype(inp, pos)?;
                    } else {
                        return self.fail(inp, pos, "StartTag: invalid element name");
                    }
                }
                _ => break,
            }
        }

        pos = self.start_tag(inp, pos)?;
        if !self.stack.is_empty() {
            pos = self.content(inp, pos, 0)?;
        }

        loop {
            pos = skip_spaces(bytes, pos);
            if pos >= bytes.len() {
                return Ok(());
            }
            if bytes[pos..].starts_with(b"<?") {
                pos = self.processing_instruction(inp, pos, true)?;
            } else if bytes[pos..].starts_with(b"<!--") {
                pos = self.comment(inp, pos, true)?;
            } else {
                return self.fail(inp, pos, "Extra content at the end of the document");
            }
        }
    }

    /// XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'
    fn xml_declaration(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut pos = skip_spaces(bytes, start + 5);

        if !bytes[pos..].starts_with(b"version") {
            return self.fail(inp, pos, "Malformed declaration expecting version");
        }
        let (value_start, value_end, next) = self.pseudo_attribute_value(inp, pos + 7)?;
        let version = &bytes[value_start..value_end];
        let is_version_number = version.len() >= 3
            && version[0].is_ascii_digit()
            && version[1] == b'.'
            && version[2..].iter().all(|byte| byte.is_ascii_digit());
        if !is_version_number || version[0] != b'1' {
            return self.fail(inp, value_start, format!("Unsupported version '{}'", lossy(version)));
        }
        self.version_1_0 = version == b"1.0";
        pos = next;

        let mut after_spaces = skip_spaces(bytes, pos);
        if bytes[after_spaces..].starts_with(b"encoding") {
            if after_spaces == pos {
                return self.fail(inp, pos, "Blank needed here");
            }
            let (value_start, value_end, next) = self.pseudo_attribute_value(inp, after_spaces + 8)?;
            // EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*
            // The name isn't used: the caller has decoded the document already.
            let name = &bytes[value_start..value_end];
            let is_encoding_name = name.first().is_some_and(|byte| byte.is_ascii_alphabetic())
                && name
                    .iter()
                    .all(|&byte| byte.is_ascii_alphanumeric() || matches!(byte, b'.' | b'_' | b'-'));
            if !is_encoding_name {
                return self.fail(inp, value_start, "Invalid encoding name");
            }
            pos = next;
            after_spaces = skip_spaces(bytes, pos);
        }

        if bytes[after_spaces..].starts_with(b"standalone") {
            if after_spaces == pos {
                return self.fail(inp, pos, "Blank needed here");
            }
            let (value_start, value_end, next) = self.pseudo_attribute_value(inp, after_spaces + 10)?;
            match &bytes[value_start..value_end] {
                b"yes" => self.standalone = true,
                b"no" => self.standalone = false,
                _ => return self.fail(inp, value_start, "standalone accepts only 'yes' or 'no'"),
            }
            pos = next;
            after_spaces = skip_spaces(bytes, pos);
        }

        if !bytes[after_spaces..].starts_with(b"?>") {
            return self.fail(inp, after_spaces, "parsing XML declaration: '?>' expected");
        }
        Ok(after_spaces + 2)
    }

    /// Parses `S? '=' S? ('"' value '"' | "'" value "'")` and returns where the value starts and ends and where the
    /// closing quote ends.
    fn pseudo_attribute_value(&mut self, inp: In, pos: usize) -> R<(usize, usize, usize)> {
        let bytes = inp.bytes;
        let mut pos = skip_spaces(bytes, pos);
        if at(bytes, pos) != b'=' {
            return self.fail(inp, pos, "expected '='");
        }
        pos = skip_spaces(bytes, pos + 1);
        let quote = at(bytes, pos);
        if quote != b'"' && quote != b'\'' {
            return self.fail(inp, pos, "String not started expecting ' or \"");
        }
        let start = pos + 1;
        match bytes[start..].iter().position(|&byte| byte == quote) {
            Some(length) => Ok((start, start + length, start + length + 1)),
            None => self.fail(inp, pos, "String not closed expecting \" or '"),
        }
    }

    // ------------------------------------------------------------------------------------------------------------
    // Content
    // ------------------------------------------------------------------------------------------------------------

    /// content ::= CharData? ((element | Reference | CDSect | PI | Comment) CharData?)*
    ///
    /// Parses content until the element stack is back down to `floor`. For the document that is the end tag of the
    /// root element. For an entity's text it has to be the end of the text: an entity can't close an element it
    /// didn't open, or leave one open.
    pub(crate) fn content(&mut self, inp: In, mut pos: usize, floor: usize) -> R<usize> {
        let bytes = inp.bytes;
        loop {
            let start = pos;
            let byte = loop {
                pos = scan::find(bytes, pos, b'<', b'&', b']', Controls::StopAtCr);
                if pos >= bytes.len() {
                    break 0;
                }
                let byte = bytes[pos];
                if byte == b']' {
                    if at(bytes, pos + 1) == b']' && at(bytes, pos + 2) == b'>' {
                        self.text(inp, start, pos);
                        return self.fail(inp, pos, "Sequence ']]>' not allowed in content");
                    }
                    pos += 1;
                    continue;
                }
                if byte >= 0x80 {
                    // OPTIMIZATION: Text in a script that isn't Latin is one such character after another, so stay
                    // here for the whole run of them.
                    while pos < bytes.len() && bytes[pos] >= 0x80 {
                        match self.check_non_ascii(inp, pos) {
                            Ok(length) => pos += length,
                            Err(stop) => {
                                // The text that leads up to the bad character is still text of the document.
                                self.text(inp, start, pos);
                                if let Some(error) = self.errors.last_mut() {
                                    error.row = self.tape.len() as u32;
                                }
                                return Err(stop);
                            }
                        }
                    }
                    continue;
                }
                break byte;
            };
            self.text(inp, start, pos);

            match byte {
                b'<' => match at(bytes, pos + 1) {
                    b'/' => {
                        pos = self.end_tag(inp, pos, floor)?;
                        if inp.main && self.stack.len() == floor {
                            return Ok(pos);
                        }
                    }
                    b'!' => {
                        if bytes[pos..].starts_with(b"<!--") {
                            pos = self.comment(inp, pos, true)?;
                        } else if bytes[pos..].starts_with(b"<![CDATA[") {
                            pos = self.cdata_section(inp, pos)?;
                        } else {
                            return self.fail(inp, pos, "StartTag: invalid element name");
                        }
                    }
                    b'?' => pos = self.processing_instruction(inp, pos, true)?,
                    _ => pos = self.start_tag(inp, pos)?,
                },
                b'&' => pos = self.reference(inp, pos)?,
                b'\r' => {
                    // A line end is a single LF, however it was written.
                    if at(bytes, pos + 1) != b'\n' {
                        let newline = self.constant(CONSTANT_NEWLINE);
                        self.tape.push(kind::TEXT, newline, 1);
                    }
                    pos += 1;
                }
                _ => {
                    if pos < bytes.len() {
                        return self.fail(inp, pos, format!("PCDATA invalid Char value {byte}"));
                    }
                    if self.stack.len() > floor {
                        let open = self.stack[self.stack.len() - 1];
                        let message = format!(
                            "Premature end of data in tag {} line {}",
                            self.name_string(open.name),
                            self.line_of(open.offset as usize)
                        );
                        return self.fail(inp, pos, message);
                    }
                    return Ok(pos);
                }
            }
        }
    }

    #[inline(always)]
    fn text(&mut self, inp: In, start: usize, end: usize) {
        if end > start {
            let (offset, length) = self.string(inp, start, end);
            self.tape.push(kind::TEXT, offset, length);
        }
    }

    fn line_of(&self, offset: usize) -> usize {
        self.src[..offset.min(self.src.len())]
            .iter()
            .filter(|&&byte| byte == b'\n')
            .count()
            + 1
    }

    // ------------------------------------------------------------------------------------------------------------
    // Tags
    // ------------------------------------------------------------------------------------------------------------

    /// STag ::= '<' QName (S Attribute)* S? '>'
    /// EmptyElemTag ::= '<' QName (S Attribute)* S? '/>'
    fn start_tag(&mut self, inp: In, pos: usize) -> R<usize> {
        let row = self.tape.len();
        self.tag_row = row as u32;
        let result = self.start_tag_inner(inp, pos, row);
        self.tag_row = NONE;
        if result.is_err() {
            self.tape.truncate(row);
        }
        result
    }

    #[inline(always)]
    fn start_tag_inner(&mut self, inp: In, start: usize, row: usize) -> R<usize> {
        let bytes = inp.bytes;
        let name_start = start + 1;
        let name_end = scan_name(bytes, name_start);
        if name_end == name_start {
            return self.fail(inp, start, "StartTag: invalid element name");
        }
        let name = self.intern_name(inp, name_start, name_end);
        let element_flags = self.names.analyze(name, Storage::new(self.src, &self.arena));
        self.tape.push(kind::START, name, NONE);
        self.stamp += 1;
        let stamp = self.stamp;
        let has_tokenized = element_flags & flag::HAS_TOKENIZED != 0;

        let mut flags = element_flags;
        let mut pos = name_end;
        let mut first = true;
        let empty;
        loop {
            let after_spaces = skip_spaces(bytes, pos);
            let byte = at(bytes, after_spaces);
            if byte == b'>' {
                pos = after_spaces + 1;
                empty = false;
                break;
            }
            if byte == b'/' && at(bytes, after_spaces + 1) == b'>' {
                pos = after_spaces + 2;
                empty = true;
                break;
            }
            if after_spaces >= bytes.len() {
                let message = format!("Couldn't find end of Start Tag {}", self.name_string(name));
                return self.fail(inp, after_spaces, message);
            }
            if after_spaces == pos && !first {
                return self.fail(inp, pos, "attributes construct error");
            }

            let attribute_end = scan_name(bytes, after_spaces);
            if attribute_end == after_spaces {
                return self.fail(inp, after_spaces, "error parsing attribute name");
            }
            let attribute = self.intern_name(inp, after_spaces, attribute_end);
            let attribute_flags = self.names.analyze(attribute, Storage::new(self.src, &self.arena));
            let entry = self.names.get_mut(attribute);
            if entry.stamp == stamp {
                let message = format!("Attribute {} redefined", self.name_string(attribute));
                return self.fail(inp, after_spaces, message);
            }
            entry.stamp = stamp;
            flags |= attribute_flags & !flag::ELEMENT_ONLY;

            let mut value_start = skip_spaces(bytes, attribute_end);
            if at(bytes, value_start) != b'=' {
                let message = format!(
                    "Specification mandates value for attribute {}",
                    self.name_string(attribute)
                );
                return self.fail(inp, value_start, message);
            }
            value_start = skip_spaces(bytes, value_start + 1);
            let quote = at(bytes, value_start);
            if quote != b'"' && quote != b'\'' {
                return self.fail(inp, value_start, "AttValue: \" or ' expected");
            }
            let collapse = has_tokenized && self.attribute_is_tokenized(name, attribute);
            let (offset, length, next) = self.attribute_value(inp, value_start, collapse)?;
            self.tape.push(kind::ATTR, attribute, NONE);
            self.tape.push(kind::VALUE, offset, length);
            pos = next;
            first = false;
        }

        // OPTIMIZATION: Most tags declare no namespace and have no prefix anywhere, and then all there is to do is
        // give the element the default namespace.
        let bindings = if flags & flag::NEEDS_NAMESPACE_PROCESSING != 0 {
            self.process_namespaces(inp, start, row, name, element_flags, flags)?
        } else {
            self.tape.set_b(row, self.default_namespace);
            0
        };

        if self.stack.len() >= MAX_ELEMENT_DEPTH {
            return self.fail(inp, start, "Excessive node nesting.");
        }
        if empty {
            self.tape.push(kind::END, name, NONE);
            self.pop_bindings(bindings);
        } else {
            self.stack.push(Open {
                name,
                bindings,
                offset: start.min(u32::MAX as usize) as u32,
            });
        }
        Ok(pos)
    }

    /// ETag ::= '</' QName S? '>'
    #[inline(always)]
    fn end_tag(&mut self, inp: In, start: usize, floor: usize) -> R<usize> {
        let bytes = inp.bytes;
        if self.stack.len() <= floor {
            return self.fail(inp, start, "chunk is not well balanced");
        }
        let open = self.stack[self.stack.len() - 1];
        // OPTIMIZATION: The name in an end tag has to be the name of the open element, so it isn't scanned, hashed
        // and looked up. It is compared to the bytes it has to be.
        let name_start = start + 2;
        if !self
            .names
            .is_at(open.name, bytes, name_start, Storage::new(self.src, &self.arena))
        {
            return self.end_tag_mismatch(inp, start, open);
        }
        let mut pos = name_start + self.names.get(open.name).length as usize;
        let byte = at(bytes, pos);
        if byte != b'>' {
            if !chars::is_space(byte) {
                return self.end_tag_mismatch(inp, start, open);
            }
            pos = skip_spaces(bytes, pos);
            if at(bytes, pos) != b'>' {
                return self.fail(inp, pos, "expected '>'");
            }
        }
        self.tape.push(kind::END, open.name, NONE);
        self.stack.pop();
        if open.bindings != 0 {
            self.pop_bindings(open.bindings);
        }
        Ok(pos + 1)
    }

    #[cold]
    #[inline(never)]
    fn end_tag_mismatch(&mut self, inp: In, start: usize, open: Open) -> R<usize> {
        let found_end = scan_nmtoken(inp.bytes, start + 2);
        let message = format!(
            "Opening and ending tag mismatch: {} line {} and {}",
            self.name_string(open.name),
            self.line_of(open.offset as usize),
            lossy(&inp.bytes[start + 2..found_end])
        );
        self.fail(inp, start, message)
    }

    // ------------------------------------------------------------------------------------------------------------
    // Attribute values
    // ------------------------------------------------------------------------------------------------------------

    /// AttValue ::= '"' ([^<&"] | Reference)* '"' | "'" ([^<&'] | Reference)* "'"
    ///
    /// `pos` is at the opening quote. Returns the string operands of the normalized value and where the closing
    /// quote ends. `collapse` is set for an attribute the DTD declares with a type other than CDATA: such a value
    /// also has its leading and trailing spaces removed and its runs of spaces made single spaces.
    #[inline(always)]
    pub(crate) fn attribute_value(&mut self, inp: In, pos: usize, collapse: bool) -> R<(u32, u32, usize)> {
        let bytes = inp.bytes;
        let quote = bytes[pos];
        let start = pos + 1;
        let end = scan::find(bytes, start, quote, b'&', b'<', Controls::StopAtAll);
        // OPTIMIZATION: A value with no reference, no white space other than spaces and nothing but ASCII is its own
        // normalized form, and that is nearly every value. It stays where it is in the source.
        if end < bytes.len() && bytes[end] == quote && !collapse {
            let (offset, length) = self.string(inp, start, end);
            return Ok((offset, length, end + 1));
        }
        self.attribute_value_slow(inp, start, end, quote, collapse)
    }

    #[inline(never)]
    fn attribute_value_slow(
        &mut self,
        inp: In,
        start: usize,
        mut pos: usize,
        quote: u8,
        collapse: bool,
    ) -> R<(u32, u32, usize)> {
        let bytes = inp.bytes;
        let mut buffer = std::mem::take(&mut self.scratch);
        buffer.clear();
        let mut run = start;
        loop {
            buffer.extend_from_slice(&bytes[run..pos]);
            if pos >= bytes.len() {
                return self.fail(inp, pos, "AttValue: ' expected");
            }
            let byte = bytes[pos];
            if byte == quote {
                break;
            }
            match byte {
                b'&' => pos = self.attribute_reference(inp, pos, &mut buffer)?,
                b'<' => return self.fail(inp, pos, "Unescaped '<' not allowed in attributes values"),
                b'\t' | b'\n' => {
                    buffer.push(b' ');
                    pos += 1;
                }
                b'\r' => {
                    buffer.push(b' ');
                    pos += 1;
                    if at(bytes, pos) == b'\n' {
                        pos += 1;
                    }
                }
                _ if byte >= 0x80 => {
                    let length = self.check_non_ascii(inp, pos)?;
                    buffer.extend_from_slice(&bytes[pos..pos + length]);
                    pos += length;
                }
                _ => return self.fail(inp, pos, format!("invalid character in attribute value: {byte}")),
            }
            run = pos;
            pos = scan::find(bytes, pos, quote, b'&', b'<', Controls::StopAtAll);
        }
        if collapse {
            collapse_spaces(&mut buffer);
        }
        let (offset, length) = self.arena_push(&buffer);
        self.scratch = buffer;
        Ok((offset, length, pos + 1))
    }

    /// Handles the reference at `bytes[pos]` in an attribute value, appending what it stands for to `buffer`.
    fn attribute_reference(&mut self, inp: In, pos: usize, buffer: &mut Vec<u8>) -> R<usize> {
        let bytes = inp.bytes;
        if at(bytes, pos + 1) == b'#' {
            let (c, next) = self.character_reference(inp, pos)?;
            chars::encode_utf8(c, buffer);
            return Ok(next);
        }
        let (name_start, name_end) = self.entity_reference_name(inp, pos)?;
        let name = &bytes[name_start..name_end];
        let next = name_end + 1;
        if let Some(constant) = predefined_entity(name) {
            buffer.push(ARENA_CONSTANTS[constant as usize]);
            return Ok(next);
        }
        self.expand_entity_in_attribute(inp, pos, name, buffer)?;
        Ok(next)
    }

    fn expand_entity_in_attribute(&mut self, inp: In, pos: usize, name: &[u8], buffer: &mut Vec<u8>) -> R<()> {
        let Some((id, kind, text)) = self.find_general_entity(name) else {
            return self.undeclared_entity(inp, pos, name, Some(buffer));
        };
        match kind {
            EntityKind::Internal => {}
            EntityKind::External => {
                let message = format!("Attribute references external entity '{}'", lossy(name));
                return self.fail(inp, pos, message);
            }
            EntityKind::Unparsed => {
                let message = format!("Entity reference to unparsed entity {}", lossy(name));
                return self.fail(inp, pos, message);
            }
        }
        self.enter_entity(inp, pos, id, text.len())?;
        let saved_offset = self.entity_reference_offset;
        if inp.main {
            self.entity_reference_offset = pos;
        }
        let result = self.entity_text_in_attribute(&text, name, buffer);
        self.entity_reference_offset = saved_offset;
        self.leave_entity(id);
        result
    }

    /// Appends the replacement text of an entity to an attribute value: white space becomes spaces, references are
    /// followed, and a '<' that isn't from a character reference is an error.
    fn entity_text_in_attribute(&mut self, text: &[u8], entity_name: &[u8], buffer: &mut Vec<u8>) -> R<()> {
        let inp = In {
            bytes: text,
            main: false,
        };
        let mut pos = 0;
        while pos < text.len() {
            let byte = text[pos];
            match byte {
                b'&' => pos = self.attribute_reference(inp, pos, buffer)?,
                b'<' => {
                    let message = format!(
                        "'<' in entity '{}' is not allowed in attributes values",
                        lossy(entity_name)
                    );
                    return self.fail(inp, pos, message);
                }
                b'\t' | b'\n' | b'\r' | b' ' => {
                    buffer.push(b' ');
                    pos += 1;
                }
                _ => {
                    buffer.push(byte);
                    pos += 1;
                }
            }
        }
        Ok(())
    }

    // ------------------------------------------------------------------------------------------------------------
    // References
    // ------------------------------------------------------------------------------------------------------------

    /// CharRef ::= '&#' [0-9]+ ';' | '&#x' [0-9a-fA-F]+ ';'
    pub(crate) fn character_reference(&mut self, inp: In, start: usize) -> R<(u32, usize)> {
        let bytes = inp.bytes;
        let mut pos = start + 2;
        let mut value: u32 = 0;
        let digits_start;
        if at(bytes, pos) == b'x' {
            pos += 1;
            digits_start = pos;
            while pos < bytes.len() && CLASS[bytes[pos] as usize] & chars::HEX != 0 {
                let digit = (bytes[pos] as char).to_digit(16).unwrap_or(0);
                value = (value * 16 + digit).min(0x110000);
                pos += 1;
            }
            if pos == digits_start || at(bytes, pos) != b';' {
                return self.fail(inp, start, "xmlParseCharRef: invalid hexadecimal value");
            }
        } else {
            digits_start = pos;
            while pos < bytes.len() && bytes[pos].is_ascii_digit() {
                value = (value * 10 + (bytes[pos] - b'0') as u32).min(0x110000);
                pos += 1;
            }
            if pos == digits_start || at(bytes, pos) != b';' {
                return self.fail(inp, start, "xmlParseCharRef: invalid decimal value");
            }
        }
        if !chars::is_char(value) {
            return self.fail(inp, start, format!("xmlParseCharRef: invalid xmlChar value {value}"));
        }
        Ok((value, pos + 1))
    }

    /// EntityRef ::= '&' Name ';'
    ///
    /// Returns where the name starts and ends.
    pub(crate) fn entity_reference_name(&mut self, inp: In, start: usize) -> R<(usize, usize)> {
        let name_start = start + 1;
        let name_end = scan_name(inp.bytes, name_start);
        if name_end == name_start {
            return self.fail(inp, start, "xmlParseEntityRef: no name");
        }
        if at(inp.bytes, name_end) != b';' {
            return self.fail(inp, name_end, "EntityRef: expecting ';'");
        }
        Ok((name_start, name_end))
    }

    fn find_general_entity(&self, name: &[u8]) -> Option<(u32, EntityKind, Rc<[u8]>)> {
        let dtd = self.dtd.as_ref()?;
        let id = self.names.find(name, self.storage())?;
        let entity = dtd.general.get(&id)?;
        Some((id, entity.kind, entity.text.clone()))
    }

    /// Checks that expanding the entity `id` here is allowed, and marks it as being expanded.
    fn enter_entity(&mut self, inp: In, pos: usize, id: u32, length: usize) -> R<()> {
        let expanding = self
            .dtd
            .as_ref()
            .and_then(|dtd| dtd.general.get(&id))
            .is_some_and(|entity| entity.expanding);
        if expanding {
            return self.fail(inp, pos, "Detected an entity reference loop");
        }
        if self.entity_depth >= MAX_ENTITY_DEPTH {
            return self.fail(inp, pos, "Maximum entity nesting depth exceeded");
        }
        self.account_for_expansion(inp, pos, length)?;
        if let Some(entity) = self.dtd.as_mut().and_then(|dtd| dtd.general.get_mut(&id)) {
            entity.expanding = true;
        }
        self.entity_depth += 1;
        Ok(())
    }

    fn leave_entity(&mut self, id: u32) {
        self.entity_depth -= 1;
        if let Some(entity) = self.dtd.as_mut().and_then(|dtd| dtd.general.get_mut(&id)) {
            entity.expanding = false;
        }
    }

    pub(crate) fn account_for_expansion(&mut self, inp: In, pos: usize, length: usize) -> R<()> {
        self.expanded = self
            .expanded
            .saturating_add(length as u64)
            .saturating_add(ENTITY_FIXED_COST);
        if self.expanded > ALLOWED_EXPANSION && self.expanded / MAX_AMPLIFICATION > self.src.len() as u64 {
            return self.fail(inp, pos, "Maximum entity amplification factor exceeded");
        }
        Ok(())
    }

    /// Handles a reference to an entity that has no declaration. With `buffer` the reference is in an attribute
    /// value, without it in content.
    fn undeclared_entity(&mut self, inp: In, pos: usize, name: &[u8], buffer: Option<&mut Vec<u8>>) -> R<()> {
        if self.is_xhtml
            && let Some(resolve) = self.options.resolve_html_entity
        {
            let mut text = Vec::new();
            if resolve(name, &mut text) {
                match buffer {
                    Some(buffer) => buffer.extend_from_slice(&text),
                    None => {
                        let (offset, length) = self.arena_push(&text);
                        self.tape.push(kind::TEXT, offset, length);
                    }
                }
                return Ok(());
            }
        }
        let message = format!("Entity '{}' not defined", lossy(name));
        if self.standalone || !(self.has_external_subset || self.has_parameter_entity_references) {
            return self.fail(inp, pos, message);
        }
        // A document that points at declarations this parser doesn't read may well declare the entity there, so the
        // document is still well-formed and parsing goes on, with the reference standing for nothing. But nothing is
        // known of what it should stand for, so the document can't be shown as its author meant it, and whoever
        // asked for it is told.
        self.report(inp, pos, false, message);
        Ok(())
    }

    /// Reference ::= EntityRef | CharRef
    #[inline(never)]
    fn reference(&mut self, inp: In, pos: usize) -> R<usize> {
        let bytes = inp.bytes;
        if at(bytes, pos + 1) == b'#' {
            let (c, next) = self.character_reference(inp, pos)?;
            let mut encoded = [0u8; 4];
            let encoded = char::from_u32(c).unwrap_or('\u{FFFD}').encode_utf8(&mut encoded);
            let (offset, length) = self.arena_push(encoded.as_bytes());
            self.tape.push(kind::TEXT, offset, length);
            return Ok(next);
        }
        let (name_start, name_end) = self.entity_reference_name(inp, pos)?;
        let name = &bytes[name_start..name_end];
        let next = name_end + 1;
        if let Some(constant) = predefined_entity(name) {
            let offset = self.constant(constant);
            self.tape.push(kind::TEXT, offset, 1);
            return Ok(next);
        }

        let Some((id, kind, text)) = self.find_general_entity(name) else {
            self.undeclared_entity(inp, pos, name, None)?;
            return Ok(next);
        };
        match kind {
            EntityKind::Internal => {}
            // External entities aren't read. The reference stands for nothing.
            EntityKind::External => return Ok(next),
            EntityKind::Unparsed => {
                let message = format!("Entity reference to unparsed entity {}", lossy(name));
                return self.fail(inp, pos, message);
            }
        }

        self.enter_entity(inp, pos, id, text.len())?;
        let saved_offset = self.entity_reference_offset;
        if inp.main {
            self.entity_reference_offset = pos;
        }
        let floor = self.stack.len();
        let result = self.content(
            In {
                bytes: &text,
                main: false,
            },
            0,
            floor,
        );
        self.entity_reference_offset = saved_offset;
        self.leave_entity(id);
        result?;
        if self.arena_overflow {
            return self.fail(inp, pos, "Document too large");
        }
        Ok(next)
    }

    // ------------------------------------------------------------------------------------------------------------
    // Comments, processing instructions, CDATA sections
    // ------------------------------------------------------------------------------------------------------------

    /// Scans from `start` to the terminator of a comment, a processing instruction or a CDATA section, checking the
    /// characters on the way. Returns the string operands of what is before the terminator and where the terminator
    /// starts.
    fn delimited(&mut self, inp: In, start: usize, what: Delimited) -> R<(u32, u32, usize)> {
        let bytes = inp.bytes;
        let delimiter = match what {
            Delimited::Comment => b'-',
            Delimited::ProcessingInstruction => b'?',
            Delimited::CData => b']',
        };
        let mut pos = start;
        let mut run = start;
        // A CR has to be normalized, which the source can't be edited to do. The text is only copied once there is
        // one.
        let mut buffer = std::mem::take(&mut self.scratch);
        buffer.clear();
        let mut copied = false;
        loop {
            pos = scan::find(bytes, pos, delimiter, delimiter, delimiter, Controls::StopAtCr);
            if pos >= bytes.len() {
                let message = match what {
                    Delimited::Comment => "Comment not terminated",
                    Delimited::ProcessingInstruction => "PI not finished",
                    Delimited::CData => "CData section not finished",
                };
                return self.fail(inp, start, message);
            }
            let byte = bytes[pos];
            if byte == delimiter {
                match what {
                    Delimited::Comment => {
                        if at(bytes, pos + 1) == b'-' {
                            if at(bytes, pos + 2) == b'>' {
                                break;
                            }
                            return self.fail(inp, pos, "Double hyphen within comment");
                        }
                    }
                    Delimited::ProcessingInstruction => {
                        if at(bytes, pos + 1) == b'>' {
                            break;
                        }
                    }
                    Delimited::CData => {
                        if at(bytes, pos + 1) == b']' && at(bytes, pos + 2) == b'>' {
                            break;
                        }
                    }
                }
                pos += 1;
            } else if byte == b'\r' {
                buffer.extend_from_slice(&bytes[run..pos]);
                buffer.push(b'\n');
                copied = true;
                pos += 1;
                if at(bytes, pos) == b'\n' {
                    pos += 1;
                }
                run = pos;
            } else if byte >= 0x80 {
                while pos < bytes.len() && bytes[pos] >= 0x80 {
                    pos += self.check_non_ascii(inp, pos)?;
                }
            } else {
                return self.fail(inp, pos, format!("PCDATA invalid Char value {byte}"));
            }
        }
        let (offset, length) = if copied {
            buffer.extend_from_slice(&bytes[run..pos]);
            self.arena_push(&buffer)
        } else {
            self.string(inp, start, pos)
        };
        self.scratch = buffer;
        Ok((offset, length, pos))
    }

    /// Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
    pub(crate) fn comment(&mut self, inp: In, start: usize, in_content: bool) -> R<usize> {
        let (offset, length, end) = self.delimited(inp, start + 4, Delimited::Comment)?;
        if in_content && self.options.preserve_comments {
            self.tape.push(kind::COMMENT, offset, length);
        }
        Ok(end + 3)
    }

    /// CDSect ::= '<![CDATA[' (Char* - (Char* ']]>' Char*)) ']]>'
    fn cdata_section(&mut self, inp: In, start: usize) -> R<usize> {
        let (offset, length, end) = self.delimited(inp, start + 9, Delimited::CData)?;
        let row_kind = if self.options.preserve_cdata {
            kind::CDATA
        } else {
            kind::TEXT
        };
        self.tape.push(row_kind, offset, length);
        Ok(end + 3)
    }

    /// PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
    ///
    /// A processing instruction in the DTD is checked but gets no row: it isn't part of the document's content.
    pub(crate) fn processing_instruction(&mut self, inp: In, start: usize, in_content: bool) -> R<usize> {
        let bytes = inp.bytes;
        let target_start = start + 2;
        let target_end = scan_name(bytes, target_start);
        if target_end == target_start {
            return self.fail(inp, start, "xmlParsePI : no target name");
        }
        let target = &bytes[target_start..target_end];
        if target == b"xml" {
            return self.fail(inp, start, "XML declaration allowed only at the start of the document");
        }
        if target.eq_ignore_ascii_case(b"xml") {
            return self.fail(inp, start, "XML reserved name");
        }
        if target.contains(&b':') {
            let message = format!("colons are forbidden from PI names '{}'", lossy(target));
            self.namespace_error(inp, start, message);
        }

        let (offset, length, end);
        if at(bytes, target_end) == b'?' && at(bytes, target_end + 1) == b'>' {
            (offset, length) = (self.src.len() as u32, 0);
            end = target_end;
        } else {
            if !chars::is_space(at(bytes, target_end)) {
                let message = format!("ParsePI: PI {} space expected", lossy(target));
                return self.fail(inp, target_end, message);
            }
            let data_start = skip_spaces(bytes, target_end);
            (offset, length, end) = self.delimited(inp, data_start, Delimited::ProcessingInstruction)?;
        }
        if in_content {
            let name = self.intern_name(inp, target_start, target_end);
            self.tape.push(kind::PI, name, NONE);
            self.tape.push(kind::VALUE, offset, length);
        }
        Ok(end + 2)
    }

    // ------------------------------------------------------------------------------------------------------------
    // Namespaces
    // ------------------------------------------------------------------------------------------------------------

    fn push_binding(&mut self, prefix: u32, namespace: u32, depth: u32) -> bool {
        let previous = self.names.get(prefix).binding;
        if previous != NONE && self.bindings[previous as usize].depth == depth {
            return false;
        }
        self.names.get_mut(prefix).binding = self.bindings.len() as u32;
        self.bindings.push(Binding {
            prefix,
            namespace,
            previous,
            depth,
        });
        if prefix == self.xmlns_name {
            self.default_namespace = namespace;
        }
        true
    }

    fn pop_bindings(&mut self, count: u32) {
        for _ in 0..count {
            let Some(binding) = self.bindings.pop() else {
                return;
            };
            self.names.get_mut(binding.prefix).binding = binding.previous;
            if binding.prefix == self.xmlns_name {
                self.default_namespace = if binding.previous == NONE {
                    NONE
                } else {
                    self.bindings[binding.previous as usize].namespace
                };
            }
        }
    }

    /// The namespace `prefix` is bound to, or None.
    fn namespace_of_prefix(&self, prefix: u32) -> Option<u32> {
        let binding = self.names.get(prefix).binding;
        if binding == NONE {
            None
        } else {
            Some(self.bindings[binding as usize].namespace)
        }
    }

    fn attribute_is_tokenized(&self, element: u32, attribute: u32) -> bool {
        self.dtd
            .as_ref()
            .is_some_and(|dtd| dtd.defined_attributes.get(&(element, attribute)) == Some(&true))
    }

    /// Does everything about a start tag that has to do with namespaces, and with the attributes the DTD gives the
    /// element by default: adds the tag's namespace declarations to the bindings in scope, appends the defaulted
    /// attributes, resolves the prefixes of the element and its attributes, and moves the declarations ahead of the
    /// other attributes. Returns how many bindings were added.
    #[inline(never)]
    fn process_namespaces(
        &mut self,
        inp: In,
        pos: usize,
        row: usize,
        element: u32,
        element_flags: u8,
        tag_flags: u8,
    ) -> R<u32> {
        let depth = self.stack.len() as u32 + 1;
        let stamp = self.stamp;
        let mut bindings = 0;
        let mut reorder = false;
        // Without a declaration in the tag, the attributes in it are all plain ones.
        let mut seen_plain_attribute =
            tag_flags & (flag::IS_XMLNS | flag::PREFIX_XMLNS) == 0 && self.tape.len() > row + 1;

        // The declarations in the tag.
        // OPTIMIZATION: A tag is far more often here for a prefix it uses than for one it declares.
        let mut index = row + 1;
        let explicit_end = if tag_flags & (flag::IS_XMLNS | flag::PREFIX_XMLNS) != 0 {
            self.tape.len()
        } else {
            0
        };
        while index < explicit_end {
            let (_, attribute, _) = self.tape.row(index);
            let attribute_flags = self.names.get(attribute).flags;
            if attribute_flags & (flag::IS_XMLNS | flag::PREFIX_XMLNS) != 0 {
                let (_, offset, length) = self.tape.row(index + 1);
                let prefix = if attribute_flags & flag::IS_XMLNS != 0 {
                    None
                } else {
                    Some(self.names.get(attribute).local)
                };
                let value = Storage::new(self.src, &self.arena).get(offset, length);
                let redeclares_xml = value == XML_NAMESPACE
                    && prefix
                        .is_some_and(|prefix| self.names.bytes(prefix, Storage::new(self.src, &self.arena)) == b"xml");
                if redeclares_xml {
                    // The xml prefix is bound from the start, so declaring it as what it already is binds nothing, but
                    // the declaration is an attribute of the element like any other.
                    self.tape.set_row(
                        index,
                        kind::ATTR | kind::NAMESPACE_DECLARATION,
                        attribute,
                        self.xmlns_namespace,
                    );
                    reorder |= seen_plain_attribute;
                } else if self.declare_namespace(inp, pos, prefix, offset, length, depth) {
                    self.tape.set_row(
                        index,
                        kind::ATTR | kind::NAMESPACE_DECLARATION,
                        attribute,
                        self.xmlns_namespace,
                    );
                    bindings += 1;
                    reorder |= seen_plain_attribute;
                } else {
                    // A declaration that breaks a rule declares nothing and isn't an attribute either.
                    self.tape.set_kind(index, 0);
                    reorder = true;
                }
            } else {
                seen_plain_attribute = true;
            }
            index += 2;
        }

        // The attributes the DTD defaults.
        if element_flags & flag::HAS_DEFAULTS != 0 {
            let definitions = self.dtd.as_ref().and_then(|dtd| dtd.attlists.get(&element)).cloned();
            for definition in definitions.iter().flat_map(|definitions| definitions.iter()) {
                let (offset, length) = definition.default;
                let entry = self.names.get_mut(definition.name);
                if entry.stamp == stamp {
                    continue;
                }
                entry.stamp = stamp;
                // A default is a name and a value the document didn't spell out here, as an entity's replacement
                // text is. A few lines of declarations can give every element thousands of attributes, so both draw
                // on the same budget.
                let name_length = self
                    .names
                    .bytes(definition.name, Storage::new(self.src, &self.arena))
                    .len();
                self.account_for_expansion(inp, pos, name_length + length as usize)?;
                let attribute_flags = self.names.analyze(definition.name, Storage::new(self.src, &self.arena));
                let name = self.names.bytes(definition.name, Storage::new(self.src, &self.arena));

                // The name in an attribute-list declaration is a Name, not a QName, so it is split at its first
                // colon whatever comes after, and "xmlns:" followed by anything declares a prefix.
                let declared_prefix: Option<Option<&[u8]>> = if name == b"xmlns" {
                    Some(None)
                } else if name.len() > 6 && name.starts_with(b"xmlns:") {
                    Some(Some(&name[6..]))
                } else {
                    None
                };
                if let Some(prefix) = declared_prefix {
                    let value = Storage::new(self.src, &self.arena).get(offset, length);
                    if prefix == Some(b"xml") {
                        // As in a start tag: declaring the xml prefix as what it already is binds nothing and is an
                        // attribute all the same.
                        if value == XML_NAMESPACE {
                            reorder |= seen_plain_attribute;
                            self.tape.push(
                                kind::ATTR | kind::NAMESPACE_DECLARATION | kind::DEFAULTED,
                                definition.name,
                                self.xmlns_namespace,
                            );
                            self.tape.push(kind::VALUE, offset, length);
                        } else {
                            self.namespace_error(inp, pos, "xml namespace prefix mapped to wrong URI".into());
                        }
                        continue;
                    }
                    if let Some(prefix) = prefix
                        && !chars::is_name(prefix, false)
                    {
                        let message = format!("Namespace prefix '{}' is not an NCName", lossy(prefix));
                        return self.fail(inp, pos, message);
                    }
                    if let Some(message) = reserved_binding_error(prefix, value) {
                        return self.fail(inp, pos, message);
                    }
                    let prefix_id = match prefix {
                        Some(_) => self.names.get(definition.name).local,
                        None => self.xmlns_name,
                    };
                    let namespace = if length == 0 {
                        NONE
                    } else {
                        self.names.intern(value, offset, Storage::new(self.src, &self.arena))
                    };
                    if !self.push_binding(prefix_id, namespace, depth) {
                        continue;
                    }
                    bindings += 1;
                    reorder |= seen_plain_attribute;
                    self.tape.push(
                        kind::ATTR | kind::NAMESPACE_DECLARATION | kind::DEFAULTED,
                        definition.name,
                        self.xmlns_namespace,
                    );
                    self.tape.push(kind::VALUE, offset, length);
                    continue;
                }

                if attribute_flags & flag::NOT_A_QNAME != 0 {
                    let message = format!("Attribute name '{}' is not a qualified name", lossy(name));
                    return self.fail(inp, pos, message);
                }
                seen_plain_attribute = true;
                self.tape.push(kind::ATTR | kind::DEFAULTED, definition.name, NONE);
                self.tape.push(kind::VALUE, offset, length);
            }
        }

        // The element's own namespace.
        let namespace = if element_flags & flag::NOT_A_QNAME != 0 {
            let message = format!("Failed to parse QName '{}'", self.name_string(element));
            self.namespace_error(inp, pos, message);
            self.default_namespace
        } else if element_flags & flag::HAS_PREFIX != 0 {
            let prefix = self.names.get(element).prefix;
            if element_flags & flag::PREFIX_XML != 0 {
                self.xml_namespace
            } else if element_flags & flag::PREFIX_XMLNS == 0
                && let Some(namespace) = self.namespace_of_prefix(prefix)
            {
                // The name "xmlns" holds the binding of the default namespace, which is no binding of the xmlns
                // prefix: that prefix can't be declared, so an element can't use it.
                namespace
            } else {
                let message = format!(
                    "Namespace prefix {} on {} is not defined",
                    self.name_string(prefix),
                    self.name_string(self.names.get(element).local)
                );
                self.namespace_error(inp, pos, message);
                NONE
            }
        } else {
            self.default_namespace
        };
        self.tape.set_b(row, namespace);

        // The namespaces of the attributes. Two attributes with different prefixes are the same attribute if the
        // prefixes are bound to the same namespace.
        let mut prefixed = std::mem::take(&mut self.prefixed_scratch);
        prefixed.clear();
        let mut index = row + 1;
        while index < self.tape.len() {
            let (row_kind, attribute, _) = self.tape.row(index);
            if row_kind & kind::MASK == kind::ATTR && row_kind & kind::NAMESPACE_DECLARATION == 0 {
                let entry = *self.names.get(attribute);
                if entry.flags & flag::NOT_A_QNAME != 0 {
                    let message = format!("Failed to parse QName '{}'", self.name_string(attribute));
                    self.namespace_error(inp, pos, message);
                } else if entry.flags & flag::HAS_PREFIX != 0 {
                    let namespace = if entry.flags & flag::PREFIX_XML != 0 {
                        Some(self.xml_namespace)
                    } else {
                        self.namespace_of_prefix(entry.prefix)
                    };
                    match namespace {
                        Some(namespace) if namespace != NONE => {
                            self.tape.set_b(index, namespace);
                            prefixed.push((entry.local, namespace));
                        }
                        _ => {
                            let message = format!(
                                "Namespace prefix {} for {} on {} is not defined",
                                self.name_string(entry.prefix),
                                self.name_string(entry.local),
                                self.name_string(element)
                            );
                            self.namespace_error(inp, pos, message);
                        }
                    }
                }
            }
            index += 2;
        }
        if prefixed.len() > 1 {
            self.check_for_repeated_namespaced_attributes(inp, pos, &prefixed);
        }
        self.prefixed_scratch = prefixed;

        if reorder {
            let mut rows = std::mem::take(&mut self.rows_scratch);
            rows.clear();
            for index in row + 1..self.tape.len() {
                rows.push(self.tape.row(index));
            }
            self.tape.truncate(row + 1);
            for declarations in [true, false] {
                for pair in rows.as_chunks::<2>().0 {
                    let (row_kind, a, b) = pair[0];
                    if row_kind == 0 || (row_kind & kind::NAMESPACE_DECLARATION != 0) != declarations {
                        continue;
                    }
                    self.tape.push(row_kind, a, b);
                    self.tape.push(pair[1].0, pair[1].1, pair[1].2);
                }
            }
            self.rows_scratch = rows;
        }

        Ok(bindings)
    }

    #[inline(never)]
    fn check_for_repeated_namespaced_attributes(&mut self, inp: In, pos: usize, prefixed: &[(u32, u32)]) {
        const FEW: usize = 8;
        let mut seen = HashSet::new();
        for (index, &attribute) in prefixed.iter().enumerate() {
            // OPTIMIZATION: A tag with more than a few prefixed attributes is rare enough that building a set for
            // every tag would cost more than it saves.
            let repeated = if prefixed.len() <= FEW {
                prefixed[..index].contains(&attribute)
            } else {
                !seen.insert(attribute)
            };
            if repeated {
                let message = format!(
                    "Namespaced Attribute {} in '{}' redefined",
                    self.name_string(attribute.0),
                    self.name_string(attribute.1)
                );
                self.namespace_error(inp, pos, message);
            }
        }
    }

    /// Handles a namespace declaration written in a start tag: `prefix` is None for `xmlns`, and the value is the
    /// string at `offset`. Returns whether it declared anything.
    fn declare_namespace(
        &mut self,
        inp: In,
        pos: usize,
        prefix: Option<u32>,
        offset: u32,
        length: u32,
        depth: u32,
    ) -> bool {
        let value = Storage::new(self.src, &self.arena).get(offset, length);
        let is_xml_namespace = value == XML_NAMESPACE;
        let is_xmlns_namespace = value == XMLNS_NAMESPACE;
        // NB: A namespace name is meant to be a URI reference, but one that isn't is a name all the same: nothing
        //     ever resolves it, and it is only ever compared with other names.
        let namespace = if length == 0 {
            NONE
        } else {
            self.names.intern(value, offset, Storage::new(self.src, &self.arena))
        };

        let Some(prefix) = prefix else {
            if is_xml_namespace {
                self.namespace_error(inp, pos, "xml namespace URI cannot be the default namespace".into());
                return false;
            }
            if is_xmlns_namespace {
                self.namespace_error(inp, pos, "reuse of the xmlns namespace name is forbidden".into());
                return false;
            }
            return self.push_binding(self.xmlns_name, namespace, depth);
        };

        let prefix_bytes = self.names.bytes(prefix, Storage::new(self.src, &self.arena));
        if prefix_bytes == b"xml" {
            // The xml prefix is bound to its namespace from the start, and there is nothing else it can be bound to.
            if !is_xml_namespace {
                self.namespace_error(inp, pos, "xml namespace prefix mapped to wrong URI".into());
            }
            return false;
        }
        if is_xml_namespace {
            self.namespace_error(inp, pos, "xml namespace URI mapped to wrong prefix".into());
            return false;
        }
        if prefix_bytes == b"xmlns" {
            self.namespace_error(inp, pos, "redefinition of the xmlns prefix is forbidden".into());
            return false;
        }
        if is_xmlns_namespace {
            self.namespace_error(inp, pos, "reuse of the xmlns namespace name is forbidden".into());
            return false;
        }
        if length == 0 {
            let message = format!("xmlns:{}: Empty XML namespace is not allowed", self.name_string(prefix));
            self.namespace_error(inp, pos, message);
            return false;
        }
        self.push_binding(prefix, namespace, depth)
    }
}

/// The rules of Namespaces in XML 1.0 §3 for the reserved prefixes and namespace names, and of §6.2 that a prefix
/// can't be bound to an empty namespace name, as they apply to a declaration the DTD defaults.
fn reserved_binding_error(prefix: Option<&[u8]>, value: &[u8]) -> Option<String> {
    let Some(prefix) = prefix else {
        if value == XML_NAMESPACE {
            return Some("The XML namespace must not be declared as the default namespace".into());
        }
        if value == XMLNS_NAMESPACE {
            return Some("The xmlns namespace must not be declared as the default namespace".into());
        }
        return None;
    };
    let prefix = lossy(prefix);
    if prefix == "xmlns" {
        return Some("The xmlns prefix must not be declared".into());
    }
    if value.is_empty() {
        return Some(format!(
            "Namespace prefix '{prefix}' must not be bound to an empty namespace name"
        ));
    }
    if value == XML_NAMESPACE {
        return Some(format!(
            "Namespace prefix '{prefix}' must not be bound to the XML namespace"
        ));
    }
    if value == XMLNS_NAMESPACE {
        return Some(format!(
            "Namespace prefix '{prefix}' must not be bound to the xmlns namespace"
        ));
    }
    None
}

/// Removes leading and trailing spaces and makes every run of spaces one space.
pub(crate) fn collapse_spaces(buffer: &mut Vec<u8>) {
    let mut write = 0;
    let mut pending_space = false;
    for read in 0..buffer.len() {
        let byte = buffer[read];
        if byte == b' ' {
            pending_space = write > 0;
        } else {
            if pending_space {
                buffer[write] = b' ';
                write += 1;
                pending_space = false;
            }
            buffer[write] = byte;
            write += 1;
        }
    }
    buffer.truncate(write);
}
