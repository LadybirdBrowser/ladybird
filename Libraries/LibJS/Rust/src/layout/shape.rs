/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::member::NullableMember;
use super::object::Object;
use super::property_lookup_cache::ObjectPropertyIteratorCacheData;
use super::realm::Realm;
use crate::layout_forward::{ShapeStorage, SpecialPropertyCache, WatchpointSetStorage};

#[repr(C)]
pub struct Shape {
    pub header: CellHeader,
    pub flags: Cell<u8>,
    pub realm: Cell<Gc<Realm>>,
    pub prototype: Cell<Option<Gc<Object>>>,
    // NB: The interpreter does not read these four, which fill the word pair before the fields it reads after them.
    pub prototype_chain_validity: NullableMember<WatchpointSet>,
    pub property_iterator_cache: NullableMember<ObjectPropertyIteratorCacheData>,
    /// The shape this one transitioned from, unless it changed the prototype. Transition caches only hold their
    /// targets weakly, so every shape keeps its parent alive (like V8's back pointers and JSC's previous structure): as
    /// long as any object has a shape, the whole chain that leads to it stays cached and the next object built the same
    /// way gets the same shape, also after a garbage collection.
    pub previous: NullableMember<Shape>,
    /// What Get of the properties that conversions look up gives on the objects of this shape (see
    /// `SpecialPropertyCache`).
    pub special_property_cache: NullableMember<SpecialPropertyCache>,
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
