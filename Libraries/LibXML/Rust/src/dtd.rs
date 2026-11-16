/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The document type declaration and its internal subset.
//!
//! Nothing here validates. The subset is read for the three things that change what a document means to a parser that
//! doesn't validate: the entities it declares, the attributes it defaults, and the attributes it gives a type other
//! than CDATA, whose values are normalized differently. Everything else is only checked for being well-formed. The
//! external subset and external parameter entities are never read.

use std::collections::HashMap;
use std::rc::Rc;

use crate::chars;
use crate::chars::CLASS;
use crate::names::IdMap;
use crate::names::flag;
use crate::parser::In;
use crate::parser::MAX_ENTITY_DEPTH;
use crate::parser::Parser;
use crate::parser::R;
use crate::parser::at;
use crate::parser::lossy;
use crate::parser::scan_name;
use crate::parser::scan_nmtoken;
use crate::parser::skip_spaces;
use crate::tape::NONE;
use crate::tape::kind;

/// How deep the groups of a content model can nest.
const MAX_CONTENT_MODEL_DEPTH: u32 = 256;

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum EntityKind {
    Internal,
    /// An external parsed entity. Its text is never read.
    External,
    /// An external entity with a notation: not XML, and not something a document can include.
    Unparsed,
}

pub(crate) struct Entity {
    pub kind: EntityKind,
    /// The replacement text of an internal entity.
    pub text: Rc<[u8]>,
    /// Set while the entity's text is being parsed, to catch an entity that includes itself.
    pub expanding: bool,
}

#[derive(Clone)]
pub(crate) struct AttributeDefinition {
    pub name: u32,
    /// The string operands of the default value.
    pub default: (u32, u32),
}

#[derive(Default)]
pub(crate) struct Dtd {
    pub general: HashMap<u32, Entity>,
    pub parameter: HashMap<u32, Entity>,
    /// The attribute definitions that have a default value, of each element type that has any, by the element's
    /// name.
    pub attlists: IdMap<Rc<Vec<AttributeDefinition>>>,
    /// Every (element, attribute) that has a definition, with whether its type is a tokenized one. This is what
    /// says which definition of an attribute came first, and it keeps an element with a great many attributes from
    /// costing a walk of a list for each of them.
    pub defined_attributes: HashMap<(u32, u32), bool>,
}

/// The public identifiers of the XHTML family of DTDs. A document with one of them can use the HTML character
/// entities, as if it had read the DTD.
fn is_xhtml_public_id(public_id: &[u8]) -> bool {
    matches!(
        public_id,
        b"-//W3C//DTD XHTML 1.0 Transitional//EN"
            | b"-//W3C//DTD XHTML 1.1//EN"
            | b"-//W3C//DTD XHTML 1.0 Strict//EN"
            | b"-//W3C//DTD XHTML 1.0 Frameset//EN"
            | b"-//W3C//DTD XHTML Basic 1.0//EN"
            | b"-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN"
            | b"-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN"
            | b"-//W3C//DTD MathML 2.0//EN"
            | b"-//WAPFORUM//DTD XHTML Mobile 1.0//EN"
            | b"-//WAPFORUM//DTD XHTML Mobile 1.1//EN"
            | b"-//WAPFORUM//DTD XHTML Mobile 1.2//EN"
    )
}

/// The public and system identifiers of an ExternalID, as ranges of the input.
#[derive(Default)]
struct ExternalId {
    public: Option<(usize, usize)>,
    system: Option<(usize, usize)>,
}

impl Parser<'_, '_> {
    fn dtd_mut(&mut self) -> &mut Dtd {
        self.dtd.get_or_insert_with(Default::default)
    }

    /// Skips white space that has to be there.
    fn required_spaces(&mut self, inp: In, pos: usize, message: &str) -> R<usize> {
        let after = skip_spaces(inp.bytes, pos);
        if after == pos {
            return self.fail(inp, pos, message);
        }
        Ok(after)
    }

