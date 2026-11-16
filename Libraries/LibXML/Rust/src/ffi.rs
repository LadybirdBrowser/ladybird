/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C interface. `rust_xml_parse` returns an opaque document, `rust_xml_document_view` hands out pointers to its
//! columns, and `rust_xml_document_free` releases it. The view is plain data: reading a document from C++ is loops
//! over arrays, with no call back into Rust per row.

use std::ffi::c_void;

use crate::Document;
use crate::Options;
use crate::rust_panic::abort_on_panic;
use crate::tape::kind;

/// What the low bits of a row's kind byte say. See `tape`.
#[repr(u8)]
pub enum XmlRowKind {
    Start = 1,
    Attribute = 2,
    Value = 3,
    End = 4,
    Text = 5,
    Cdata = 6,
    Comment = 7,
    ProcessingInstruction = 8,
    Doctype = 9,
}

/// The bits of a kind byte that hold the kind.
#[repr(u8)]
pub enum XmlRowKindBits {
    Mask = 0x0F,
}

/// The flags in the kind byte of an attribute row.
#[repr(u8)]
pub enum XmlAttributeFlag {
    /// The attribute wasn't in the tag, its value is the default the DTD declares.
    Defaulted = 0x40,
    /// The attribute is a namespace declaration.
    NamespaceDeclaration = 0x80,
}

/// The flags in the kind byte of a value row.
#[repr(u8)]
pub enum XmlValueFlag {
    /// On an id of a doctype: there is no such id, as opposed to an empty one.
    Absent = 0x80,
}

// The values above are written out because the header generator can't follow a path to a constant.
const _: () = {
    assert!(XmlRowKind::Start as u8 == kind::START);
    assert!(XmlRowKind::Attribute as u8 == kind::ATTR);
    assert!(XmlRowKind::Value as u8 == kind::VALUE);
    assert!(XmlRowKind::End as u8 == kind::END);
    assert!(XmlRowKind::Text as u8 == kind::TEXT);
    assert!(XmlRowKind::Cdata as u8 == kind::CDATA);
    assert!(XmlRowKind::Comment as u8 == kind::COMMENT);
    assert!(XmlRowKind::ProcessingInstruction as u8 == kind::PI);
    assert!(XmlRowKind::Doctype as u8 == kind::DOCTYPE);
    assert!(XmlRowKindBits::Mask as u8 == kind::MASK);
    assert!(XmlAttributeFlag::Defaulted as u8 == kind::DEFAULTED);
    assert!(XmlAttributeFlag::NamespaceDeclaration as u8 == kind::NAMESPACE_DECLARATION);
    assert!(XmlValueFlag::Absent as u8 == kind::ABSENT);
};

/// Resolves the name of an HTML character entity. Writes what it stands for, as UTF-8, to `out`, which has room for
/// `capacity` bytes, and returns how many bytes that was, or a negative number if there is no such entity.
/// Null for no resolver.
pub type XmlHtmlEntityResolver = Option<
    unsafe extern "C" fn(
        context: *mut c_void,
        name: *const u8,
        name_length: usize,
        out: *mut u8,
        capacity: usize,
    ) -> isize,
>;

#[repr(C)]
pub struct XmlParseOptions {
    pub preserve_comments: bool,
    pub preserve_cdata: bool,
    /// Used for references to undeclared entities in a document whose doctype has one of the XHTML public ids.
    pub resolve_html_entity: XmlHtmlEntityResolver,
    pub resolver_context: *mut c_void,
}

/// Where a name is, in the address space string operands point into: the source, then the arena.
#[repr(C)]
pub struct XmlName {
    pub offset: u32,
    pub length: u32,
}

#[repr(C)]
pub struct XmlError {
    pub message: *const u8,
    pub message_length: usize,
    pub offset: u32,
    pub line: u32,
    pub column: u32,
    /// The row the error comes before.
    pub row: u32,
    pub fatal: bool,
}

/// Pointers into a document. They are good until the document is freed.
#[repr(C)]
pub struct XmlDocumentView {
    pub kinds: *const u8,
    pub a: *const u32,
    pub b: *const u32,
    pub row_count: usize,
    pub arena: *const u8,
    pub arena_length: usize,
    pub names: *const XmlName,
    pub name_count: usize,
    pub errors: *const XmlError,
    pub error_count: usize,
    pub version_1_0: bool,
    /// Whether the parser got past the XML declaration. If it didn't, the document never started.
    pub started: bool,
}

pub struct XmlDocument {
    document: Document,
    names: Vec<XmlName>,
    errors: Vec<XmlError>,
}

