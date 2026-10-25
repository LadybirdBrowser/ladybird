/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(unused, clippy::all)]

use std::rc::Rc;

use js::{Cell, DeferGc, Deferred, Gc, GcWeak, MarkedVec, Value, Visitor};

pub struct Untraced {
    cells: Vec<Gc<Cell>>, //~ ERROR 'Untraced' does not implement Trace but keeps GC pointers in 'Vec<Gc<Cell>>'
}

pub struct Shared {
    values: Rc<[Value]>, //~ ERROR 'Shared' does not implement Trace but keeps GC pointers in 'Rc<[Value]>'
}

pub enum Choice {
    Cells(Box<[Gc<Cell>]>), //~ ERROR 'Choice' does not implement Trace but keeps GC pointers in 'Box<[Gc<Cell>]>'
    Nothing,
}

pub struct Direct {
    cell: Gc<Cell>,
    weak: Vec<GcWeak<Cell>>,
}

pub struct Rooted<'vm> {
    cells: MarkedVec<'vm, Gc<Cell>>,
}

pub struct Traced {
    cells: Vec<Gc<Cell>>,
}

unsafe impl js::Trace for Traced {
    fn trace(&self, _: &mut Visitor) {}
}

pub struct TiedToADeferral<'d> {
    defer_gc: &'d DeferGc<'d>,
    cells: Vec<Gc<Cell>>,
}

pub struct HoldingADeferred<'d> {
    covered: Deferred<'d, Vec<Gc<Cell>>>,
    cells: Vec<Gc<Cell>>,
}

pub fn tied_locals(defer_gc: &DeferGc<'_>, vm: &js::Vm, cell: Gc<Cell>) {
    let tied = TiedToADeferral {
        defer_gc,
        cells: vec![cell],
    };
}