    /// doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? ('[' intSubset ']' S?)? '>'
    pub(crate) fn doctype(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut pos = self.required_spaces(inp, start + 9, "Space required after 'DOCTYPE'")?;
        let name_end = scan_name(bytes, pos);
        if name_end == pos {
            return self.fail(inp, pos, "xmlParseDocTypeDecl : no DOCTYPE name !");
        }
        let name = self.intern_name(inp, pos, name_end);
        pos = skip_spaces(bytes, name_end);

        let (external_id, next) = self.external_id(inp, pos, true)?;
        pos = next;
        self.has_external_subset = external_id.public.is_some() || external_id.system.is_some();
        if let Some((public_start, public_end)) = external_id.public {
            self.is_xhtml = is_xhtml_public_id(&bytes[public_start..public_end]);
        }

        self.tape.push(kind::DOCTYPE, name, NONE);
        for id in [external_id.public, external_id.system] {
            match id {
                Some((id_start, id_end)) => {
                    let id = &bytes[id_start..id_end];
                    // A line end is a LF wherever it is, and the source can't be edited to make it one.
                    let (offset, length) = if id.contains(&b'\r') {
                        let mut normalized = Vec::with_capacity(id.len());
                        let mut index = 0;
                        while index < id.len() {
                            if id[index] == b'\r' {
                                normalized.push(b'\n');
                                index += if id.get(index + 1) == Some(&b'\n') { 2 } else { 1 };
                            } else {
                                normalized.push(id[index]);
                                index += 1;
                            }
                        }
                        self.arena_push(&normalized)
                    } else {
                        self.string(inp, id_start, id_end)
                    };
                    self.tape.push(kind::VALUE, offset, length);
                }
                None => self.tape.push(kind::VALUE | kind::ABSENT, self.src.len() as u32, 0),
            }
        }

        pos = skip_spaces(bytes, pos);
        if at(bytes, pos) == b'[' {
            pos = self.subset(inp, pos + 1, true)?;
            pos = skip_spaces(bytes, pos);
        }
        if at(bytes, pos) != b'>' {
            return self.fail(inp, pos, "DOCTYPE improperly terminated");
        }
        Ok(pos + 1)
    }

    /// ExternalID ::= 'SYSTEM' S SystemLiteral | 'PUBLIC' S PubidLiteral S SystemLiteral
    ///
    /// A notation declaration can also have a public identifier alone, which `strict` rules out.
    fn external_id(&mut self, inp: In, pos: usize, strict: bool) -> R<(ExternalId, usize)> {
        let bytes = inp.bytes;
        let mut id = ExternalId::default();
        if bytes[pos.min(bytes.len())..].starts_with(b"SYSTEM") {
            let literal = self.required_spaces(inp, pos + 6, "Space required after 'SYSTEM'")?;
            let (literal_start, literal_end) = self.system_literal(inp, literal)?;
            id.system = Some((literal_start, literal_end));
            return Ok((id, literal_end + 1));
        }
        if bytes[pos.min(bytes.len())..].starts_with(b"PUBLIC") {
            let literal = self.required_spaces(inp, pos + 6, "Space required after 'PUBLIC'")?;
            let (literal_start, literal_end) = self.pubid_literal(inp, literal)?;
            id.public = Some((literal_start, literal_end));
            let after = skip_spaces(bytes, literal_end + 1);
            let quote = at(bytes, after);
            if !strict && quote != b'"' && quote != b'\'' {
                return Ok((id, literal_end + 1));
            }
            if after == literal_end + 1 {
                return self.fail(inp, after, "Space required after the Public Identifier");
            }
            let (literal_start, literal_end) = self.system_literal(inp, after)?;
            id.system = Some((literal_start, literal_end));
            return Ok((id, literal_end + 1));
        }
        Ok((id, pos))
    }

    /// SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'")
    ///
    /// Returns where the literal's text starts and ends, the end being the closing quote.
    fn system_literal(&mut self, inp: In, pos: usize) -> R<(usize, usize)> {
        let bytes = inp.bytes;
        let quote = at(bytes, pos);
        if quote != b'"' && quote != b'\'' {
            return self.fail(inp, pos, "SystemLiteral \" or ' expected");
        }
        let start = pos + 1;
        let mut end = start;
        loop {
            if end >= bytes.len() {
                return self.fail(inp, pos, "Unfinished System or Public ID \" or ' expected");
            }
            let byte = bytes[end];
            if byte == quote {
                break;
            }
            if byte >= 0x80 {
                end += self.check_non_ascii(inp, end)?;
            } else if CLASS[byte as usize] & chars::INVALID != 0 {
                return self.fail(inp, end, "Unfinished System or Public ID \" or ' expected");
            } else {
                end += 1;
            }
        }
        Ok((start, end))
    }

