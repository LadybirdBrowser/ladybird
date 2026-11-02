/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, CellHeaderTail};
use super::member::{Member, NullableMember};
use super::object::Object;
use super::realm::Realm;
use crate::layout_forward::{DescriptorArray, ShapeStorage, WatchpointSetStorage};

/// A shape starts with the realm and the prototype in the first word of its cell header, which LibGC leaves to the
/// cell, followed by the rest of the header.
#[repr(C)]
pub struct Shape {
    pub realm: Member<Realm>,
    pub prototype: NullableMember<Object>,
    pub header: CellHeaderTail,
    pub flags: Cell<u8>,
    pub property_count: Cell<u32>,
    pub prototype_chain_validity: NullableMember<WatchpointSet>,
    /// The shape this one transitioned from, unless it changed the prototype. Transition caches only hold their
    /// targets weakly, so every shape keeps its parent alive: as long as any object has a shape, the whole chain that
    /// leads to it stays cached and the next object built the same way gets the same shape, also after a garbage
    /// collection.
    pub previous: NullableMember<Shape>,
    pub dictionary_generation: Cell<u32>,
    /// The descriptor array, or the DictionaryPropertyTable of a dictionary shape.
    pub property_storage: NullableMember<DescriptorArray>,
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
