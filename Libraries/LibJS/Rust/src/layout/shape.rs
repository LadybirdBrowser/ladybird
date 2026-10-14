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
use crate::layout_forward::{ShapeStorage, WatchpointSetStorage};

#[repr(C)]
pub struct Shape {
    pub header: CellHeader,
    pub flags: Cell<u8>,
    pub realm: Cell<Gc<Realm>>,
    pub prototype: Cell<Option<Gc<Object>>>,
    pub prototype_chain_validity: Cell<Option<Gc<WatchpointSet>>>,
    pub property_iterator_cache: Cell<Option<Gc<ObjectPropertyIteratorCacheData>>>,
    pub property_count: Cell<u32>,
    pub dictionary_generation: Cell<u32>,
    pub storage: ShapeStorage,
}

/// Something the runtime and JIT code may rely on until it stops holding, when the set fires once and stays invalid.
/// The shape of each prototype has one, which fires when the properties or the prototype of that object or of one
/// above it on its prototype chain change (see `Shape::prototype_chain_validity`).
#[repr(C)]
pub struct WatchpointSet {
    pub header: CellHeader,
    pub valid: Cell<bool>,
    pub storage: WatchpointSetStorage,
}