    /// PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"
    fn pubid_literal(&mut self, inp: In, pos: usize) -> R<(usize, usize)> {
        let bytes = inp.bytes;
        let quote = at(bytes, pos);
        if quote != b'"' && quote != b'\'' {
            return self.fail(inp, pos, "PubidLiteral \" or ' expected");
        }
        let start = pos + 1;
        let mut end = start;
        while end < bytes.len() && bytes[end] != quote && CLASS[bytes[end] as usize] & chars::PUBID != 0 {
            end += 1;
        }
        if at(bytes, end) != quote {
            return self.fail(inp, end, "Unfinished PubidLiteral");
        }
        Ok((start, end))
    }

    /// intSubset ::= (markupdecl | PEReference | S)*
    ///
    /// For the subset itself (`top`), parses up to the closing bracket and returns where it ends. For the
    /// replacement text of a parameter entity, parses all of it: a declaration can't start in an entity and end
    /// outside of it.
    fn subset(&mut self, inp: In, mut pos: usize, top: bool) -> R<usize> {
        let bytes = inp.bytes;
        loop {
            pos = skip_spaces(bytes, pos);
            if pos >= bytes.len() {
                if top {
                    return self.fail(inp, pos, "internal subset is not finished");
                }
                return Ok(pos);
            }
            match bytes[pos] {
                b']' if top => return Ok(pos + 1),
                b'%' => pos = self.parameter_entity_reference(inp, pos)?,
                b'<' => {
                    let rest = &bytes[pos..];
                    pos = if rest.starts_with(b"<!ELEMENT") {
                        self.element_declaration(inp, pos)?
                    } else if rest.starts_with(b"<!ATTLIST") {
                        self.attlist_declaration(inp, pos)?
                    } else if rest.starts_with(b"<!ENTITY") {
                        self.entity_declaration(inp, pos)?
                    } else if rest.starts_with(b"<!NOTATION") {
                        self.notation_declaration(inp, pos)?
                    } else if rest.starts_with(b"<!--") {
                        self.comment(inp, pos, false)?
                    } else if rest.starts_with(b"<?") {
                        self.processing_instruction(inp, pos, false)?
                    } else {
                        return self.fail(inp, pos, "internal subset is not finished");
                    };
                }
                _ => return self.fail(inp, pos, "internal subset is not finished"),
            }
        }
    }

    /// PEReference ::= '%' Name ';'
    ///
    /// Between declarations, a reference to a parameter entity stands for more declarations.
    fn parameter_entity_reference(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let name_start = start + 1;
        let name_end = scan_name(bytes, name_start);
        if name_end == name_start {
            return self.fail(inp, start, "PEReference: no name");
        }
        if at(bytes, name_end) != b';' {
            return self.fail(inp, name_end, "PEReference: expecting ';'");
        }
        let name = &bytes[name_start..name_end];
        let next = name_end + 1;
        self.has_parameter_entity_references = true;

        let found = self.names.find(name, self.storage()).and_then(|id| {
            let entity = self.dtd.as_ref()?.parameter.get(&id)?;
            Some((id, entity.kind, entity.text.clone(), entity.expanding))
        });
        let Some((id, entity_kind, text, expanding)) = found else {
            // The entity may be declared in something this parser doesn't read.
            if self.standalone {
                return self.fail(inp, start, format!("PEReference: %{}; not found", lossy(name)));
            }
            return Ok(next);
        };
        if entity_kind != EntityKind::Internal {
            return Ok(next);
        }
        if expanding {
            return self.fail(inp, start, "Detected an entity reference loop");
        }
        if self.entity_depth >= MAX_ENTITY_DEPTH {
            return self.fail(inp, start, "Maximum entity nesting depth exceeded");
        }
        self.account_for_expansion(inp, start, text.len())?;

        if let Some(entity) = self.dtd_mut().parameter.get_mut(&id) {
            entity.expanding = true;
        }
        self.entity_depth += 1;
        // An error in the entity's text is reported where the document refers to the entity.
        let saved_offset = self.entity_reference_offset;
        if inp.main {
            self.entity_reference_offset = start;
        }
        let result = self.subset(
            In {
                bytes: &text,
                main: false,
            },
            0,
            false,
        );
        self.entity_reference_offset = saved_offset;
        self.entity_depth -= 1;
        if let Some(entity) = self.dtd_mut().parameter.get_mut(&id) {
            entity.expanding = false;
        }
        result?;
        Ok(next)
    }

