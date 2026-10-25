/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Mirrors Tests/ClangPlugins/LibJSGCTests/strong_root_fields_in_gc_allocated_types.cpp.

#![allow(unused, clippy::all)]

use js::{Cell, Gc, MarkedVec, Root, Trace, Visitor};

pub struct CellClass {
    root: Root<'static, Gc<Cell>>, //~ ERROR 'CellClass' implements Trace but holds the root 'Root<'static, Gc<Cell>>'
    marked: MarkedVec<'static, Gc<Cell>>, //~ ERROR holds the root 'MarkedVec<'static, Gc<Cell>>'
    nested: Option<Vec<Root<'static, Gc<Cell>>>>, //~ ERROR holds the root 'Root<'_, Gc<Cell>>'
    cell: Gc<Cell>,
}

unsafe impl Trace for CellClass {
    fn trace(&self, _: &mut Visitor) {}
}

pub enum TracedEnum {
    Rooted(Root<'static, Gc<Cell>>), //~ ERROR 'TracedEnum' implements Trace but holds the root
    Plain(Gc<Cell>),
}

unsafe impl Trace for TracedEnum {
    fn trace(&self, _: &mut Visitor) {}
}

pub struct NonTracedClass {
    root: Root<'static, Gc<Cell>>,
}

pub struct SuppressedClass {
    #[cfg_attr(ladybird_gc_lints, expect(root_in_traced_type, reason = "the root is cleared before the cell dies"))]
    root: Root<'static, Gc<Cell>>,
}

unsafe impl Trace for SuppressedClass {
    fn trace(&self, _: &mut Visitor) {}
}
