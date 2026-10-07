/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::object::Object;
use super::property_lookup_cache::ObjectPropertyIteratorCacheData;
use super::realm::Realm;
use crate::layout_forward::ShapeStorage;

#[repr(C)]
pub struct Shape {
    pub header: CellHeader,
    pub flags: Cell<u8>,
    pub realm: Cell<Gc<Realm>>,
    pub prototype: Cell<Option<Gc<Object>>>,
    pub prototype_chain_validity: Cell<Option<Gc<PrototypeChainValidity>>>,
    pub property_iterator_cache: Cell<Option<Gc<ObjectPropertyIteratorCacheData>>>,
    pub property_count: Cell<u32>,
    pub dictionary_generation: Cell<u32>,
    pub storage: ShapeStorage,
}

#[repr(C)]
pub struct PrototypeChainValidity {
    pub header: CellHeader,
    pub valid: Cell<bool>,
    /// Keeps the cell at the minimum cell size.
    pub padding: usize,
}