    /// Checks that a declaration ends where it should.
    fn end_of_declaration(&mut self, inp: In, pos: usize, message: &str) -> R<usize> {
        let pos = skip_spaces(inp.bytes, pos);
        if at(inp.bytes, pos) != b'>' {
            return self.fail(inp, pos, message);
        }
        Ok(pos + 1)
    }

    /// elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>'
    /// contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
    fn element_declaration(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut pos = self.required_spaces(inp, start + 9, "Space required after 'ELEMENT'")?;
        let name_end = scan_name(bytes, pos);
        if name_end == pos {
            return self.fail(inp, pos, "xmlParseElementDecl: no name for Element");
        }
        pos = self.required_spaces(inp, name_end, "Space required after the element name")?;

        let rest = &bytes[pos..];
        if rest.starts_with(b"EMPTY") {
            pos += 5;
        } else if rest.starts_with(b"ANY") {
            pos += 3;
        } else if rest.starts_with(b"(") {
            let inside = skip_spaces(bytes, pos + 1);
            if bytes[inside..].starts_with(b"#PCDATA") {
                pos = self.mixed_content(inp, inside + 7)?;
            } else {
                pos = self.content_group(inp, inside, 1)?;
                if matches!(at(bytes, pos), b'?' | b'*' | b'+') {
                    pos += 1;
                }
            }
        } else {
            return self.fail(inp, pos, "xmlParseElementDecl: 'EMPTY', 'ANY' or '(' expected");
        }
        self.end_of_declaration(inp, pos, "xmlParseElementDecl: expected '>' at the end")
    }

    /// Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*' | '(' S? '#PCDATA' S? ')'
    ///
    /// `pos` is right after '#PCDATA'.
    fn mixed_content(&mut self, inp: In, mut pos: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut names = 0;
        loop {
            pos = skip_spaces(bytes, pos);
            match at(bytes, pos) {
                b')' => {
                    pos += 1;
                    break;
                }
                b'|' => {
                    pos = skip_spaces(bytes, pos + 1);
                    let name_end = scan_name(bytes, pos);
                    if name_end == pos {
                        return self.fail(inp, pos, "xmlParseElementMixedContentDecl : Name expected");
                    }
                    names += 1;
                    pos = name_end;
                }
                _ => return self.fail(inp, pos, "MixedContentDecl : '|' or ')*' expected"),
            }
        }
        if at(bytes, pos) == b'*' {
            return Ok(pos + 1);
        }
        if names > 0 {
            return self.fail(inp, pos, "MixedContentDecl : '|' or ')*' expected");
        }
        Ok(pos)
    }

