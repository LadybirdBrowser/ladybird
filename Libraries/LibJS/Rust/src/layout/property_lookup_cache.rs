/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::buffer::InterpreterBuffer;
use super::cell::{CellHeader, Gc};
use super::object::Object;
use super::shape::{PrototypeChainValidity, Shape};
use super::value::Value;
use crate::layout_forward::ObjectPropertyIteratorCacheDataStorage;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum PropertyLookupCacheEntryType {
    Empty,
    AddOwnProperty,
    ChangeOwnProperty,
    GetOwnProperty,
    ChangePropertyInPrototypeChain,
    GetPropertyInPrototypeChain,
    GetMissingProperty,
}

/// One cached property access. The shapes and objects are not traced; the VM's sweep callback clears entries whose
/// cells did not survive a collection.
#[repr(C)]
pub struct PropertyLookupCacheEntry {
    pub entry_type: Cell<PropertyLookupCacheEntryType>,
    pub property_offset: Cell<u32>,
    pub shape_dictionary_generation: Cell<u32>,
    pub direct_getter_validated: Cell<bool>,
    pub writes_data_property: Cell<bool>,
    pub from_shape: Cell<Option<Gc<Shape>>>,
    pub shape: Cell<Option<Gc<Shape>>>,
    pub prototype: Cell<Option<Gc<Object>>>,
    pub prototype_chain_validity: Cell<Option<Gc<PrototypeChainValidity>>>,
}

/// A tagged pointer to the entries of a property lookup cache; the interpreter only ever consults the first entry.
#[repr(C)]
pub struct PropertyLookupCache {
    pub data: Cell<usize>,
}

pub const PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK: usize = 3;

#[repr(C)]
pub struct GlobalVariableCache {
    pub entry: PropertyLookupCacheEntry,
    pub environment_serial_number: Cell<u64>,
    pub environment_binding_index: Cell<u32>,
    pub has_environment_binding_index: Cell<bool>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ObjectPropertyIteratorFastPath {
    None,
    PlainNamed,
    PackedIndexed,
}

#[repr(C)]
pub struct ObjectPropertyIteratorCacheData {
    pub header: CellHeader,
    pub shape_is_dictionary: Cell<bool>,
    pub fast_path: Cell<ObjectPropertyIteratorFastPath>,
    pub receiver_has_magical_length: Cell<bool>,
    pub indexed_property_count: Cell<u32>,
    pub shape_dictionary_generation: Cell<u32>,
    pub shape: Cell<Option<Gc<Shape>>>,
    pub prototype_chain_validity: Cell<Option<Gc<PrototypeChainValidity>>>,
    pub property_values: InterpreterBuffer<Value>,
    pub storage: ObjectPropertyIteratorCacheDataStorage,
}

#[repr(C)]
pub struct ObjectPropertyIteratorCache {
    pub data: Cell<Option<Gc<ObjectPropertyIteratorCacheData>>>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct EnvironmentCoordinate {
    pub hops: u32,
    pub index: u32,
}

impl EnvironmentCoordinate {
    pub const INVALID_MARKER: u32 = 0xffff_fffe;
}
