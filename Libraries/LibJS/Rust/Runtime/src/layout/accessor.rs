/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::function_object::FunctionObject;
use crate::layout_forward::Symbol;

#[repr(C)]
pub struct Accessor {
    pub header: CellHeader,
    pub getter: Cell<Option<Gc<FunctionObject>>>,
    pub setter: Cell<Option<Gc<FunctionObject>>>,
    pub cached_value_key: Cell<Option<Gc<Symbol>>>,
}