    /// choice ::= '(' S? cp ( S? '|' S? cp )+ S? ')'
    /// seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
    /// cp ::= (Name | choice | seq) ('?' | '*' | '+')?
    ///
    /// `pos` is after the opening parenthesis and the white space that follows it. Returns where the closing
    /// parenthesis ends.
    fn content_group(&mut self, inp: In, mut pos: usize, depth: u32) -> R<usize> {
        let bytes = inp.bytes;
        if depth > MAX_CONTENT_MODEL_DEPTH {
            return self.fail(inp, pos, "xmlParseElementChildrenContentDecl : depth too deep");
        }
        let mut separator = 0u8;
        loop {
            // A content particle.
            if at(bytes, pos) == b'(' {
                let inside = skip_spaces(bytes, pos + 1);
                pos = self.content_group(inp, inside, depth + 1)?;
            } else {
                let name_end = scan_name(bytes, pos);
                if name_end == pos {
                    return self.fail(inp, pos, "ContentDecl : Name or '(' expected");
                }
                pos = name_end;
            }
            if matches!(at(bytes, pos), b'?' | b'*' | b'+') {
                pos += 1;
            }

            pos = skip_spaces(bytes, pos);
            let byte = at(bytes, pos);
            if byte == b')' {
                return Ok(pos + 1);
            }
            if byte != b',' && byte != b'|' {
                return self.fail(inp, pos, "ContentDecl : ',' '|' or ')' expected");
            }
            if separator == 0 {
                separator = byte;
            } else if separator != byte {
                let message = format!("xmlParseElementChildrenContentDecl : '{}' expected", separator as char);
                return self.fail(inp, pos, message);
            }
            pos = skip_spaces(bytes, pos + 1);
        }
    }

    /// AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
    /// AttDef ::= S Name S AttType S DefaultDecl
    fn attlist_declaration(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut pos = self.required_spaces(inp, start + 9, "Space required after '<!ATTLIST'")?;
        let element_end = scan_name(bytes, pos);
        if element_end == pos {
            return self.fail(inp, pos, "ATTLIST: no name for Element");
        }
        let element = self.intern_name(inp, pos, element_end);
        pos = element_end;

        loop {
            let after_spaces = skip_spaces(bytes, pos);
            if at(bytes, after_spaces) == b'>' {
                return Ok(after_spaces + 1);
            }
            if after_spaces == pos {
                return self.fail(inp, pos, "Space required after the attribute default value");
            }
            let name_end = scan_name(bytes, after_spaces);
            if name_end == after_spaces {
                return self.fail(inp, after_spaces, "ATTLIST: no name for Attribute");
            }
            let name = self.intern_name(inp, after_spaces, name_end);
            pos = self.required_spaces(inp, name_end, "Space required after the attribute name")?;

            let (tokenized, next) = self.attribute_type(inp, pos)?;
            pos = self.required_spaces(inp, next, "Space required after the attribute type")?;

            // DefaultDecl ::= '#REQUIRED' | '#IMPLIED' | (('#FIXED' S)? AttValue)
            let rest = &bytes[pos..];
            let mut default = None;
            if rest.starts_with(b"#REQUIRED") {
                pos += 9;
            } else if rest.starts_with(b"#IMPLIED") {
                pos += 8;
            } else {
                if rest.starts_with(b"#FIXED") {
                    pos = self.required_spaces(inp, pos + 6, "Space required after '#FIXED'")?;
                }
                let quote = at(bytes, pos);
                if quote != b'"' && quote != b'\'' {
                    return self.fail(inp, pos, "Attribute default value declaration error");
                }
                let (offset, length, next) = self.attribute_value(inp, pos, tokenized)?;
                default = Some((offset, length));
                pos = next;
            }

            // The first definition of an attribute is the one that counts.
            let mut element_flags = 0;
            if default.is_some() {
                element_flags |= flag::HAS_DEFAULTS;
            }
            if tokenized {
                element_flags |= flag::HAS_TOKENIZED;
            }
            self.names.get_mut(element).flags |= element_flags;
            let dtd = self.dtd_mut();
            if let std::collections::hash_map::Entry::Vacant(entry) = dtd.defined_attributes.entry((element, name)) {
                entry.insert(tokenized);
                // Only a definition with a default is ever looked at again, and an element can have a great many
                // without one.
                if let Some(default) = default {
                    Rc::make_mut(dtd.attlists.entry(element).or_default()).push(AttributeDefinition { name, default });
                }
            }
        }
    }