/// Parses the `length` bytes of UTF-8 at `source` as a whole document. Never returns null: a document that isn't
/// well-formed is a document with errors in it. The result has to be freed with `rust_xml_document_free`.
///
/// # Safety
/// `source` must point to `length` readable bytes, or `length` must be zero. `options` must be null or point to a
/// valid `XmlParseOptions`. The resolver, if there is one, must be safe to call with `resolver_context` for as long
/// as this call lasts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_xml_parse(
    source: *const u8,
    length: usize,
    options: *const XmlParseOptions,
) -> *mut XmlDocument {
    abort_on_panic(|| {
        // SAFETY: The caller promises `length` readable bytes at `source`.
        let source: &[u8] = unsafe { ak::slice_from_raw_parts(source, length) };
        // SAFETY: The caller promises `options` is null or valid.
        let options = unsafe { options.as_ref() };

        let resolver = options.and_then(|options| {
            let resolve = options.resolve_html_entity?;
            let context = options.resolver_context;
            Some(move |name: &[u8], out: &mut Vec<u8>| -> bool {
                let mut buffer = [0u8; 64];
                // SAFETY: The caller promises the resolver can be called with its context. It is given a buffer and
                // the buffer's true size.
                let written = unsafe { resolve(context, name.as_ptr(), name.len(), buffer.as_mut_ptr(), buffer.len()) };
                match usize::try_from(written) {
                    Ok(written) if written <= buffer.len() => {
                        out.extend_from_slice(&buffer[..written]);
                        true
                    }
                    _ => false,
                }
            })
        });
        let document = crate::parse(
            source,
            Options {
                preserve_comments: options.is_some_and(|options| options.preserve_comments),
                preserve_cdata: options.is_none_or(|options| options.preserve_cdata),
                resolve_html_entity: resolver.as_ref().map(|resolver| resolver as crate::HtmlEntityResolver),
            },
        );

        let names = document
            .names
            .entries()
            .iter()
            .map(|name| XmlName {
                offset: name.offset,
                length: name.length,
            })
            .collect();
        let errors = document
            .errors
            .iter()
            .map(|error| XmlError {
                // The message is owned by `document`, which lives exactly as long as this pointer to it.
                message: error.message.as_ptr(),
                message_length: error.message.len(),
                offset: error.offset,
                line: error.line,
                column: error.column,
                row: error.row,
                fatal: error.fatal,
            })
            .collect();
        Box::into_raw(Box::new(XmlDocument {
            document,
            names,
            errors,
        }))
    })
}

/// # Safety
/// `document` must be a document from `rust_xml_parse` that hasn't been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_xml_document_view(document: *const XmlDocument) -> XmlDocumentView {
    // SAFETY: The caller promises a live document.
    let document = unsafe { &*document };
    let tape = &document.document.tape;
    XmlDocumentView {
        kinds: tape.kinds().as_ptr(),
        a: tape.a().as_ptr(),
        b: tape.b().as_ptr(),
        row_count: tape.len(),
        arena: document.document.arena.as_ptr(),
        arena_length: document.document.arena.len(),
        names: document.names.as_ptr(),
        name_count: document.names.len(),
        errors: document.errors.as_ptr(),
        error_count: document.errors.len(),
        version_1_0: document.document.version_1_0,
        started: document.document.started,
    }
}

/// # Safety
/// `document` must be null or a document from `rust_xml_parse` that hasn't been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_xml_document_free(document: *mut XmlDocument) {
    if !document.is_null() {
        // SAFETY: The caller promises the pointer came from `rust_xml_parse`, where it was a Box, and that this is
        // the only time it is freed.
        drop(unsafe { Box::from_raw(document) });
    }
}

/// What a node of an XPath program is. See `xpath` for what each kind's operands mean.
#[repr(u8)]
pub enum XPathKind {
    Or = 0,
    And = 1,
    Equal = 2,
    NotEqual = 3,
    Less = 4,
    LessOrEqual = 5,
    Greater = 6,
    GreaterOrEqual = 7,
    Add = 8,
    Subtract = 9,
    Multiply = 10,
    Divide = 11,
    Modulo = 12,
    Union = 13,
    Negate = 14,
    Number = 15,
    String = 16,
    Variable = 17,
    Function = 18,
    Filter = 19,
    RelativePath = 20,
    AbsolutePath = 21,
    ExpressionPath = 22,
}

#[repr(u8)]
pub enum XPathAxis {
    Ancestor = 0,
    AncestorOrSelf = 1,
    Attribute = 2,
    Child = 3,
    Descendant = 4,
    DescendantOrSelf = 5,
    Following = 6,
    FollowingSibling = 7,
    Namespace = 8,
    Parent = 9,
    Preceding = 10,
    PrecedingSibling = 11,
    Self_ = 12,
}

