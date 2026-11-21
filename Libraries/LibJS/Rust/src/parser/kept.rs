/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the first parse of a source keeps on the heap, at its final size, for the compile jobs that come later, maybe
//! on other threads.

use ak::Zone;
use libjs_parser::columns::FunctionIndexView;
use libjs_parser::intern::Interner;
use libjs_parser::intern::LongRow;
use libjs_parser::intern::NameTables;
use libjs_parser::intern::NameTablesMut;
use libjs_parser::intern::Row;
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

/// The function index of a source, which its first parse keeps at its final size. See `FunctionIndexView` for the
/// columns.
#[derive(Default)]
pub struct FunctionIndex {
    pub start: Box<[u32]>,
    pub params_start: Box<[u32]>,
    pub end: Box<[u32]>,
    pub flags: Box<[u32]>,
    pub length: Box<[u32]>,
    pub parameter_count: Box<[u32]>,
    pub ctx: Box<[u16]>,
    pub name: Box<[u32]>,
    pub capture_start: Box<[u32]>,
    pub capture_count: Box<[u32]>,
    pub capture_names: Box<[u32]>,
    pub rows_by_params_start: Box<[u32]>,
    pub summaries: Box<[u8]>,
    pub summary_start: Box<[u32]>,
    pub declared_names: Box<[u64]>,
    pub program_summary_end: u32,
    pub program_has_direct_eval: bool,
}

impl FunctionIndex {
    /// A copy of `index`.
    pub(crate) fn of(index: &FunctionIndexView<'_>) -> Self {
        Self {
            start: index.start.into(),
            params_start: index.params_start.into(),
            end: index.end.into(),
            flags: index.flags.into(),
            length: index.length.into(),
            parameter_count: index.parameter_count.into(),
            ctx: index.ctx.into(),
            name: index.name.into(),
            capture_start: index.capture_start.into(),
            capture_count: index.capture_count.into(),
            capture_names: index.capture_names.into(),
            rows_by_params_start: index.rows_by_params_start.into(),
            summaries: index.summaries.into(),
            summary_start: index.summary_start.into(),
            declared_names: index.declared_names.into(),
            program_summary_end: index.program_summary_end,
            program_has_direct_eval: index.program_has_direct_eval,
        }
    }

    #[inline(always)]
    pub(crate) fn view(&self) -> FunctionIndexView<'_> {
        FunctionIndexView {
            start: &self.start,
            params_start: &self.params_start,
            end: &self.end,
            flags: &self.flags,
            length: &self.length,
            parameter_count: &self.parameter_count,
            ctx: &self.ctx,
            name: &self.name,
            capture_start: &self.capture_start,
            capture_count: &self.capture_count,
            capture_names: &self.capture_names,
            rows_by_params_start: &self.rows_by_params_start,
            summaries: &self.summaries,
            summary_start: &self.summary_start,
            declared_names: &self.declared_names,
            program_summary_end: self.program_summary_end,
            program_has_direct_eval: self.program_has_direct_eval,
        }
    }

    /// Whether anything in the source declares `name`.
    #[inline(always)]
    pub fn is_declared_anywhere(&self, name: u32) -> bool {
        self.view().is_declared_anywhere(name)
    }

    /// Where the summary of the function at `row` is in the pool.
    #[inline(always)]
    pub fn summary_range(&self, row: u32) -> (usize, usize) {
        self.view().summary_range(row)
    }

    pub fn memory_size(&self) -> usize {
        let u32_columns = [
            &self.start,
            &self.params_start,
            &self.end,
            &self.flags,
            &self.length,
            &self.parameter_count,
            &self.name,
            &self.capture_start,
            &self.capture_count,
            &self.capture_names,
            &self.rows_by_params_start,
            &self.summary_start,
        ];
        u32_columns.iter().map(|column| column.len()).sum::<usize>() * size_of::<u32>()
            + self.ctx.len() * size_of::<u16>()
            + self.summaries.len()
            + self.declared_names.len() * size_of::<u64>()
    }
}