    /// AttType ::= StringType | TokenizedType | EnumeratedType
    ///
    /// Returns whether the type is anything other than CDATA, and where it ends.
    fn attribute_type(&mut self, inp: In, pos: usize) -> R<(bool, usize)> {
        let bytes = inp.bytes;
        let rest = &bytes[pos..];
        if rest.starts_with(b"CDATA") {
            return Ok((false, pos + 5));
        }
        // The longer of two keywords that start the same has to be tried first.
        for keyword in [
            &b"IDREFS"[..],
            b"IDREF",
            b"ID",
            b"ENTITY",
            b"ENTITIES",
            b"NMTOKENS",
            b"NMTOKEN",
        ] {
            if rest.starts_with(keyword) {
                return Ok((true, pos + keyword.len()));
            }
        }

        // EnumeratedType ::= NotationType | Enumeration
        // NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
        // Enumeration ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')'
        let mut pos = pos;
        let is_notation = rest.starts_with(b"NOTATION");
        if is_notation {
            pos = self.required_spaces(inp, pos + 8, "Space required after 'NOTATION'")?;
        }
        if at(bytes, pos) != b'(' {
            let message = if is_notation {
                "'(' required to start 'NOTATION'"
            } else {
                "Invalid attribute type"
            };
            return self.fail(inp, pos, message);
        }
        pos += 1;
        loop {
            pos = skip_spaces(bytes, pos);
            let token_end = if is_notation {
                scan_name(bytes, pos)
            } else {
                scan_nmtoken(bytes, pos)
            };
            if token_end == pos {
                let message = if is_notation {
                    "Name expected in NOTATION declaration"
                } else {
                    "NmToken expected in ATTLIST enumeration"
                };
                return self.fail(inp, pos, message);
            }
            pos = skip_spaces(bytes, token_end);
            match at(bytes, pos) {
                b')' => return Ok((true, pos + 1)),
                b'|' => pos += 1,
                _ => {
                    let message = if is_notation {
                        "')' required to finish NOTATION declaration"
                    } else {
                        "')' required to finish ATTLIST enumeration"
                    };
                    return self.fail(inp, pos, message);
                }
            }
        }
    }

    /// EntityDecl ::= '<!ENTITY' S Name S EntityDef S? '>' | '<!ENTITY' S '%' S Name S PEDef S? '>'
    /// EntityDef ::= EntityValue | (ExternalID NDataDecl?)
    /// PEDef ::= EntityValue | ExternalID
    fn entity_declaration(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut pos = self.required_spaces(inp, start + 8, "Space required after '<!ENTITY'")?;
        let is_parameter = at(bytes, pos) == b'%';
        if is_parameter {
            pos = self.required_spaces(inp, pos + 1, "Space required after '%'")?;
        }
        let name_end = scan_name(bytes, pos);
        if name_end == pos {
            return self.fail(inp, pos, "xmlParseEntityDecl: no name");
        }
        let name_bytes = &bytes[pos..name_end];
        if name_bytes.contains(&b':') {
            let message = format!("colons are forbidden from entities names '{}'", lossy(name_bytes));
            self.namespace_error(inp, pos, message);
        }
        let name = self.intern_name(inp, pos, name_end);
        pos = self.required_spaces(inp, name_end, "Space required after the entity name")?;

        let entity;
        let quote = at(bytes, pos);
        if quote == b'"' || quote == b'\'' {
            let (text, next) = self.entity_value(inp, pos)?;
            entity = Entity {
                kind: EntityKind::Internal,
                text,
                expanding: false,
            };
            pos = next;
        } else {
            let (external_id, next) = self.external_id(inp, pos, true)?;
            let Some((system_start, system_end)) = external_id.system else {
                return self.fail(inp, pos, "Entity value required");
            };
            if bytes[system_start..system_end].contains(&b'#') {
                return self.fail(inp, system_start, "Fragment not allowed");
            }
            pos = next;
            let mut entity_kind = EntityKind::External;
            if !is_parameter {
                let after_spaces = skip_spaces(bytes, pos);
                if bytes[after_spaces..].starts_with(b"NDATA") {
                    if after_spaces == pos {
                        return self.fail(inp, pos, "Space required before 'NDATA'");
                    }
                    pos = self.required_spaces(inp, after_spaces + 5, "Space required after 'NDATA'")?;
                    let notation_end = scan_name(bytes, pos);
                    if notation_end == pos {
                        return self.fail(inp, pos, "xmlParseEntityDecl: no notation name");
                    }
                    pos = notation_end;
                    entity_kind = EntityKind::Unparsed;
                }
            }
            entity = Entity {
                kind: entity_kind,
                text: Rc::from(&b""[..]),
                expanding: false,
            };
        }

        let message = format!("xmlParseEntityDecl: entity {} not terminated", lossy(name_bytes));
        pos = self.end_of_declaration(inp, pos, &message)?;

        // The first declaration of an entity is the one that counts. The five predefined entities are what they are.
        let is_predefined = matches!(name_bytes, b"lt" | b"gt" | b"amp" | b"apos" | b"quot");
        let dtd = self.dtd_mut();
        if is_parameter {
            dtd.parameter.entry(name).or_insert(entity);
        } else if !is_predefined {
            dtd.general.entry(name).or_insert(entity);
        }
        Ok(pos)
    }

