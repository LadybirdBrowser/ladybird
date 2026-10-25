/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#![allow(unused, clippy::all)]

use js::{Cell, Gc, GcRefCell, GcWeak, Value, Visitor};
use libjs_runtime_macros::Trace;

#[derive(Trace)]
pub struct Traced {
    cells: GcRefCell<Vec<Gc<Cell>>>,
    value: GcRefCell<Option<Value>>,
    counts: GcRefCell<Vec<u32>>, //~ ERROR field of type 'GcRefCell<Vec<u32>>' holds no GC pointers; use a RefCell
    weak: GcRefCell<Vec<GcWeak<Cell>>>, //~ ERROR field of type 'GcRefCell<Vec<GcWeak<Cell>>>' holds no GC pointers; use a RefCell
    #[gc(untraced)]
    name: GcRefCell<String>, //~ ERROR field of type 'GcRefCell<String>' is untraced; use a RefCell
    #[gc(untraced = "the owner traces it")]
    owned: GcRefCell<Gc<Cell>>, //~ ERROR field of type 'GcRefCell<Gc<Cell>>' is untraced; use a RefCell
    plain: core::cell::RefCell<Vec<u32>>,
    tagged: GcRefCell<Vec<Tagged>>,
}

unsafe impl js::Trace for Traced {
    fn trace(&self, _: &mut Visitor) {}
}

/// A cell pointer in a tagged word, which only its hand-written trace() knows about.
pub struct Tagged(usize);

unsafe impl js::Trace for Tagged {
    fn trace(&self, _: &mut Visitor) {}
}

pub struct Generic<T> {
    state: GcRefCell<T>,
}
