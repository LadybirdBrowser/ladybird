/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Checks a `Document` against everything a reader of one relies on. For tests and fuzzing: whatever the input, the
//! parser has to produce a document this accepts.

use crate::Document;
use crate::tape::NONE;
use crate::tape::kind;

/// Returns what is wrong with `document`, the result of parsing `source`, if anything is.
pub fn verify(document: &Document, source: &[u8]) -> Result<(), String> {
    let tape = &document.tape;
    let (kinds, a, b) = (tape.kinds(), tape.a(), tape.b());
    let names = document.names.entries();
    let end_of_strings = source.len() + document.arena.len();

    // A string is wholly in the source or wholly in the arena, and is UTF-8: readers turn it into text unchecked.
    let string = |offset: u32, length: u32, what: &str| -> Result<(), String> {
        let (offset, length) = (offset as usize, length as usize);
        let bytes = if offset < source.len() {
            source.get(offset..offset + length)
        } else if offset + length <= end_of_strings {
            document
                .arena
                .get(offset - source.len()..offset - source.len() + length)
        } else {
            None
        };
        let Some(bytes) = bytes else {
            return Err(format!("{what}: string {offset}+{length} is out of range"));
        };
        if std::str::from_utf8(bytes).is_err() {
            return Err(format!("{what}: string {offset}+{length} is not UTF-8"));
        }
        Ok(())
    };
    let name = |id: u32, what: &str| -> Result<(), String> {
        let Some(entry) = names.get(id as usize) else {
            return Err(format!("{what}: name {id} is out of range"));
        };
        if entry.length == 0 {
            return Err(format!("{what}: name {id} is empty"));
        }
        string(entry.offset, entry.length, what)
    };
    let value = |row: usize, what: &str| -> Result<(), String> {
        if row >= kinds.len() || kinds[row] & kind::MASK != kind::VALUE {
            return Err(format!("{what} at row {row}: no value row"));
        }
        string(a[row], b[row], what)
    };

    let fatal = document.errors.iter().any(|error| error.fatal);
    let mut open: Vec<u32> = Vec::new();
    let mut roots = 0;
    let mut row = 0;
    while row < kinds.len() {
        match kinds[row] & kind::MASK {
            kind::START => {
                name(a[row], "start tag")?;
                if b[row] != NONE {
                    name(b[row], "element namespace")?;
                }
                if open.is_empty() {
                    roots += 1;
                }
                open.push(a[row]);
                while row + 1 < kinds.len() && kinds[row + 1] & kind::MASK == kind::ATTR {
                    name(a[row + 1], "attribute")?;
                    if b[row + 1] != NONE {
                        name(b[row + 1], "attribute namespace")?;
                    }
                    value(row + 2, "attribute")?;
                    row += 2;
                }
            }
            kind::END => {
                name(a[row], "end tag")?;
                if open.pop() != Some(a[row]) {
                    return Err(format!("row {row}: end tag doesn't match the open element"));
                }
            }
            kind::TEXT | kind::CDATA | kind::COMMENT => string(a[row], b[row], "character data")?,
            kind::PI => {
                name(a[row], "processing instruction")?;
                value(row + 1, "processing instruction")?;
                row += 1;
            }
            kind::DOCTYPE => {
                name(a[row], "doctype")?;
                value(row + 1, "doctype")?;
                value(row + 2, "doctype")?;
                row += 2;
            }
            other => return Err(format!("row {row}: unexpected kind {other}")),
        }
        row += 1;
    }

    if !fatal {
        if !open.is_empty() {
            return Err("an element is left open in a document without a fatal error".into());
        }
        if roots != 1 {
            return Err(format!("{roots} root elements in a document without a fatal error"));
        }
    } else if roots > 1 {
        return Err(format!("{roots} root elements"));
    }

    let mut previous_row = 0;
    for (index, error) in document.errors.iter().enumerate() {
        if error.row < previous_row || error.row as usize > kinds.len() {
            return Err(format!("error {index} is out of order, at row {}", error.row));
        }
        previous_row = error.row;
        if error.fatal && index + 1 != document.errors.len() {
            return Err(format!("error {index} is fatal but isn't the last"));
        }
        if error.offset as usize > source.len() {
            return Err(format!("error {index} is past the end of the source"));
        }
    }
    Ok(())
}
