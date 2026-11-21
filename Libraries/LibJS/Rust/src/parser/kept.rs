/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the first parse of a source keeps on the heap, at its final size, for the compile jobs that come later, maybe
//! on other threads.

use super::intern::Interner;
use super::intern::LongRow;
use super::intern::NameTables;
use super::intern::NameTablesMut;
use super::intern::Row;
use ak::Zone;
use std::sync::OnceLock;

/// The tables of an interner on the heap, at the size they have, which the reparse context of a source keeps.
pub(crate) struct KeptNames {
    rows: Box<[Row]>,
    shift: u32,
    count: u32,
    long_rows: Box<[LongRow]>,
    long_count: u32,
    text: Box<[u8]>,
    spans: Box<[(u32, u32)]>,
}

impl KeptNames {
    /// A copy of `tables`.
    pub(crate) fn of(tables: &NameTables<'_>) -> Self {
        Self {
            rows: tables.rows.into(),
            shift: tables.shift,
            count: tables.count,
            long_rows: tables.long_rows.into(),
            long_count: tables.long_count,
            text: tables.text.into(),
            spans: tables.spans.into(),
        }
    }

    /// The tables of an interner with the names that every interner starts with, which are worked out in `zone` once.
    pub(crate) fn initial(zone: &Zone) -> &'static Self {
        static INITIAL: OnceLock<KeptNames> = OnceLock::new();
        INITIAL.get_or_init(|| Self::of(&Interner::initial(zone).tables()))
    }

    pub(crate) fn len(&self) -> usize {
        self.spans.len()
    }

    pub(crate) fn tables(&self) -> NameTables<'_> {
        NameTables {
            rows: &self.rows,
            shift: self.shift,
            count: self.count,
            long_rows: &self.long_rows,
            long_count: self.long_count,
            text: &self.text,
            spans: &self.spans,
        }
    }

    pub(crate) fn tables_mut(&mut self) -> NameTablesMut<'_> {
        NameTablesMut {
            rows: &mut self.rows,
            shift: self.shift,
            count: self.count,
            long_rows: &mut self.long_rows,
            long_count: self.long_count,
            text: &mut self.text,
            spans: &mut self.spans,
        }
    }
}
