/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the parser produces: a flat list of events in document order, stored as three parallel columns.
//!
//! A row is a kind and two 32-bit operands. Strings are never copied out of the document if they can be used as they
//! are: a string operand pair is an offset and a length into one address space that holds the source first and the
//! parser's arena (decoded text, normalized attribute values) right after it. Names are ids into the name table.
//!
//! | kind    | a            | b            | followed by                                  |
//! |---------|--------------|--------------|----------------------------------------------|
//! | START   | name         | namespace    | its ATTR rows                                |
//! | ATTR    | name         | namespace    | one VALUE row                                |
//! | VALUE   | offset       | length       |                                              |
//! | END     | name         | NONE         |                                              |
//! | TEXT    | offset       | length       |                                              |
//! | CDATA   | offset       | length       |                                              |
//! | COMMENT | offset       | length       |                                              |
//! | PI      | target name  | NONE         | one VALUE row, the data                      |
//! | DOCTYPE | name         | NONE         | two VALUE rows, the public and system ids    |

pub const NONE: u32 = u32::MAX;

pub mod kind {
    pub const START: u8 = 1;
    pub const ATTR: u8 = 2;
    pub const VALUE: u8 = 3;
    pub const END: u8 = 4;
    pub const TEXT: u8 = 5;
    pub const CDATA: u8 = 6;
    pub const COMMENT: u8 = 7;
    pub const PI: u8 = 8;
    pub const DOCTYPE: u8 = 9;

    /// The low bits of a kind byte hold the kind, the high bits its flags.
    pub const MASK: u8 = 0x0F;

    /// On ATTR: the attribute is a namespace declaration.
    pub const NAMESPACE_DECLARATION: u8 = 0x80;
    /// On ATTR: the attribute wasn't in the tag, its value is the default the DTD declares.
    pub const DEFAULTED: u8 = 0x40;
    /// On a VALUE row of a DOCTYPE: the id is absent, as opposed to empty.
    pub const ABSENT: u8 = 0x80;
}

#[derive(Default)]
pub struct Tape {
    // The columns are always fully initialized up to their capacity and `len` says how much is in use, so that
    // appending a row is one capacity check and three plain stores.
    kind: Vec<u8>,
    a: Vec<u32>,
    b: Vec<u32>,
    len: usize,
}

impl Tape {
    pub fn with_capacity(rows: usize) -> Self {
        let rows = rows.max(16);
        Tape {
            kind: vec![0; rows],
            a: vec![0; rows],
            b: vec![0; rows],
            len: 0,
        }
    }

    #[inline(always)]
    pub fn push(&mut self, kind: u8, a: u32, b: u32) {
        if self.len == self.kind.len() {
            self.grow();
        }
        let index = self.len;
        self.kind[index] = kind;
        self.a[index] = a;
        self.b[index] = b;
        self.len = index + 1;
    }

    #[cold]
    #[inline(never)]
    fn grow(&mut self) {
        let rows = (self.kind.len() * 2).max(16);
        self.kind.resize(rows, 0);
        self.a.resize(rows, 0);
        self.b.resize(rows, 0);
    }

    #[inline(always)]
    pub fn len(&self) -> usize {
        self.len
    }

    pub fn is_empty(&self) -> bool {
        self.len == 0
    }

    pub fn truncate(&mut self, len: usize) {
        self.len = self.len.min(len);
    }

    pub fn kinds(&self) -> &[u8] {
        &self.kind[..self.len]
    }

    pub fn a(&self) -> &[u32] {
        &self.a[..self.len]
    }

    pub fn b(&self) -> &[u32] {
        &self.b[..self.len]
    }

    #[inline(always)]
    pub fn row(&self, index: usize) -> (u8, u32, u32) {
        (self.kind[index], self.a[index], self.b[index])
    }

    #[inline(always)]
    pub fn set_row(&mut self, index: usize, kind: u8, a: u32, b: u32) {
        debug_assert!(index < self.len);
        self.kind[index] = kind;
        self.a[index] = a;
        self.b[index] = b;
    }

    #[inline(always)]
    pub fn set_b(&mut self, index: usize, b: u32) {
        debug_assert!(index < self.len);
        self.b[index] = b;
    }

    #[inline(always)]
    pub fn set_kind(&mut self, index: usize, kind: u8) {
        debug_assert!(index < self.len);
        self.kind[index] = kind;
    }
}