#[repr(u8)]
pub enum XPathNodeTest {
    Name = 0,
    AnyName = 1,
    AnyNameInNamespace = 2,
    Node = 3,
    Text = 4,
    Comment = 5,
    ProcessingInstruction = 6,
    ProcessingInstructionWithTarget = 7,
}

#[repr(u32)]
pub enum XPathFunction {
    Boolean = 0,
    Ceiling = 1,
    Concat = 2,
    Contains = 3,
    Count = 4,
    False = 5,
    Floor = 6,
    Id = 7,
    Lang = 8,
    Last = 9,
    LocalName = 10,
    Name = 11,
    NamespaceUri = 12,
    NormalizeSpace = 13,
    Not = 14,
    Number = 15,
    Position = 16,
    Round = 17,
    StartsWith = 18,
    String = 19,
    StringLength = 20,
    Substring = 21,
    SubstringAfter = 22,
    SubstringBefore = 23,
    Sum = 24,
    Translate = 25,
    True = 26,
}

const _: () = {
    use crate::xpath::axis;
    use crate::xpath::function;
    use crate::xpath::kind;
    use crate::xpath::test;
    assert!(XPathKind::Or as u8 == kind::OR);
    assert!(XPathKind::And as u8 == kind::AND);
    assert!(XPathKind::Equal as u8 == kind::EQUAL);
    assert!(XPathKind::NotEqual as u8 == kind::NOT_EQUAL);
    assert!(XPathKind::Less as u8 == kind::LESS);
    assert!(XPathKind::LessOrEqual as u8 == kind::LESS_OR_EQUAL);
    assert!(XPathKind::Greater as u8 == kind::GREATER);
    assert!(XPathKind::GreaterOrEqual as u8 == kind::GREATER_OR_EQUAL);
    assert!(XPathKind::Add as u8 == kind::ADD);
    assert!(XPathKind::Subtract as u8 == kind::SUBTRACT);
    assert!(XPathKind::Multiply as u8 == kind::MULTIPLY);
    assert!(XPathKind::Divide as u8 == kind::DIVIDE);
    assert!(XPathKind::Modulo as u8 == kind::MODULO);
    assert!(XPathKind::Union as u8 == kind::UNION);
    assert!(XPathKind::Negate as u8 == kind::NEGATE);
    assert!(XPathKind::Number as u8 == kind::NUMBER);
    assert!(XPathKind::String as u8 == kind::STRING);
    assert!(XPathKind::Variable as u8 == kind::VARIABLE);
    assert!(XPathKind::Function as u8 == kind::FUNCTION);
    assert!(XPathKind::Filter as u8 == kind::FILTER);
    assert!(XPathKind::RelativePath as u8 == kind::RELATIVE_PATH);
    assert!(XPathKind::AbsolutePath as u8 == kind::ABSOLUTE_PATH);
    assert!(XPathKind::ExpressionPath as u8 == kind::EXPRESSION_PATH);

    assert!(XPathAxis::Ancestor as u8 == axis::ANCESTOR);
    assert!(XPathAxis::AncestorOrSelf as u8 == axis::ANCESTOR_OR_SELF);
    assert!(XPathAxis::Attribute as u8 == axis::ATTRIBUTE);
    assert!(XPathAxis::Child as u8 == axis::CHILD);
    assert!(XPathAxis::Descendant as u8 == axis::DESCENDANT);
    assert!(XPathAxis::DescendantOrSelf as u8 == axis::DESCENDANT_OR_SELF);
    assert!(XPathAxis::Following as u8 == axis::FOLLOWING);
    assert!(XPathAxis::FollowingSibling as u8 == axis::FOLLOWING_SIBLING);
    assert!(XPathAxis::Namespace as u8 == axis::NAMESPACE);
    assert!(XPathAxis::Parent as u8 == axis::PARENT);
    assert!(XPathAxis::Preceding as u8 == axis::PRECEDING);
    assert!(XPathAxis::PrecedingSibling as u8 == axis::PRECEDING_SIBLING);
    assert!(XPathAxis::Self_ as u8 == axis::SELF);

    assert!(XPathNodeTest::Name as u8 == test::NAME);
    assert!(XPathNodeTest::AnyName as u8 == test::ANY_NAME);
    assert!(XPathNodeTest::AnyNameInNamespace as u8 == test::ANY_NAME_IN_NAMESPACE);
    assert!(XPathNodeTest::Node as u8 == test::NODE);
    assert!(XPathNodeTest::Text as u8 == test::TEXT);
    assert!(XPathNodeTest::Comment as u8 == test::COMMENT);
    assert!(XPathNodeTest::ProcessingInstruction as u8 == test::PROCESSING_INSTRUCTION);
    assert!(XPathNodeTest::ProcessingInstructionWithTarget as u8 == test::PROCESSING_INSTRUCTION_WITH_TARGET);

    assert!(XPathFunction::Boolean as u32 == function::BOOLEAN);
    assert!(XPathFunction::Ceiling as u32 == function::CEILING);
    assert!(XPathFunction::Concat as u32 == function::CONCAT);
    assert!(XPathFunction::Contains as u32 == function::CONTAINS);
    assert!(XPathFunction::Count as u32 == function::COUNT);
    assert!(XPathFunction::False as u32 == function::FALSE);
    assert!(XPathFunction::Floor as u32 == function::FLOOR);
    assert!(XPathFunction::Id as u32 == function::ID);
    assert!(XPathFunction::Lang as u32 == function::LANG);
    assert!(XPathFunction::Last as u32 == function::LAST);
    assert!(XPathFunction::LocalName as u32 == function::LOCAL_NAME);
    assert!(XPathFunction::Name as u32 == function::NAME);
    assert!(XPathFunction::NamespaceUri as u32 == function::NAMESPACE_URI);
    assert!(XPathFunction::NormalizeSpace as u32 == function::NORMALIZE_SPACE);
    assert!(XPathFunction::Not as u32 == function::NOT);
    assert!(XPathFunction::Number as u32 == function::NUMBER);
    assert!(XPathFunction::Position as u32 == function::POSITION);
    assert!(XPathFunction::Round as u32 == function::ROUND);
    assert!(XPathFunction::StartsWith as u32 == function::STARTS_WITH);
    assert!(XPathFunction::String as u32 == function::STRING);
    assert!(XPathFunction::StringLength as u32 == function::STRING_LENGTH);
    assert!(XPathFunction::Substring as u32 == function::SUBSTRING);
    assert!(XPathFunction::SubstringAfter as u32 == function::SUBSTRING_AFTER);
    assert!(XPathFunction::SubstringBefore as u32 == function::SUBSTRING_BEFORE);
    assert!(XPathFunction::Sum as u32 == function::SUM);
    assert!(XPathFunction::Translate as u32 == function::TRANSLATE);
    assert!(XPathFunction::True as u32 == function::TRUE);
};

