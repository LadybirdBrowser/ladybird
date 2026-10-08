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
    /// The encoded string or symbol Value whose property this entry describes, for the caches of keyed accesses like
    /// GetByValue, or 0 for the caches of named accesses like GetById, whose instruction determines the property. Like
    /// the cells above, the key is not kept alive by the cache.
    pub key: Cell<u64>,
}

/// The layout of an entry of the VM's keyed property lookup cache (KeyedPropertyLookupCacheEntry of the bytecode
/// executable, which asserts that it matches), for the interpreter, which looks up own data properties in it.
#[repr(C, align(64))]
pub struct KeyedPropertyLookupCacheEntryLayout {
    pub entry_type: PropertyLookupCacheEntryType,
    pub property_offset: u32,
    pub shape_dictionary_generation: u32,
    pub shape: Option<Gc<Shape>>,
    pub prototype: Option<Gc<Object>>,
    pub prototype_chain_validity: Option<Gc<PrototypeChainValidity>>,
    /// The identity of the name of the property, the word of a fly string, or 0.
    pub property_name: u64,
}

const _: () = assert!(size_of::<KeyedPropertyLookupCacheEntryLayout>() == 64);

/// How the interpreter finds the entry of the keyed property lookup cache that may hold a lookup: the top this many bits
/// of the 32-bit product of this multiplier and the low 32 bits of the shape's address XORed with the name's identity
/// (see KeyedPropertyLookupCache::entry_index_for() of the bytecode executable, which asserts that these match).
pub const KEYED_PROPERTY_LOOKUP_CACHE_INTERPRETER_INDEX_BITS: u32 = 11;
pub const KEYED_PROPERTY_LOOKUP_CACHE_INTERPRETER_HASH_MULTIPLIER: u32 = 0x9e37_79b9;

/// A tagged pointer to the entries of a property lookup cache; the interpreter only ever consults the first entry.
#[repr(C)]
pub struct PropertyLookupCache {
    pub data: Cell<usize>,
}

pub const PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK: usize = 3;

/// The tag of a polymorphic cache's data, whose entries are an array the interpreter probes.
pub const PROPERTY_LOOKUP_CACHE_POLYMORPHIC_DATA_TAG: usize = 1;

/// The data of a keyed generic cache: the tag without data (see PropertyLookupCache::is_keyed_generic()).
pub const PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA: usize = 3;

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
