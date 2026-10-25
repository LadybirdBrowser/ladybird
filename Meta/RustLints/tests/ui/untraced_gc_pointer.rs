/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(unused, clippy::all)]

use js::{Cell, Gc, GcWeak, Value, Visitor};
use libjs_runtime_macros::Trace;

#[derive(Trace)]
pub struct Traced {
    #[gc(untraced)]
    count: u32,
    #[gc(untraced)]
    weak: GcWeak<Cell>,
    #[gc(untraced)]
    cell: Gc<Cell>, //~ ERROR untraced field of type 'Gc<Cell>'
    #[gc(untraced)]
    values: Vec<Value>, //~ ERROR untraced field of type 'Vec<Value>'
    #[gc(untraced = "the owner traces it")]
    explained: Gc<Cell>,
    traced: Gc<Cell>,
}

unsafe impl js::Trace for Traced {
    fn trace(&self, _: &mut Visitor) {}
}

#[derive(Trace)]
pub enum Either {
    Cell(#[gc(untraced)] Gc<Cell>), //~ ERROR untraced field of type 'Gc<Cell>'
    Count(#[gc(untraced)] u32),
}

unsafe impl js::Trace for Either {
    fn trace(&self, _: &mut Visitor) {}
}
