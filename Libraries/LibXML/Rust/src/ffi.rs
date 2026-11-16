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
