/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(unused, clippy::all)]

use js::{Cell, Gc, Visitor};

pub struct Owner {
    cell: Gc<Cell>,
    nested: Vec<Nested>,
}

unsafe impl js::Trace for Owner {
    fn trace(&self, _: &mut Visitor) {}
}

unsafe impl js::gc::class::GcCell for Owner {}

impl Drop for Owner {
    fn drop(&mut self) {
        let copy = self.cell;
        let cell: &Cell = &self.cell; //~ ERROR the Drop of 'Owner'
    }
}

pub struct Nested {
    cell: Gc<Cell>,
}

impl Drop for Nested {
    fn drop(&mut self) {
        self.cell.method(); //~ ERROR the Drop of 'Nested'
    }
}

pub struct NotInACell {
    cell: Gc<Cell>,
}

impl Drop for NotInACell {
    fn drop(&mut self) {
        self.cell.method();
    }
}