    /// EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"' | "'" ([^%&'] | PEReference | Reference)* "'"
    ///
    /// Returns the entity's replacement text: the literal with its character references replaced. References to
    /// general entities stay as they are, to be followed when the entity is used.
    ///
    /// Line ends stay as they are too, and are normalized where the entity is used, as they are in libxml2. So a CR
    /// that is written as a character reference becomes a line end in content like one that is written out, and a
    /// CR LF pair is two spaces in an attribute value.
    fn entity_value(&mut self, inp: In, start: usize) -> R<(Rc<[u8]>, usize)> {
        let bytes = inp.bytes;
        let quote = bytes[start];
        let mut pos = start + 1;
        let mut text = Vec::new();
        loop {
            if pos >= bytes.len() {
                return self.fail(inp, start, "EntityValue: \" expected");
            }
            let byte = bytes[pos];
            if byte == quote {
                break;
            }
            match byte {
                b'&' => {
                    if at(bytes, pos + 1) == b'#' {
                        let (c, next) = self.character_reference(inp, pos)?;
                        chars::encode_utf8(c, &mut text);
                        pos = next;
                    } else {
                        let name_end = scan_name(bytes, pos + 1);
                        if name_end == pos + 1 || at(bytes, name_end) != b';' {
                            return self.fail(inp, pos, "EntityValue: '&' forbidden except for entities references");
                        }
                        text.extend_from_slice(&bytes[pos..=name_end]);
                        pos = name_end + 1;
                    }
                }
                // In the internal subset a parameter entity can only be referenced between declarations.
                b'%' => return self.fail(inp, pos, "PEReferences forbidden in internal subset"),
                _ if byte >= 0x80 => {
                    let length = self.check_non_ascii(inp, pos)?;
                    text.extend_from_slice(&bytes[pos..pos + length]);
                    pos += length;
                }
                _ if CLASS[byte as usize] & chars::INVALID != 0 => {
                    return self.fail(inp, pos, "invalid character in entity value");
                }
                _ => {
                    text.push(byte);
                    pos += 1;
                }
            }
        }
        Ok((Rc::from(text), pos + 1))
    }

    /// NotationDecl ::= '<!NOTATION' S Name S (ExternalID | PublicID) S? '>'
    fn notation_declaration(&mut self, inp: In, start: usize) -> R<usize> {
        let bytes = inp.bytes;
        let mut pos = self.required_spaces(inp, start + 10, "Space required after '<!NOTATION'")?;
        let name_end = scan_name(bytes, pos);
        if name_end == pos {
            return self.fail(inp, pos, "NOTATION: Name expected here");
        }
        if bytes[pos..name_end].contains(&b':') {
            let message = format!(
                "colons are forbidden from notation names '{}'",
                lossy(&bytes[pos..name_end])
            );
            self.namespace_error(inp, pos, message);
        }
        pos = self.required_spaces(inp, name_end, "Space required after the NOTATION name'")?;
        let (external_id, next) = self.external_id(inp, pos, false)?;
        if external_id.public.is_none() && external_id.system.is_none() {
            return self.fail(inp, pos, "NOTATION: external identifier expected");
        }
        self.end_of_declaration(inp, next, "'>' required to close NOTATION declaration")
    }
}
