/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::function_object::FunctionObject;
use super::member::NullableMember;
use crate::layout_forward::Symbol;

#[repr(C)]
pub struct Accessor {
    pub header: CellHeader,
    /// NB: The fields the interpreter does not read come first, in the bytes after the header.
    pub cache_on_receiver: Cell<bool>,
    pub cached_value_key: NullableMember<Symbol>,
    pub getter: Cell<Option<Gc<FunctionObject>>>,
    pub setter: Cell<Option<Gc<FunctionObject>>>,
}

const _: () = assert!(size_of::<Accessor>() == 32);