/// A compiled XPath expression.
pub struct XPathProgram {
    program: crate::xpath::Program,
}

/// Pointers into a compiled XPath expression, good until it is freed. Offsets in it are into the expression's text
/// as it was given to `rust_xpath_compile`.
#[repr(C)]
pub struct XPathProgramView {
    pub kinds: *const u8,
    pub a: *const u32,
    pub b: *const u32,
    pub c: *const u32,
    pub node_count: usize,
    pub steps: *const crate::xpath::Step,
    pub step_count: usize,
    pub lists: *const u32,
    pub list_count: usize,
    pub numbers: *const f64,
    pub number_count: usize,
    pub root: u32,
}

/// Compiles an XPath 1.0 expression. Returns null if it isn't one.
///
/// # Safety
/// `source` must point to `length` readable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_xpath_compile(source: *const u8, length: usize) -> *mut XPathProgram {
    abort_on_panic(|| {
        // SAFETY: The caller promises `length` readable bytes at `source`.
        let source: &[u8] = unsafe { ak::slice_from_raw_parts(source, length) };
        match crate::xpath::compile(source) {
            Ok(program) => Box::into_raw(Box::new(XPathProgram { program })),
            Err(_) => std::ptr::null_mut(),
        }
    })
}

/// # Safety
/// `program` must be a live program from `rust_xpath_compile`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_xpath_program_view(program: *const XPathProgram) -> XPathProgramView {
    // SAFETY: The caller promises a live program.
    let program = unsafe { &(*program).program };
    XPathProgramView {
        kinds: program.kinds.as_ptr(),
        a: program.a.as_ptr(),
        b: program.b.as_ptr(),
        c: program.c.as_ptr(),
        node_count: program.kinds.len(),
        steps: program.steps.as_ptr(),
        step_count: program.steps.len(),
        lists: program.lists.as_ptr(),
        list_count: program.lists.len(),
        numbers: program.numbers.as_ptr(),
        number_count: program.numbers.len(),
        root: program.root,
    }
}

/// # Safety
/// `program` must be null or a program from `rust_xpath_compile` that hasn't been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_xpath_program_free(program: *mut XPathProgram) {
    if !program.is_null() {
        // SAFETY: The caller promises the pointer came from `rust_xpath_compile`, where it was a Box, and that this
        // is the only time it is freed.
        drop(unsafe { Box::from_raw(program) });
    }
}
