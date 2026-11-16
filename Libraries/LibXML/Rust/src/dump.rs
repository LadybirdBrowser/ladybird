/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A tape as text, one event to a line, for tests and for comparing against another parser.

use std::fmt::Write;

use crate::Document;
use crate::tape::NONE;
use crate::tape::kind;

fn escape(bytes: &[u8], out: &mut String) {
    for &byte in bytes {
        match byte {
            b'\n' => out.push_str("\\n"),
            b'\r' => out.push_str("\\r"),
            b'\t' => out.push_str("\\t"),
            b'\\' => out.push_str("\\\\"),
            b'"' => out.push_str("\\\""),
            0x20..=0x7E => out.push(byte as char),
            _ => {
                let _ = write!(out, "\\x{byte:02X}");
            }
        }
    }
}

/// Writes the events of `document`. Adjacent text is joined, since where a parser splits a run of text is up to it.
/// With `namespaces`, each element and attribute name is followed by its namespace name in braces.
pub fn dump(document: &Document, source: &[u8], namespaces: bool) -> String {
    let storage = document.storage(source);
    let tape = &document.tape;
    let mut out = String::new();
    let name = |id: u32, out: &mut String| escape(document.names.bytes(id, storage), out);
    let namespace = |id: u32, out: &mut String| {
        if namespaces && id != NONE {
            out.push('{');
            escape(document.names.bytes(id, storage), out);
            out.push('}');
        }
    };

    let mut errors = document.errors.iter().peekable();
    let mut in_text = false;
    let mut index = 0;
    loop {
        while let Some(error) = errors.peek() {
            if error.row as usize > index {
                break;
            }
            if in_text {
                out.push('\n');
                in_text = false;
            }
            let _ = writeln!(out, "{} {}", if error.fatal { "FATAL" } else { "ERROR" }, error.message);
            errors.next();
        }
        if index >= tape.len() {
            break;
        }
        let (row_kind, a, b) = tape.row(index);
        let is_text = row_kind & kind::MASK == kind::TEXT;
        if in_text && !is_text {
            out.push('\n');
            in_text = false;
        }
        match row_kind & kind::MASK {
            kind::START => {
                out.push_str("START ");
                name(a, &mut out);
                namespace(b, &mut out);
                index += 1;
                while index < tape.len() && tape.row(index).0 & kind::MASK == kind::ATTR {
                    let (_, attribute, attribute_namespace) = tape.row(index);
                    let (_, offset, length) = tape.row(index + 1);
                    out.push(' ');
                    name(attribute, &mut out);
                    namespace(attribute_namespace, &mut out);
                    out.push_str("=\"");
                    escape(storage.get(offset, length), &mut out);
                    out.push('"');
                    index += 2;
                }
                out.push('\n');
                continue;
            }
            kind::END => {
                out.push_str("END ");
                name(a, &mut out);
                out.push('\n');
            }
            kind::TEXT => {
                if !in_text {
                    out.push_str("TEXT ");
                    in_text = true;
                }
                escape(storage.get(a, b), &mut out);
            }
            kind::CDATA => {
                out.push_str("CDATA ");
                escape(storage.get(a, b), &mut out);
                out.push('\n');
            }
            kind::COMMENT => {
                out.push_str("COMMENT ");
                escape(storage.get(a, b), &mut out);
                out.push('\n');
            }
            kind::PI => {
                let (_, offset, length) = tape.row(index + 1);
                out.push_str("PI ");
                name(a, &mut out);
                out.push(' ');
                escape(storage.get(offset, length), &mut out);
                out.push('\n');
                index += 1;
            }
            kind::DOCTYPE => {
                out.push_str("DOCTYPE ");
                name(a, &mut out);
                for id in [index + 1, index + 2] {
                    let (value_kind, offset, length) = tape.row(id);
                    if value_kind & kind::ABSENT != 0 {
                        out.push_str(" -");
                    } else {
                        out.push_str(" \"");
                        escape(storage.get(offset, length), &mut out);
                        out.push('"');
                    }
                }
                out.push('\n');
                index += 2;
            }
            _ => {}
        }
        index += 1;
    }
    out
}
