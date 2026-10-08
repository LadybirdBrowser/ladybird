/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::io::Write;
use std::rc::Rc;

use ak::Utf16FlyString;

use crate::breakpoint::BreakpointID;
use crate::bytecode::basic_block::SourceMapEntry;
use crate::bytecode::class_blueprint::ClassBlueprint;
use crate::bytecode::constant::{AbstractOperationKind, WellKnownSymbolKind};
use crate::bytecode::encoding::{IdentifierTableIndex, PropertyKeyTableIndex, StringTableIndex};
use crate::bytecode::executable_data::ExecutableCacheCounts as FrontendExecutableCacheCounts;
use crate::bytecode::executable_data::ExecutableData;
use crate::bytecode::generator::{ConstantValue, ExceptionHandler};
use crate::bytecode::generator::{LocalVariable, PendingClassBlueprint};
use crate::bytecode_cache::{DecodedBytecodeBytes, DecodedExecutableRecord};
use crate::frontend_host::{count_bytecode_basic_blocks, dump_bytecode, rust_free_compiled_regex};
use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::heap::cell_is_dead;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::executable::ExecutableHead;
use crate::layout::object::Object;
pub use crate::layout::property_lookup_cache::{
    EnvironmentCoordinate, GlobalVariableCache, ObjectPropertyIteratorCache, ObjectPropertyIteratorCacheData,
    ObjectPropertyIteratorFastPath, PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK, PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA,
    PropertyLookupCache, PropertyLookupCacheEntry, PropertyLookupCacheEntryType,
};
use crate::layout::shape::{PrototypeChainValidity, Shape};
use crate::layout::value::Value;
use crate::layout_forward::FlyStringSlot;
use crate::runtime::array::Array;
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::environment_shape::{EnvironmentShape, EnvironmentShapeCache};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::primitive_string::u64_hash;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::runtime::value::number_to_string;
use crate::source_code::SourceCode;
use crate::source_range::{Position, SourceRange};
use crate::utf16::Utf16View;
use libjs_runtime_macros::Trace;

/// The contents of a cache entry, copied out of the cache so that no reference into its storage outlives a call that
/// may update the cache and free that storage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct PropertyLookupCacheEntryData {
    pub entry_type: PropertyLookupCacheEntryType,
    pub property_offset: u32,
    pub shape_dictionary_generation: u32,
    pub direct_getter_validated: bool,
    pub writes_data_property: bool,
    pub from_shape: Option<Gc<Shape>>,
    pub shape: Option<Gc<Shape>>,
    pub prototype: Option<Gc<Object>>,
    pub prototype_chain_validity: Option<Gc<PrototypeChainValidity>>,
    /// See PropertyLookupCacheEntry::key.
    pub key: u64,
}

impl Default for PropertyLookupCacheEntryData {
    fn default() -> Self {
        Self {
            entry_type: PropertyLookupCacheEntryType::Empty,
            property_offset: 0,
            shape_dictionary_generation: 0,
            direct_getter_validated: false,
            writes_data_property: false,
            from_shape: None,
            shape: None,
            prototype: None,
            prototype_chain_validity: None,
            key: 0,
        }
    }
}

impl PropertyLookupCacheEntryData {
    /// The shape an entry is looked up by: the shape before the addition for AddOwnProperty, the receiver's shape
    /// otherwise.
    pub fn lookup_shape(&self) -> Option<Gc<Shape>> {
        if self.entry_type == PropertyLookupCacheEntryType::AddOwnProperty {
            return self.from_shape;
        }
        self.shape
    }

    fn has_same_cache_key_as(&self, other: &Self) -> bool {
        if self.entry_type == PropertyLookupCacheEntryType::Empty
            || other.entry_type == PropertyLookupCacheEntryType::Empty
        {
            return false;
        }
        if self.entry_type != other.entry_type || self.key != other.key {
            return false;
        }

        match self.entry_type {
            PropertyLookupCacheEntryType::AddOwnProperty => {
                self.from_shape == other.from_shape && self.shape == other.shape
            }
            PropertyLookupCacheEntryType::ChangeOwnProperty
            | PropertyLookupCacheEntryType::GetOwnProperty
            | PropertyLookupCacheEntryType::GetMissingProperty => self.shape == other.shape,
            PropertyLookupCacheEntryType::ChangePropertyInPrototypeChain
            | PropertyLookupCacheEntryType::GetPropertyInPrototypeChain => {
                self.shape == other.shape && self.prototype == other.prototype
            }
            PropertyLookupCacheEntryType::Empty => unreachable!("empty entries have no cache key"),
        }
    }

    fn has_dead_cell(&self) -> bool {
        (self.key != 0 && cell_is_dead(Value(self.key).as_cell()))
            || self.from_shape.is_some_and(cell_is_dead)
            || self.shape.is_some_and(cell_is_dead)
            || self.prototype.is_some_and(cell_is_dead)
            || self.prototype_chain_validity.is_some_and(cell_is_dead)
    }
}

impl PropertyLookupCacheEntry {
    pub fn new() -> Self {
        Self::from_data(PropertyLookupCacheEntryData::default())
    }

    fn from_data(data: PropertyLookupCacheEntryData) -> Self {
        Self {
            entry_type: Cell::new(data.entry_type),
            property_offset: Cell::new(data.property_offset),
            shape_dictionary_generation: Cell::new(data.shape_dictionary_generation),
            direct_getter_validated: Cell::new(data.direct_getter_validated),
            writes_data_property: Cell::new(data.writes_data_property),
            from_shape: Cell::new(data.from_shape),
            shape: Cell::new(data.shape),
            prototype: Cell::new(data.prototype),
            prototype_chain_validity: Cell::new(data.prototype_chain_validity),
            key: Cell::new(data.key),
        }
    }

    pub fn get(&self) -> PropertyLookupCacheEntryData {
        PropertyLookupCacheEntryData {
            entry_type: self.entry_type.get(),
            property_offset: self.property_offset.get(),
            shape_dictionary_generation: self.shape_dictionary_generation.get(),
            direct_getter_validated: self.direct_getter_validated.get(),
            writes_data_property: self.writes_data_property.get(),
            from_shape: self.from_shape.get(),
            shape: self.shape.get(),
            prototype: self.prototype.get(),
            prototype_chain_validity: self.prototype_chain_validity.get(),
            key: self.key.get(),
        }
    }

    pub fn set(&self, data: PropertyLookupCacheEntryData) {
        self.entry_type.set(data.entry_type);
        self.property_offset.set(data.property_offset);
        self.shape_dictionary_generation.set(data.shape_dictionary_generation);
        self.direct_getter_validated.set(data.direct_getter_validated);
        self.writes_data_property.set(data.writes_data_property);
        self.from_shape.set(data.from_shape);
        self.shape.set(data.shape);
        self.prototype.set(data.prototype);
        self.prototype_chain_validity.set(data.prototype_chain_validity);
        self.key.set(data.key);
    }

    /// Forgets the whole entry if one of its cells died in this collection, as clear_cache_entry_if_dead does.
    fn clear_if_it_has_a_dead_cell(&self) {
        if self.get().has_dead_cell() {
            self.set(PropertyLookupCacheEntryData::default());
        }
    }
}

impl Default for PropertyLookupCacheEntry {
    fn default() -> Self {
        Self::new()
    }
}

pub const MAX_NUMBER_OF_SHAPES_TO_REMEMBER: usize = 4;
pub const MEGAMORPHIC_PRIMARY_CACHE_SIZE: usize = 64;
pub const MEGAMORPHIC_SECONDARY_CACHE_SIZE: usize = 64;
const POLYMORPHIC_DATA_TAG: usize = 1;
const MEGAMORPHIC_DATA_TAG: usize = 2;
/// How many (shape, key) pairs a megamorphic cache of a keyed access may miss and learn before it gives up (see
/// PropertyLookupCache::is_keyed_generic()).
const MAX_KEYED_MEGAMORPHIC_MISSES: u32 = 1024;

const _: () = assert!(MEGAMORPHIC_PRIMARY_CACHE_SIZE.is_power_of_two());
const _: () = assert!(MEGAMORPHIC_SECONDARY_CACHE_SIZE.is_power_of_two());

// The interpreter reads the first entry at the start of each tier's data.
#[repr(C)]
struct MonomorphicData {
    entry: PropertyLookupCacheEntry,
}

#[repr(C)]
struct PolymorphicData {
    entries: [PropertyLookupCacheEntry; MAX_NUMBER_OF_SHAPES_TO_REMEMBER],
}

// Keep the most recently used entry first so generated interpreter code can use the
// same fast path for every cache tier. Other shapes use the bounded two-level cache.
#[repr(C)]
struct MegamorphicData {
    entry: PropertyLookupCacheEntry,
    primary_entries: [PropertyLookupCacheEntry; MEGAMORPHIC_PRIMARY_CACHE_SIZE],
    secondary_entries: [PropertyLookupCacheEntry; MEGAMORPHIC_SECONDARY_CACHE_SIZE],
    /// Misses of a keyed access (see MAX_KEYED_MEGAMORPHIC_MISSES).
    keyed_misses: Cell<u32>,
}

const _: () = assert!(align_of::<MonomorphicData>() > PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
const _: () = assert!(align_of::<PolymorphicData>() > PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
const _: () = assert!(align_of::<MegamorphicData>() > PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
const _: () = assert!(core::mem::offset_of!(MonomorphicData, entry) == 0);
const _: () = assert!(core::mem::offset_of!(PolymorphicData, entries) == 0);
const _: () = assert!(core::mem::offset_of!(MegamorphicData, entry) == 0);

fn megamorphic_hash(shape: Gc<Shape>, key: u64) -> usize {
    let hash = u64_hash(shape.as_ptr().addr() as u64);
    if key == 0 {
        return hash as usize;
    }
    pair_int_hash(hash, u64_hash(key)) as usize
}

fn megamorphic_primary_index(shape: Gc<Shape>, key: u64) -> usize {
    megamorphic_hash(shape, key) & (MEGAMORPHIC_PRIMARY_CACHE_SIZE - 1)
}

fn megamorphic_secondary_index(shape: Gc<Shape>, key: u64) -> usize {
    (megamorphic_hash(shape, key) >> 8) & (MEGAMORPHIC_SECONDARY_CACHE_SIZE - 1)
}

fn insert_megamorphic_entry(data: &MegamorphicData, entry: &PropertyLookupCacheEntryData) {
    let lookup_shape = entry.lookup_shape().expect("a megamorphic entry has a lookup shape");

    let primary_entry = &data.primary_entries[megamorphic_primary_index(lookup_shape, entry.key)];
    let primary = primary_entry.get();
    if let Some(displaced_shape) = primary.lookup_shape()
        && (displaced_shape != lookup_shape || primary.key != entry.key)
    {
        data.secondary_entries[megamorphic_secondary_index(displaced_shape, primary.key)].set(primary);
    }
    primary_entry.set(*entry);
}

/// The entries of a cache that may hold a shape, copied out by PropertyLookupCache::entries_for_shape().
pub struct PropertyLookupCacheEntries {
    entries: [PropertyLookupCacheEntryData; MAX_NUMBER_OF_SHAPES_TO_REMEMBER],
    count: usize,
}

impl PropertyLookupCacheEntries {
    pub fn as_slice(&self) -> &[PropertyLookupCacheEntryData] {
        &self.entries[..self.count]
    }
}

// Represents one tiered inline cache used for property lookups. The tier is in the low bits of the data pointer.
impl PropertyLookupCache {
    pub fn new() -> Self {
        Self { data: Cell::new(0) }
    }

    fn data_pointer<T>(&self, tag: usize) -> Option<&T> {
        let data = self.data.get();
        if data == 0 || data & PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK != tag {
            return None;
        }
        let pointer = core::ptr::with_exposed_provenance::<T>(data & !PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
        // SAFETY: The tagged pointer came from Box::into_raw of a T with this tag, and the cache owns it until clear().
        Some(unsafe { &*pointer })
    }

    fn monomorphic_data(&self) -> Option<&MonomorphicData> {
        self.data_pointer(0)
    }

    fn polymorphic_data(&self) -> Option<&PolymorphicData> {
        self.data_pointer(POLYMORPHIC_DATA_TAG)
    }

    fn megamorphic_data(&self) -> Option<&MegamorphicData> {
        self.data_pointer(MEGAMORPHIC_DATA_TAG)
    }

    fn set_data<T>(&self, data: Box<T>, tag: usize) {
        let address = Box::into_raw(data).expose_provenance();
        assert!(address & PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK == 0);
        self.data.set(address | tag);
    }

    /// Whether this is the cache of a keyed access that saw too many (shape, key) pairs to keep track of. Such a cache
    /// stays empty, so its accesses go straight to their slow paths.
    pub fn is_keyed_generic(&self) -> bool {
        self.data.get() == PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA
    }

    pub fn first_entry(&self) -> Option<PropertyLookupCacheEntryData> {
        self.first_entry_slot().map(PropertyLookupCacheEntry::get)
    }

    /// The entry the interpreter consults, for updating it in place. The reference must not be held across anything
    /// that may update the cache.
    pub fn first_entry_slot(&self) -> Option<&PropertyLookupCacheEntry> {
        if let Some(data) = self.monomorphic_data() {
            return Some(&data.entry);
        }
        if let Some(data) = self.polymorphic_data() {
            return Some(&data.entries[0]);
        }
        if let Some(data) = self.megamorphic_data() {
            return Some(&data.entry);
        }
        None
    }

    fn entries(&self) -> &[PropertyLookupCacheEntry] {
        if let Some(data) = self.monomorphic_data() {
            return core::slice::from_ref(&data.entry);
        }
        if let Some(data) = self.polymorphic_data() {
            return &data.entries;
        }
        if let Some(data) = self.megamorphic_data() {
            return core::slice::from_ref(&data.entry);
        }
        &[]
    }

    fn copy_entries(entries: &[PropertyLookupCacheEntry]) -> PropertyLookupCacheEntries {
        let mut copied = PropertyLookupCacheEntries {
            entries: [PropertyLookupCacheEntryData::default(); MAX_NUMBER_OF_SHAPES_TO_REMEMBER],
            count: entries.len(),
        };
        for (copy, entry) in copied.entries.iter_mut().zip(entries) {
            *copy = entry.get();
        }
        copied
    }

    /// The entries that may be for `shape` and `key` (see PropertyLookupCacheEntry::key; 0 for named accesses).
    /// Callers check each entry's shape and key. A megamorphic cache finds the one for the shape and key and moves it
    /// first, where the interpreter looks.
    pub fn entries_for_shape(&self, shape: Gc<Shape>, key: u64) -> PropertyLookupCacheEntries {
        Self::copy_entries(self.entry_slots_for_shape(shape, key))
    }

    /// entries_for_shape() in place, for the cache-only fast paths. The entries must not be held across anything that
    /// may update the cache.
    #[inline]
    pub fn entry_slots_for_shape(&self, shape: Gc<Shape>, key: u64) -> &[PropertyLookupCacheEntry] {
        let Some(data) = self.megamorphic_data() else {
            return self.entries();
        };

        let find_entry = |entries: &[PropertyLookupCacheEntry], index: usize| {
            let entry = entries[index].get();
            (entry.lookup_shape() == Some(shape) && entry.key == key).then_some(entry)
        };

        let entry = find_entry(&data.primary_entries, megamorphic_primary_index(shape, key))
            .or_else(|| find_entry(&data.secondary_entries, megamorphic_secondary_index(shape, key)));
        let Some(entry) = entry else {
            return &[];
        };

        data.entry.set(entry);
        core::slice::from_ref(&data.entry)
    }

    /// Records an entry of `entry_type`, filled in by `callback`, moving the cache to the next tier when it has no
    /// room for it. Keyed sites also refresh entries after hits in the VM cache. Keep this out of line instead of
    /// growing the frames of the property access paths that every access runs through.
    #[inline(never)]
    pub fn update(
        &self,
        entry_type: PropertyLookupCacheEntryType,
        callback: impl FnOnce(&mut PropertyLookupCacheEntryData),
    ) {
        if self.is_keyed_generic() {
            return;
        }

        let mut new_entry = PropertyLookupCacheEntryData {
            entry_type,
            ..Default::default()
        };
        callback(&mut new_entry);

        if let Some(data) = self.megamorphic_data() {
            // NB: A keyed access that keeps missing cycles through more keys than the cache can hold, and filling it
            //     would cost more than it saves.
            if new_entry.key != 0 {
                let lookup_shape = new_entry
                    .lookup_shape()
                    .expect("a megamorphic entry has a lookup shape");
                let is_known = |entry: &PropertyLookupCacheEntry| {
                    let entry = entry.get();
                    entry.lookup_shape() == Some(lookup_shape) && entry.key == new_entry.key
                };
                let known = is_known(&data.primary_entries[megamorphic_primary_index(lookup_shape, new_entry.key)])
                    || is_known(&data.secondary_entries[megamorphic_secondary_index(lookup_shape, new_entry.key)]);
                if !known {
                    data.keyed_misses.set(data.keyed_misses.get() + 1);
                }
                if data.keyed_misses.get() > MAX_KEYED_MEGAMORPHIC_MISSES {
                    self.clear();
                    self.data.set(PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA);
                    return;
                }
            }
            insert_megamorphic_entry(data, &new_entry);
            data.entry.set(new_entry);
            return;
        }

        if self.data.get() == 0 {
            self.set_data(
                Box::new(MonomorphicData {
                    entry: PropertyLookupCacheEntry::from_data(new_entry),
                }),
                0,
            );
            return;
        }

        if let Some(data) = self.monomorphic_data() {
            let old_entry = data.entry.get();
            if old_entry.has_same_cache_key_as(&new_entry) {
                data.entry.set(new_entry);
                return;
            }

            let new_data = Box::new(PolymorphicData {
                entries: core::array::from_fn(|_| PropertyLookupCacheEntry::new()),
            });
            new_data.entries[0].set(new_entry);
            new_data.entries[1].set(old_entry);
            self.clear();
            self.set_data(new_data, POLYMORPHIC_DATA_TAG);
            return;
        }

        let entries = &self
            .polymorphic_data()
            .expect("a cache with data is in one of the tiers")
            .entries;
        let mut insertion_index = entries
            .iter()
            .position(|entry| entry.get().has_same_cache_key_as(&new_entry))
            .unwrap_or(entries.len());

        if insertion_index == entries.len()
            && entries[entries.len() - 1].entry_type.get() != PropertyLookupCacheEntryType::Empty
        {
            self.move_full_polymorphic_entries_to_megamorphic_tier(&new_entry);
            return;
        }

        if insertion_index == entries.len() {
            insertion_index = entries.len() - 1;
        }

        for index in (1..=insertion_index).rev() {
            entries[index].set(entries[index - 1].get());
        }
        entries[0].set(new_entry);
    }

    /// Boxing a MegamorphicData builds it on the stack first, in a frame of about 12 KB that is probed page by page on
    /// every entry to the function holding it, so this stays out of line from update(), which runs on every miss.
    #[cold]
    #[inline(never)]
    fn move_full_polymorphic_entries_to_megamorphic_tier(&self, new_entry: &PropertyLookupCacheEntryData) {
        let entries = &self
            .polymorphic_data()
            .expect("only a polymorphic cache becomes megamorphic")
            .entries;
        let new_data = Box::new(MegamorphicData {
            entry: PropertyLookupCacheEntry::new(),
            primary_entries: core::array::from_fn(|_| PropertyLookupCacheEntry::new()),
            secondary_entries: core::array::from_fn(|_| PropertyLookupCacheEntry::new()),
            keyed_misses: Cell::new(0),
        });
        for entry in entries.iter().rev() {
            let entry = entry.get();
            if entry.lookup_shape().is_some() {
                insert_megamorphic_entry(&new_data, &entry);
            }
        }
        insert_megamorphic_entry(&new_data, new_entry);
        new_data.entry.set(*new_entry);
        self.clear();
        self.set_data(new_data, MEGAMORPHIC_DATA_TAG);
    }

    pub fn clear(&self) {
        let data = self.data.get();
        if data == 0 || data == PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA {
            self.data.set(0);
            return;
        }
        let address = data & !PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK;
        // SAFETY: The tagged pointer came from Box::into_raw of the tier its tag names.
        unsafe {
            match data & PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK {
                0 => drop(Box::from_raw(
                    core::ptr::with_exposed_provenance_mut::<MonomorphicData>(address),
                )),
                POLYMORPHIC_DATA_TAG => {
                    drop(Box::from_raw(
                        core::ptr::with_exposed_provenance_mut::<PolymorphicData>(address),
                    ));
                }
                MEGAMORPHIC_DATA_TAG => {
                    drop(Box::from_raw(
                        core::ptr::with_exposed_provenance_mut::<MegamorphicData>(address),
                    ));
                }
                _ => unreachable!("a cache tier tag"),
            }
        }
        self.data.set(0);
    }

    pub fn copy_from(&self, other: &PropertyLookupCache) {
        self.clear();
        if other.is_keyed_generic() {
            self.data.set(PROPERTY_LOOKUP_CACHE_KEYED_GENERIC_DATA);
            return;
        }
        if let Some(data) = other.monomorphic_data() {
            self.set_data(
                Box::new(MonomorphicData {
                    entry: PropertyLookupCacheEntry::from_data(data.entry.get()),
                }),
                0,
            );
            return;
        }
        if let Some(data) = other.polymorphic_data() {
            self.set_data(
                Box::new(PolymorphicData {
                    entries: core::array::from_fn(|index| {
                        PropertyLookupCacheEntry::from_data(data.entries[index].get())
                    }),
                }),
                POLYMORPHIC_DATA_TAG,
            );
            return;
        }
        if let Some(data) = other.megamorphic_data() {
            self.set_data(
                Box::new(MegamorphicData {
                    entry: PropertyLookupCacheEntry::from_data(data.entry.get()),
                    primary_entries: core::array::from_fn(|index| {
                        PropertyLookupCacheEntry::from_data(data.primary_entries[index].get())
                    }),
                    secondary_entries: core::array::from_fn(|index| {
                        PropertyLookupCacheEntry::from_data(data.secondary_entries[index].get())
                    }),
                    keyed_misses: Cell::new(data.keyed_misses.get()),
                }),
                MEGAMORPHIC_DATA_TAG,
            );
        }
    }

    /// Forgets the entries with a cell that died in this collection. Only the VM's sweep callback calls this.
    pub fn remove_dead_entries(&self) {
        if let Some(data) = self.megamorphic_data() {
            for entry in data
                .primary_entries
                .iter()
                .chain(&data.secondary_entries)
                .chain(core::iter::once(&data.entry))
            {
                entry.clear_if_it_has_a_dead_cell();
            }
            return;
        }

        for entry in self.entries() {
            entry.clear_if_it_has_a_dead_cell();
        }
    }
}

impl Default for PropertyLookupCache {
    fn default() -> Self {
        Self::new()
    }
}

impl Drop for PropertyLookupCache {
    fn drop(&mut self) {
        self.clear();
    }
}

impl GlobalVariableCache {
    /// Assigns a default GlobalVariableCache.
    pub fn reset(&self) {
        self.entry.set(PropertyLookupCacheEntryData::default());
        self.environment_serial_number.set(0);
        self.environment_binding_index.set(0);
        self.has_environment_binding_index.set(false);
    }

    pub fn first_entry(&self) -> Option<PropertyLookupCacheEntryData> {
        let entry = self.entry.get();
        (entry.entry_type != PropertyLookupCacheEntryType::Empty).then_some(entry)
    }

    pub fn update(
        &self,
        entry_type: PropertyLookupCacheEntryType,
        callback: impl FnOnce(&mut PropertyLookupCacheEntryData),
    ) {
        let mut entry = PropertyLookupCacheEntryData {
            entry_type,
            ..Default::default()
        };
        callback(&mut entry);
        self.entry.set(entry);
    }
}

/// Defines the call sites of the runtime that keep a property lookup cache of their own.
macro_rules! define_static_property_lookup_cache_sites {
    ($($site:ident,)*) => {
        #[derive(Clone, Copy, Debug, PartialEq, Eq)]
        #[repr(usize)]
        pub enum StaticPropertyLookupCacheSite {
            $($site,)*
        }

        const STATIC_PROPERTY_LOOKUP_CACHE_SITE_COUNT: usize = [$(StaticPropertyLookupCacheSite::$site,)*].len();
    };
}

define_static_property_lookup_cache_sites! {
    ObjectOrdinaryToPrimitiveToString,
    ObjectOrdinaryToPrimitiveValueOf,
    LengthOfArrayLike,
    SpeciesConstructorConstructor,
    SpeciesConstructorSpecies,
    ValueToPrimitive,
    GetPrototypeFromConstructorPrototype,
    ValueIsRegExp,
    InstanceOfHasInstance,
    OrdinaryHasInstancePrototype,
    GetIteratorDirectNext,
    GetIteratorFromMethodNext,
    GetIteratorSyncMethod,
    GetIteratorMethod,
    GetIteratorFlattenableMethod,
    IteratorCompleteDone,
    IteratorValueValue,
    ArrayAppendIteratorMethod,
    ArrayAppendNextMethod,
    ObjectPrototypeToStringToStringTag,
    ArrayFromIteratorMethod,
    ArraySpeciesCreateConstructor,
    ArraySpeciesCreateSpecies,
    JSONSerializeToJSON,
    StringPrototypeMatchMatcher,
    StringPrototypeMatchAllMatcher,
    StringPrototypeReplaceReplacer,
    StringPrototypeReplaceAllReplacer,
    StringPrototypeSearchSearcher,
    StringPrototypeSplitSplitter,
    MapIterationIsUnobservableNextMethod,
    SetIterationIsUnobservableNextMethod,
    RegExpIncrementLastIndexGetLastIndex,
    RegExpIncrementLastIndexSetLastIndex,
    RegExpUnmodifiedInstanceExec,
    RegExpFlagsUnobservableFlags,
    RegExpFlagsUnobservableHasIndices,
    RegExpFlagsUnobservableGlobal,
    RegExpFlagsUnobservableIgnoreCase,
    RegExpFlagsUnobservableMultiline,
    RegExpFlagsUnobservableDotAll,
    RegExpFlagsUnobservableUnicode,
    RegExpFlagsUnobservableUnicodeSets,
    RegExpFlagsUnobservableSticky,
    RegExpLastIndexUnobservableLastIndex,
    RegExpBuiltinExecGetLastIndex,
    RegExpBuiltinExecResetLastIndexPastEnd,
    RegExpBuiltinExecResetLastIndexNoMatch,
    RegExpBuiltinExecSetLastIndex,
    RegExpBuiltinExecSetGroups,
    RegExpExecExec,
    RegExpPrototypeFlagsHasIndices,
    RegExpPrototypeFlagsGlobal,
    RegExpPrototypeFlagsIgnoreCase,
    RegExpPrototypeFlagsMultiline,
    RegExpPrototypeFlagsDotAll,
    RegExpPrototypeFlagsUnicode,
    RegExpPrototypeFlagsUnicodeSets,
    RegExpPrototypeFlagsSticky,
    RegExpPrototypeSymbolMatchFlags,
    RegExpPrototypeSymbolMatchLastIndex,
    RegExpPrototypeSymbolMatchAllFlags,
    RegExpPrototypeSymbolMatchAllGetLastIndex,
    RegExpPrototypeSymbolMatchAllSetLastIndex,
    RegExpPrototypeSymbolReplaceFastGetLastIndex,
    RegExpPrototypeSymbolReplaceFastResetLastIndex,
    RegExpPrototypeSymbolReplaceFastSetLastIndex,
    RegExpPrototypeSymbolReplaceFlags,
    RegExpPrototypeSymbolReplaceLastIndex,
    RegExpPrototypeSymbolReplaceIndex,
    RegExpPrototypeSymbolReplaceGroups,
    RegExpPrototypeSymbolSearchGetLastIndex,
    RegExpPrototypeSymbolSearchResetLastIndex,
    RegExpPrototypeSymbolSearchGetCurrentLastIndex,
    RegExpPrototypeSymbolSearchRestoreLastIndex,
    RegExpPrototypeSymbolSearchIndex,
    RegExpPrototypeSymbolSplitFlags,
    RegExpPrototypeSymbolSplitSetLastIndex,
    RegExpPrototypeSymbolSplitGetLastIndex,
    RegExpPrototypeToStringSource,
    RegExpPrototypeToStringFlags,
    IteratorConcatIteratorMethod,
}

/// The caches of the static call sites, one set per VM, which prunes them like the caches of executables.
pub struct StaticPropertyLookupCaches {
    caches: [PropertyLookupCache; STATIC_PROPERTY_LOOKUP_CACHE_SITE_COUNT],
}

impl StaticPropertyLookupCaches {
    pub fn new() -> Self {
        Self {
            caches: core::array::from_fn(|_| PropertyLookupCache::new()),
        }
    }

    pub fn get(&self, site: StaticPropertyLookupCacheSite) -> &PropertyLookupCache {
        &self.caches[site as usize]
    }

    pub fn remove_dead_entries(&self) {
        for cache in &self.caches {
            cache.remove_dead_entries();
        }
    }
}

impl Default for StaticPropertyLookupCaches {
    fn default() -> Self {
        Self::new()
    }
}

pub const KEYED_PROPERTY_LOOKUP_CACHE_ENTRY_COUNT: usize = 2048;

const _: () = assert!(KEYED_PROPERTY_LOOKUP_CACHE_ENTRY_COUNT.is_power_of_two());

/// One remembered lookup of a property by name on a shape. Like the entries of a PropertyLookupCache, it does not keep
/// its cells alive; the VM's sweep callback clears the entries whose cells died.
#[derive(Clone)]
pub struct KeyedPropertyLookupCacheEntry {
    pub entry_type: PropertyLookupCacheEntryType,
    pub property_offset: u32,
    pub shape_dictionary_generation: u32,
    pub shape: Option<Gc<Shape>>,
    pub prototype: Option<Gc<Object>>,
    pub prototype_chain_validity: Option<Gc<PrototypeChainValidity>>,
    pub property_name: Option<Utf16FlyString>,
}

impl Default for KeyedPropertyLookupCacheEntry {
    fn default() -> Self {
        Self {
            entry_type: PropertyLookupCacheEntryType::Empty,
            property_offset: 0,
            shape_dictionary_generation: 0,
            shape: None,
            prototype: None,
            prototype_chain_validity: None,
            property_name: None,
        }
    }
}

/// The VM-wide cache of string-keyed GetByValue lookups. Entries are copied in and out, so that none is borrowed
/// while the lookup it caches runs.
pub struct KeyedPropertyLookupCache {
    entries: RefCell<Box<[KeyedPropertyLookupCacheEntry]>>,
}

/// Mirrors AK::pair_int_hash.
fn pair_int_hash(key1: u32, key2: u32) -> u32 {
    u64_hash((u64::from(key1) << 32) | u64::from(key2))
}

impl KeyedPropertyLookupCache {
    pub fn new() -> Self {
        Self {
            entries: RefCell::new(
                (0..KEYED_PROPERTY_LOOKUP_CACHE_ENTRY_COUNT)
                    .map(|_| KeyedPropertyLookupCacheEntry::default())
                    .collect(),
            ),
        }
    }

    /// The index of the one entry that may hold the lookup of `property_name` on `shape`. Fly strings are interned,
    /// so the identity of the name stands in for the hash of its contents.
    pub fn entry_index_for(shape: Gc<Shape>, property_name: &Utf16FlyString) -> usize {
        let shape_hash = u64_hash(shape.as_ptr().addr() as u64);
        let property_name_hash = u64_hash(property_name.raw_identity() as u64);
        pair_int_hash(shape_hash, property_name_hash) as usize & (KEYED_PROPERTY_LOOKUP_CACHE_ENTRY_COUNT - 1)
    }

    pub fn entry(&self, index: usize) -> KeyedPropertyLookupCacheEntry {
        self.entries.borrow()[index].clone()
    }

    pub fn set_entry(&self, index: usize, entry: KeyedPropertyLookupCacheEntry) {
        self.entries.borrow_mut()[index] = entry;
    }

    /// Forgets the entries with a cell that died in this collection. Only the VM's sweep callback calls this.
    pub fn remove_dead_entries(&self) {
        for entry in self.entries.borrow_mut().iter_mut() {
            if entry.entry_type == PropertyLookupCacheEntryType::Empty {
                continue;
            }
            if entry.shape.is_some_and(cell_is_dead)
                || entry.prototype.is_some_and(cell_is_dead)
                || entry.prototype_chain_validity.is_some_and(cell_is_dead)
            {
                *entry = KeyedPropertyLookupCacheEntry::default();
            }
        }
    }
}

impl Default for KeyedPropertyLookupCache {
    fn default() -> Self {
        Self::new()
    }
}

// https://tc39.es/ecma262/#sec-gettemplateobject
// Template objects are cached at the call site.
#[repr(C)]
#[derive(Trace)]
pub struct TemplateObjectCache {
    header: CellHeader,
    cached_template_object: Cell<Option<Gc<Array>>>,
}

define_cell!(TemplateObjectCache, Other);

impl TemplateObjectCache {
    pub fn create(vm: &Vm) -> Gc<TemplateObjectCache> {
        vm.heap().allocate(TemplateObjectCache {
            header: CellHeader::for_class(Self::CLASS),
            cached_template_object: Cell::new(None),
        })
    }

    pub fn cached_template_object(&self) -> Option<Gc<Array>> {
        self.cached_template_object.get()
    }

    pub fn set_cached_template_object(&self, template_object: Gc<Array>) {
        self.cached_template_object.set(Some(template_object));
    }
}

// Cache for object literal shapes.
// When an object literal like {a: 1, b: 2} is instantiated, we cache the final shape
// so that subsequent instantiations can allocate the object with the correct shape directly,
// avoiding repeated shape transitions.
// We also cache the property offsets so that subsequent property writes can bypass
// shape lookups and write directly to the correct storage slot.
/// The shape is not kept alive; the VM's sweep callback forgets it when it dies.
#[derive(Default)]
pub struct ObjectShapeCache {
    pub shape: Cell<Option<Gc<Shape>>>,
    pub property_offsets: RefCell<Vec<u32>>,
}

/// The parts of an ObjectPropertyIteratorCacheData the interpreter does not read.
#[derive(Default)]
pub struct ObjectPropertyIteratorCacheDataStorage {
    properties: Box<[PropertyKey]>,
}

define_cell!(ObjectPropertyIteratorCacheData, Other, finalize: finalize);

// SAFETY: Visits the shape, the validity, the materialized key values and the keys.
unsafe impl Trace for ObjectPropertyIteratorCacheData {
    fn trace(&self, visitor: &mut Visitor) {
        self.shape.trace(visitor);
        self.prototype_chain_validity.trace(visitor);
        self.property_values.trace(visitor);
        self.storage.properties.trace(visitor);
    }
}

impl Finalize for ObjectPropertyIteratorCacheData {
    fn finalize(&self) {
        self.property_values.clear();
    }
}

impl ObjectPropertyIteratorCacheData {
    /// Fast-path snapshot: a cached, revalidatable key list for one shape, shared by every site that enumerates it.
    pub fn create_with_fast_path(
        vm: &Vm,
        properties: &MarkedVec<'_, PropertyKey>,
        fast_path: ObjectPropertyIteratorFastPath,
        indexed_property_count: u32,
        receiver_has_magical_length_property: bool,
        shape: Gc<Shape>,
        prototype_chain_validity: Option<Gc<PrototypeChainValidity>>,
    ) -> Gc<ObjectPropertyIteratorCacheData> {
        let (shape_is_dictionary, shape_dictionary_generation) = if shape.is_dictionary() {
            (true, shape.dictionary_generation())
        } else {
            (false, 0)
        };
        let cache_data = vm.heap().allocate(Self {
            header: CellHeader::for_class(Self::CLASS),
            shape_is_dictionary: Cell::new(shape_is_dictionary),
            fast_path: Cell::new(fast_path),
            receiver_has_magical_length: Cell::new(receiver_has_magical_length_property),
            indexed_property_count: Cell::new(indexed_property_count),
            shape_dictionary_generation: Cell::new(shape_dictionary_generation),
            shape: Cell::new(Some(shape)),
            prototype_chain_validity: Cell::new(prototype_chain_validity),
            property_values: InterpreterBuffer::new(),
            storage: ObjectPropertyIteratorCacheDataStorage {
                properties: properties.to_vec().into_boxed_slice(),
            },
        });

        // The iterator fast path returns JS Values directly, so materialize the
        // cached key list once up front instead of converting PropertyKeys during
        // every ObjectPropertyIteratorNext.
        cache_data
            .property_values
            .ensure_capacity(indexed_property_count as usize + properties.len());
        for index in 0..indexed_property_count {
            let value = PropertyKey::from(index).to_value(vm);
            cache_data.property_values.append(value);
        }
        for index in 0..properties.len() {
            let value = properties.get(index).expect("the index is in bounds").to_value(vm);
            cache_data.property_values.append(value);
        }
        cache_data
    }

    /// Slow-path snapshot: a plain key list with no fast path. Enumeration filters deleted keys with has_property() at
    /// each step, so there is no shape to revalidate against.
    pub fn create(vm: &Vm, properties: &MarkedVec<'_, PropertyKey>) -> Gc<ObjectPropertyIteratorCacheData> {
        // The slow path keeps only the key list. Values are converted lazily during enumeration,
        // because deleted keys have to be filtered with has_property() at each step anyway.
        vm.heap().allocate(Self {
            header: CellHeader::for_class(Self::CLASS),
            shape_is_dictionary: Cell::new(false),
            fast_path: Cell::new(ObjectPropertyIteratorFastPath::None),
            receiver_has_magical_length: Cell::new(false),
            indexed_property_count: Cell::new(0),
            shape_dictionary_generation: Cell::new(0),
            shape: Cell::new(None),
            prototype_chain_validity: Cell::new(None),
            property_values: InterpreterBuffer::new(),
            storage: ObjectPropertyIteratorCacheDataStorage {
                properties: properties.to_vec().into_boxed_slice(),
            },
        })
    }

    pub fn property_count(&self) -> usize {
        self.storage.properties.len()
    }

    pub fn property(&self, index: usize) -> PropertyKey {
        self.storage.properties[index].clone()
    }

    pub fn fast_path(&self) -> ObjectPropertyIteratorFastPath {
        self.fast_path.get()
    }

    pub fn indexed_property_count(&self) -> u32 {
        self.indexed_property_count.get()
    }

    pub fn receiver_has_magical_length_property(&self) -> bool {
        self.receiver_has_magical_length.get()
    }

    pub fn shape(&self) -> Option<Gc<Shape>> {
        self.shape.get()
    }

    pub fn prototype_chain_validity(&self) -> Option<Gc<PrototypeChainValidity>> {
        self.prototype_chain_validity.get()
    }

    pub fn shape_dictionary_generation(&self) -> u32 {
        self.shape_dictionary_generation.get()
    }

    /// The key values the interpreter's fast path hands out, one per indexed and named key.
    pub fn property_value(&self, index: usize) -> Value {
        self.property_values.get(index)
    }

    pub fn property_value_count(&self) -> usize {
        self.property_values.size()
    }
}

/// Where the bytecode of an executable lives: in memory of its own, or in place in the bytecode cache blob it was
/// materialized from, which it keeps alive.
pub enum ExecutableBytecode {
    Owned(Box<[u8]>),
    InBytecodeCacheBlob(DecodedBytecodeBytes),
}

impl ExecutableBytecode {
    pub fn as_slice(&self) -> &[u8] {
        match self {
            Self::Owned(bytecode) => bytecode,
            Self::InBytecodeCacheBlob(bytecode) => bytecode.as_slice(),
        }
    }
}

/// A unit of bytecode: a script, a module, a function body or an eval, with what the interpreter needs to run it.
#[repr(C)]
pub struct Executable {
    pub head: ExecutableHead,
    bytecode: ExecutableBytecode,
    constants: Box<[Value]>,
    property_lookup_caches: Box<[PropertyLookupCache]>,
    global_variable_caches: Box<[GlobalVariableCache]>,
    environment_coordinate_caches: Box<[Cell<EnvironmentCoordinate>]>,
    environment_shape_caches: Box<[Cell<Option<Gc<EnvironmentShape>>>]>,
    template_object_caches: Box<[Gc<TemplateObjectCache>]>,
    object_shape_caches: Box<[ObjectShapeCache]>,
    object_property_iterator_caches: Box<[ObjectPropertyIteratorCache]>,
    pub number_of_registers: u32,
    pub number_of_arguments: u32,
    pub is_strict_mode: bool,
    pub identifier_table: Vec<ak::Utf16FlyString>,
    /// The property keys as the strings they were made from, which is how the bytecode dump prints them.
    property_key_table: Vec<ak::Utf16FlyString>,
    /// The property keys themselves, made once, since turning a string into a property key checks whether it is an
    /// array index.
    property_keys: Box<[PropertyKey]>,
    pub string_table: Vec<ak::Utf16FlyString>,
    /// Sorted by start offset, and not overlapping.
    pub exception_handlers: Box<[ExceptionHandler]>,
    /// The functions the bytecode creates, which NewFunction and NewClass refer to by index.
    shared_function_data: Box<[Gc<SharedFunctionInstanceData>]>,
    class_blueprints: Box<[ClassBlueprint]>,
    pub length_identifier: Option<PropertyKeyTableIndex>,
    /// The name of the function the executable is the body of, given when the function is first compiled. Empty for
    /// other code.
    name: FlyStringSlot,
    /// Sorted by bytecode offset: where in the source code the instructions from each offset on came from.
    pub source_map: Box<[SourceMapEntry]>,
    pub local_variable_names: Box<[Utf16FlyString]>,
    pub argument_variable_names: Box<[Utf16FlyString]>,
    /// One for each of the local variables.
    pub local_variable_metadata: Box<[LocalVariableMetadata]>,
    pub source_code: Option<Rc<SourceCode>>,
    /// The breakpoints of the debugger at the offsets of the instructions it pauses before.
    debugger_breakpoint_sites: RefCell<HashMap<u32, DebuggerBreakpointSite, foldhash::fast::RandomState>>,
}

define_cell!(Executable, Other);

/// Executable::LocalVariableScopeRange: the part of the source code in which a local variable is in scope.
#[derive(Clone, Copy)]
pub struct LocalVariableScopeRange {
    pub start: Position,
    pub end: Position,
}

/// Executable::LocalVariableMetadata
#[derive(Clone, Copy)]
pub struct LocalVariableMetadata {
    pub is_mutable: bool,
    pub scope_range: Option<LocalVariableScopeRange>,
}

#[derive(Default)]
struct DebuggerBreakpointSite {
    breakpoint_ids: Vec<BreakpointID>,
}

const _: () = assert!(core::mem::offset_of!(Executable, head) == 0);

/// The cache counts an executable's bytecode refers to.
pub struct ExecutableCacheCounts {
    pub property_lookup_caches: u32,
    pub global_variable_caches: u32,
    pub environment_coordinate_caches: u32,
    pub environment_shape_caches: u32,
}

/// What an executable is made of, as the frontend compiled it or a bytecode cache blob holds it.
struct ExecutableParts {
    bytecode: ExecutableBytecode,
    number_of_registers: u32,
    number_of_arguments: u32,
    is_strict: bool,
    cache_counts: FrontendExecutableCacheCounts,
    identifier_table: Vec<Utf16FlyString>,
    property_key_table: Vec<Utf16FlyString>,
    string_table: Vec<Utf16FlyString>,
    constants: Vec<ConstantValue>,
    exception_handlers: Vec<ExceptionHandler>,
    source_map: Vec<SourceMapEntry>,
    local_variables: Vec<LocalVariable>,
    argument_variable_names: Vec<Utf16FlyString>,
    length_identifier: Option<u32>,
    class_blueprints: Vec<PendingClassBlueprint>,
}

fn interpreter_buffer<T>(elements: &[T]) -> InterpreterBuffer<T> {
    InterpreterBuffer {
        data: Cell::new(elements.as_ptr().cast_mut()),
        size: Cell::new(elements.len()),
        capacity: Cell::new(elements.len()),
    }
}

impl Executable {
    /// Moves `executable` into the heap and has the VM prune its inline caches after every collection.
    pub fn create_from_parts(vm: &Vm, executable: Executable) -> Gc<Executable> {
        let executable = vm.heap().allocate(executable);
        vm.register_executable(executable);
        executable
    }

    pub fn property_lookup_cache(&self, index: usize) -> &PropertyLookupCache {
        &self.property_lookup_caches[index]
    }

    pub fn template_object_cache(&self, index: u32) -> Gc<TemplateObjectCache> {
        self.template_object_caches[index as usize]
    }

    pub fn object_shape_cache(&self, index: u32) -> &ObjectShapeCache {
        &self.object_shape_caches[index as usize]
    }

    pub fn object_property_iterator_cache(&self, index: u32) -> &ObjectPropertyIteratorCache {
        &self.object_property_iterator_caches[index as usize]
    }

    pub fn get_identifier(&self, index: IdentifierTableIndex) -> &Utf16FlyString {
        &self.identifier_table[index.0 as usize]
    }

    pub fn get_property_key(&self, index: PropertyKeyTableIndex) -> &PropertyKey {
        &self.property_keys[index.0 as usize]
    }

    pub fn property_key_table(&self) -> &[ak::Utf16FlyString] {
        &self.property_key_table
    }

    pub fn set_property_key_table(&mut self, property_key_table: Vec<ak::Utf16FlyString>) {
        self.property_keys = property_key_table.iter().cloned().map(PropertyKey::from).collect();
        self.property_key_table = property_key_table;
    }

    pub fn get_string(&self, index: StringTableIndex) -> &Utf16FlyString {
        &self.string_table[index.0 as usize]
    }

    /// Forgets the cells the inline caches remember that died in this collection.
    pub fn remove_dead_cells(&self) {
        for cache in &self.property_lookup_caches {
            cache.remove_dead_entries();
        }
        for cache in &self.global_variable_caches {
            cache.entry.clear_if_it_has_a_dead_cell();
        }
        for cache in &self.object_shape_caches {
            if cache.shape.get().is_some_and(cell_is_dead) {
                cache.shape.set(None);
            }
        }
    }

    pub fn new(
        bytecode: Box<[u8]>,
        number_of_registers: u32,
        number_of_locals: u32,
        number_of_arguments: u32,
        constants: Box<[Value]>,
        cache_counts: &ExecutableCacheCounts,
        is_strict_mode: bool,
    ) -> Self {
        Self::new_with_bytecode(
            ExecutableBytecode::Owned(bytecode),
            number_of_registers,
            number_of_locals,
            number_of_arguments,
            constants,
            cache_counts,
            is_strict_mode,
        )
    }

    pub fn new_with_bytecode(
        bytecode: ExecutableBytecode,
        number_of_registers: u32,
        number_of_locals: u32,
        number_of_arguments: u32,
        constants: Box<[Value]>,
        cache_counts: &ExecutableCacheCounts,
        is_strict_mode: bool,
    ) -> Self {
        let property_lookup_caches: Box<[PropertyLookupCache]> = (0..cache_counts.property_lookup_caches)
            .map(|_| PropertyLookupCache::new())
            .collect();
        let global_variable_caches: Box<[GlobalVariableCache]> = (0..cache_counts.global_variable_caches)
            .map(|_| GlobalVariableCache {
                entry: PropertyLookupCacheEntry::new(),
                environment_serial_number: Cell::new(0),
                environment_binding_index: Cell::new(0),
                has_environment_binding_index: Cell::new(false),
            })
            .collect();
        let environment_coordinate_caches: Box<[Cell<EnvironmentCoordinate>]> = (0..cache_counts
            .environment_coordinate_caches)
            .map(|_| Cell::new(EnvironmentCoordinate::invalid()))
            .collect();
        let environment_shape_caches = (0..cache_counts.environment_shape_caches)
            .map(|_| Cell::new(None))
            .collect();
        let registers_and_locals_count = number_of_registers + number_of_locals;
        let constant_count = u32::try_from(constants.len()).expect("constant count fits in u32");
        let head = ExecutableHead {
            header: CellHeader::for_class(Self::CLASS),
            registers_and_locals_count: Cell::new(registers_and_locals_count),
            registers_and_locals_and_constants_count: Cell::new(registers_and_locals_count + constant_count),
            asm_constants_size: Cell::new(constants.len() as u64),
            asm_constants_data: Cell::new(constants.as_ptr()),
            bytecode_data: Cell::new(bytecode.as_slice().as_ptr()),
            bytecode_size: Cell::new(bytecode.as_slice().len()),
            constants: interpreter_buffer(&constants),
            property_lookup_caches: interpreter_buffer(&property_lookup_caches),
            global_variable_caches: interpreter_buffer(&global_variable_caches),
            // The interpreter only reads the caches, which slow paths update through their cells.
            environment_coordinate_caches: InterpreterBuffer {
                data: Cell::new(environment_coordinate_caches.as_ptr().cast_mut().cast()),
                size: Cell::new(environment_coordinate_caches.len()),
                capacity: Cell::new(environment_coordinate_caches.len()),
            },
        };
        Self {
            head,
            bytecode,
            constants,
            property_lookup_caches,
            global_variable_caches,
            environment_coordinate_caches,
            environment_shape_caches,
            template_object_caches: Box::new([]),
            object_shape_caches: Box::new([]),
            object_property_iterator_caches: Box::new([]),
            number_of_registers,
            number_of_arguments,
            is_strict_mode,
            identifier_table: Vec::new(),
            property_key_table: Vec::new(),
            property_keys: Box::new([]),
            string_table: Vec::new(),
            exception_handlers: Box::new([]),
            shared_function_data: Box::new([]),
            class_blueprints: Box::new([]),
            length_identifier: None,
            name: FlyStringSlot::new(None),
            source_map: Box::new([]),
            local_variable_names: Box::new([]),
            argument_variable_names: Box::new([]),
            local_variable_metadata: Box::new([]),
            source_code: None,
            debugger_breakpoint_sites: RefCell::new(HashMap::default()),
        }
    }

    /// Gives the executable the object literal shape caches and for-in key snapshot caches its bytecode refers to.
    pub fn allocate_object_caches(&mut self, object_shape_caches: u32, object_property_iterator_caches: u32) {
        self.object_shape_caches = (0..object_shape_caches).map(|_| ObjectShapeCache::default()).collect();
        self.object_property_iterator_caches = (0..object_property_iterator_caches)
            .map(|_| ObjectPropertyIteratorCache { data: Cell::new(None) })
            .collect();
    }

    /// Creates the executable for what the frontend compiled from code it does not know the source of.
    pub fn create(vm: &Vm, data: ExecutableData) -> Gc<Executable> {
        Self::create_with_source_code(vm, data, None)
    }

    /// Creates the executable for what the frontend compiled from `source_code`, with the functions and classes it
    /// declares.
    pub fn create_with_source_code(
        vm: &Vm,
        mut data: ExecutableData,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<Executable> {
        // The shared function data stays rooted until the executable that holds it is allocated.
        let rooted_shared_function_data = MarkedVec::with_capacity(vm, data.shared_function_data.len());
        let is_strict = data.is_strict;
        for pending in &mut data.shared_function_data {
            rooted_shared_function_data.push(SharedFunctionInstanceData::create_from_pending_shared_function_data(
                vm,
                pending,
                is_strict,
                source_code,
            ));
        }

        // The regexes were only compiled to report early errors; the runtime compiles them again when it runs.
        for regex in data.compiled_regexes {
            // SAFETY: Each handle came from rust_compile_regex and is freed once.
            unsafe { rust_free_compiled_regex(regex.into_raw()) };
        }
        let parts = ExecutableParts {
            bytecode: ExecutableBytecode::Owned(data.bytecode.into_boxed_slice()),
            number_of_registers: data.number_of_registers,
            number_of_arguments: data.number_of_arguments,
            is_strict: data.is_strict,
            cache_counts: data.cache_counts,
            identifier_table: data.identifier_table,
            property_key_table: data.property_key_table,
            string_table: data.string_table,
            constants: data.constants,
            exception_handlers: data.exception_handlers,
            source_map: data.source_map,
            local_variables: data.local_variables,
            argument_variable_names: data.argument_variable_names,
            length_identifier: data.length_identifier.map(|index| index.0),
            class_blueprints: data.class_blueprints,
        };
        Self::assemble(vm, parts, &rooted_shared_function_data, source_code, None)
    }

    /// Creates the executable of a record of a bytecode cache blob that passed validation, whose bytecode runs in
    /// place in the blob. `rooted_shared_function_data` holds the functions the bytecode creates, made from the
    /// record's function records in their order. An executable this one replaces in a running program hands its
    /// inline caches over. Returns `None` if the record turns out to be malformed.
    pub fn create_from_bytecode_cache(
        vm: &Vm,
        record: &DecodedExecutableRecord,
        rooted_shared_function_data: &MarkedVec<'_, Gc<SharedFunctionInstanceData>>,
        source_code: &Rc<SourceCode>,
        replaced_executable: Option<Gc<Executable>>,
    ) -> Option<Gc<Executable>> {
        let parts = ExecutableParts {
            bytecode: ExecutableBytecode::InBytecodeCacheBlob(record.bytecode().clone()),
            number_of_registers: record.number_of_registers(),
            number_of_arguments: record.number_of_arguments(),
            is_strict: record.is_strict(),
            cache_counts: record.cache_counts(),
            identifier_table: record.identifiers()?,
            property_key_table: record.property_keys()?,
            string_table: record.strings()?,
            constants: record.constant_values()?,
            exception_handlers: record.exception_handler_entries()?,
            source_map: record.source_map_entries()?,
            local_variables: record.locals()?,
            argument_variable_names: record.argument_names()?,
            length_identifier: record.length_identifier(),
            class_blueprints: record.classes()?,
        };
        Some(Self::assemble(
            vm,
            parts,
            rooted_shared_function_data,
            Some(source_code),
            replaced_executable,
        ))
    }

    fn assemble(
        vm: &Vm,
        parts: ExecutableParts,
        rooted_shared_function_data: &MarkedVec<'_, Gc<SharedFunctionInstanceData>>,
        source_code: Option<&Rc<SourceCode>>,
        replaced_executable: Option<Gc<Executable>>,
    ) -> Gc<Executable> {
        // The literal values of class elements stay rooted until the executable that holds them is allocated.
        let rooted_literal_values = MarkedVec::new(vm);
        let class_blueprints: Box<[ClassBlueprint]> = parts
            .class_blueprints
            .iter()
            .map(|blueprint| ClassBlueprint::create(vm, blueprint, source_code, &rooted_literal_values))
            .collect();
        let shared_function_data: Box<[Gc<SharedFunctionInstanceData>]> =
            rooted_shared_function_data.to_vec().into_boxed_slice();

        // The constants stay rooted until the executable that holds them is allocated.
        let rooted_constants = MarkedVec::with_capacity(vm, parts.constants.len());
        for constant in &parts.constants {
            rooted_constants.push(constant_value(vm, constant));
        }
        let constants: Box<[Value]> = rooted_constants.to_vec().into_boxed_slice();
        // The template object caches stay rooted until the executable that holds them is allocated. An executable this
        // one replaces shares its own, so that both hand out the same template objects.
        let rooted_template_object_caches = MarkedVec::with_capacity(vm, parts.cache_counts.template_object as usize);
        match replaced_executable {
            Some(replaced_executable)
                if replaced_executable.template_object_caches.len() == parts.cache_counts.template_object as usize =>
            {
                for cache in &replaced_executable.template_object_caches {
                    rooted_template_object_caches.push(*cache);
                }
            }
            _ => {
                for _ in 0..parts.cache_counts.template_object {
                    rooted_template_object_caches.push(TemplateObjectCache::create(vm));
                }
            }
        }
        let template_object_caches: Box<[Gc<TemplateObjectCache>]> =
            rooted_template_object_caches.to_vec().into_boxed_slice();
        let counts = ExecutableCacheCounts {
            property_lookup_caches: parts.cache_counts.property_lookup,
            global_variable_caches: parts.cache_counts.global_variable,
            environment_coordinate_caches: parts.cache_counts.environment_coordinate,
            environment_shape_caches: parts.cache_counts.environment_shape,
        };
        let number_of_locals = u32::try_from(parts.local_variables.len()).expect("local count fits in u32");
        let mut executable = Self::new_with_bytecode(
            parts.bytecode,
            parts.number_of_registers,
            number_of_locals,
            parts.number_of_arguments,
            constants,
            &counts,
            parts.is_strict,
        );
        executable.identifier_table = parts.identifier_table;
        executable.set_property_key_table(parts.property_key_table);
        executable.string_table = parts.string_table;
        executable.exception_handlers = parts.exception_handlers.into_boxed_slice();
        executable.shared_function_data = shared_function_data;
        executable.class_blueprints = class_blueprints;
        executable.template_object_caches = template_object_caches;
        executable.allocate_object_caches(
            parts.cache_counts.object_shape,
            parts.cache_counts.object_property_iterator,
        );
        executable.length_identifier = parts.length_identifier.map(PropertyKeyTableIndex);
        executable.source_map = parts.source_map.into_boxed_slice();
        executable.local_variable_metadata = parts
            .local_variables
            .iter()
            .map(|local_variable| LocalVariableMetadata {
                is_mutable: local_variable.is_mutable,
                scope_range: local_variable.scope_range.map(|range| LocalVariableScopeRange {
                    start: Position {
                        line: range.start.line,
                        column: range.start.column,
                    },
                    end: Position {
                        line: range.end.line,
                        column: range.end.column,
                    },
                }),
            })
            .collect();
        executable.local_variable_names = parts
            .local_variables
            .into_iter()
            .map(|local_variable| local_variable.name)
            .collect();
        executable.argument_variable_names = parts.argument_variable_names.into_boxed_slice();
        executable.source_code = source_code.cloned();
        let executable = Self::create_from_parts(vm, executable);
        if let Some(replaced_executable) = replaced_executable {
            Self::copy_runtime_caches_from(executable, &replaced_executable);
        }
        drop(rooted_template_object_caches);
        drop(rooted_constants);
        drop(rooted_literal_values);

        if let Some(debugger) = vm.debugger() {
            debugger.register_executable(vm, executable);
        }

        executable
    }

    pub fn head(executable: Gc<Executable>) -> Gc<ExecutableHead> {
        // SAFETY: An Executable starts with its head.
        unsafe { Gc::from_non_null(executable.as_non_null().cast()) }
    }

    pub fn from_head(head: Gc<ExecutableHead>) -> Gc<Executable> {
        // SAFETY: Only executables have an ExecutableHead.
        unsafe { Gc::from_non_null(head.as_non_null().cast()) }
    }

    pub fn shared_function_data(&self, index: u32) -> Gc<SharedFunctionInstanceData> {
        self.shared_function_data[index as usize]
    }

    pub fn shared_function_data_count(&self) -> usize {
        self.shared_function_data.len()
    }

    pub fn class_blueprint(&self, index: u32) -> &ClassBlueprint {
        &self.class_blueprints[index as usize]
    }

    /// The handler whose range holds the instruction at `offset`.
    pub fn exception_handlers_for_offset(&self, offset: u32) -> Option<&ExceptionHandler> {
        self.exception_handlers
            .binary_search_by(|handler| {
                if offset < handler.start_offset {
                    core::cmp::Ordering::Greater
                } else if offset >= handler.end_offset {
                    core::cmp::Ordering::Less
                } else {
                    core::cmp::Ordering::Equal
                }
            })
            .ok()
            .map(|index| &self.exception_handlers[index])
    }

    pub fn source_range_at(&self, offset: u32) -> Option<SourceRange> {
        if offset as usize >= self.bytecode().len() {
            return None;
        }
        if self.source_map.is_empty() {
            return None;
        }
        let entries_at_or_before_offset = self.source_map.partition_point(|entry| entry.bytecode_offset <= offset);
        if entries_at_or_before_offset == 0 {
            return None;
        }
        let entry = &self.source_map[entries_at_or_before_offset - 1];
        Some(SourceRange {
            code: self
                .source_code
                .clone()
                .unwrap_or_else(|| SourceCode::create(ak::Utf16String::default(), ak::Utf16String::default())),
            start: Position {
                line: entry.line,
                column: entry.column,
            },
        })
    }

    /// The source range at `program_counter`, or an empty one if the source map has none.
    pub fn get_source_range(&self, program_counter: u32) -> SourceRange {
        self.source_range_at(program_counter).unwrap_or_else(|| SourceRange {
            code: SourceCode::create(ak::Utf16String::default(), ak::Utf16String::default()),
            start: Position::default(),
        })
    }

    pub fn add_debugger_breakpoint(&self, bytecode_offset: u32, breakpoint_id: BreakpointID) {
        let mut sites = self.debugger_breakpoint_sites.borrow_mut();
        let site = sites.entry(bytecode_offset).or_default();
        if !site.breakpoint_ids.contains(&breakpoint_id) {
            site.breakpoint_ids.push(breakpoint_id);
        }
    }

    pub fn remove_debugger_breakpoint(&self, breakpoint_id: BreakpointID) {
        let mut sites = self.debugger_breakpoint_sites.borrow_mut();
        sites.retain(|_, site| {
            if let Some(index) = site.breakpoint_ids.iter().position(|&id| id == breakpoint_id) {
                site.breakpoint_ids.remove(index);
            }
            !site.breakpoint_ids.is_empty()
        });
        if sites.is_empty() {
            sites.shrink_to_fit();
        }
    }

    pub fn clear_debugger_breakpoints(&self) {
        *self.debugger_breakpoint_sites.borrow_mut() = HashMap::default();
    }

    pub fn has_debugger_breakpoint_at(&self, bytecode_offset: u32) -> bool {
        self.debugger_breakpoint_sites.borrow().contains_key(&bytecode_offset)
    }

    pub fn debugger_breakpoints_at(&self, bytecode_offset: u32) -> Vec<BreakpointID> {
        self.debugger_breakpoint_sites
            .borrow()
            .get(&bytecode_offset)
            .map(|site| site.breakpoint_ids.clone())
            .unwrap_or_default()
    }

    pub fn has_debugger_breakpoint(&self, breakpoint_id: BreakpointID) -> bool {
        self.debugger_breakpoint_sites
            .borrow()
            .values()
            .any(|site| site.breakpoint_ids.contains(&breakpoint_id))
    }

    pub fn bytecode(&self) -> &[u8] {
        self.bytecode.as_slice()
    }

    /// Whether the bytecode runs in place in the bytecode cache blob it was materialized from.
    pub fn runs_in_place_in_bytecode_cache_blob(&self) -> bool {
        matches!(self.bytecode, ExecutableBytecode::InBytecodeCacheBlob(_))
    }

    /// Executable::copy_runtime_caches_from(): makes `executable` take over what the inline caches of `other`, an
    /// executable compiled from the same code, learned, as installing a bytecode cache into a running program does.
    /// Each kind of cache is only taken over if the two executables have the same number of them. The template object
    /// caches are not copied here: an executable shares those of the one it replaces from its creation.
    /// NB: This takes an executable on the heap because the VM only forgets the dead cells in the caches of executables
    ///     it registered. Entries copied into one not registered yet could outlive the cells they point to.
    fn copy_runtime_caches_from(executable: Gc<Executable>, other: &Executable) {
        let this = &*executable;
        if this.property_lookup_caches.len() == other.property_lookup_caches.len() {
            for (cache, other_cache) in this.property_lookup_caches.iter().zip(&other.property_lookup_caches) {
                cache.copy_from(other_cache);
            }
        }
        if this.global_variable_caches.len() == other.global_variable_caches.len() {
            for (cache, other_cache) in this.global_variable_caches.iter().zip(&other.global_variable_caches) {
                cache.entry.set(other_cache.entry.get());
                cache
                    .environment_serial_number
                    .set(other_cache.environment_serial_number.get());
                cache
                    .environment_binding_index
                    .set(other_cache.environment_binding_index.get());
                cache
                    .has_environment_binding_index
                    .set(other_cache.has_environment_binding_index.get());
            }
        }
        if this.environment_coordinate_caches.len() == other.environment_coordinate_caches.len() {
            for (cache, other_cache) in this
                .environment_coordinate_caches
                .iter()
                .zip(&other.environment_coordinate_caches)
            {
                cache.set(other_cache.get());
            }
        }
        if this.object_shape_caches.len() == other.object_shape_caches.len() {
            for (cache, other_cache) in this.object_shape_caches.iter().zip(&other.object_shape_caches) {
                cache.shape.set(other_cache.shape.get());
                cache
                    .property_offsets
                    .replace(other_cache.property_offsets.borrow().clone());
            }
        }
        if this.object_property_iterator_caches.len() == other.object_property_iterator_caches.len() {
            for (cache, other_cache) in this
                .object_property_iterator_caches
                .iter()
                .zip(&other.object_property_iterator_caches)
            {
                cache.data.set(other_cache.data.get());
            }
        }
        if this.environment_shape_caches.len() == other.environment_shape_caches.len() {
            for (cache, other_cache) in this
                .environment_shape_caches
                .iter()
                .zip(&other.environment_shape_caches)
            {
                cache.set(other_cache.get());
            }
        }
    }

    pub fn constants(&self) -> &[Value] {
        &self.constants
    }

    pub fn registers_and_locals_count(&self) -> u32 {
        self.head.registers_and_locals_count.get()
    }

    pub fn global_variable_cache(&self, index: u32) -> &GlobalVariableCache {
        &self.global_variable_caches[index as usize]
    }

    pub fn environment_coordinate_cache(&self, index: u32) -> &Cell<EnvironmentCoordinate> {
        &self.environment_coordinate_caches[index as usize]
    }

    pub fn environment_shape_cache(&self, index: u32) -> EnvironmentShapeCache {
        let slot = &self.environment_shape_caches[index as usize];
        // SAFETY: Executables are only reached through the heap, and the slot is part of this one's caches, which
        // live as long as it does.
        unsafe { EnvironmentShapeCache::new(Gc::from_ref(self), slot) }
    }

    pub fn name(&self) -> Utf16FlyString {
        self.name.get().unwrap_or_default()
    }

    pub fn set_name(&self, name: Utf16FlyString) {
        self.name.set(Some(name));
    }

    pub fn source_code(&self) -> Option<&Rc<SourceCode>> {
        self.source_code.as_ref()
    }

    /// The operand index of the first local, which follows the registers.
    pub fn local_index_base(&self) -> u32 {
        self.number_of_registers
    }

    /// The operand index of the first argument, which follows the registers, the locals and the constants.
    pub fn argument_index_base(&self) -> u32 {
        self.head.registers_and_locals_and_constants_count.get()
    }

    pub fn dump(&self) {
        let mut output = self.dump_to_builder();
        // warnln("{}", output.string_view());
        output.push(b'\n');
        let _ = std::io::stderr().write_all(&output);
    }

    /// The StringBuilder dump() builds before it writes it to the standard error.
    fn dump_to_builder(&self) -> Vec<u8> {
        let mut output = Vec::new();

        dump_header(&mut output, self);
        dump_metadata(&mut output, self);
        output.push(b'\n');
        dump_bytecode(&mut output, self);

        output.push(b'\n');
        output
    }
}

fn first_real_source_map_entry(executable: &Executable) -> Option<&SourceMapEntry> {
    let mut first_entry: Option<&SourceMapEntry> = None;
    for entry in &executable.source_map {
        if entry.line == 0 && entry.column == 0 {
            continue;
        }
        if first_entry.is_none_or(|first_entry| {
            entry.line < first_entry.line || (entry.line == first_entry.line && entry.column < first_entry.column)
        }) {
            first_entry = Some(entry);
        }
    }
    first_entry
}

fn dump_header(output: &mut Vec<u8>, executable: &Executable) {
    const WHITE_BOLD: &str = "\x1b[37;1m";
    const RESET: &str = "\x1b[0m";
    let first_source_map_entry = first_real_source_map_entry(executable);

    let mut hash: u32 = 2166136261; // FNV-1a offset basis
    let update_hash = |hash: &mut u32, value: u32| {
        for i in 0..u32::BITS / 8 {
            *hash ^= (value >> (i * 8)) & 0xFF;
            *hash = hash.wrapping_mul(16777619);
        }
    };
    let update_hash_with_code_unit = |hash: &mut u32, code_unit: u16| {
        *hash ^= u32::from(code_unit) & 0xFF;
        *hash = hash.wrapping_mul(16777619);
        *hash ^= (u32::from(code_unit) >> 8) & 0xFF;
        *hash = hash.wrapping_mul(16777619);
    };

    let name = executable.name();
    let name_view = Utf16View::of_fly_string(&name);
    for code_unit in name_view.code_units() {
        update_hash_with_code_unit(&mut hash, code_unit);
    }
    if let Some(first_source_map_entry) = first_source_map_entry {
        update_hash(&mut hash, first_source_map_entry.line);
        update_hash(&mut hash, first_source_map_entry.column);
    }
    update_hash(
        &mut hash,
        u32::try_from(executable.bytecode().len()).unwrap_or(u32::MAX),
    );

    output.extend_from_slice(WHITE_BOLD.as_bytes());
    name_view.append_as_wtf8_to(output);
    let _ = write!(output, "${hash:08x}{RESET}");

    // Show source location if available.
    if let Some(first_source_map_entry) = first_source_map_entry {
        let filename = executable.source_code.as_ref().map_or(Utf16View::EMPTY, |source_code| {
            Utf16View::of_string(source_code.filename())
        });
        if !filename.is_empty() {
            // Show just the basename to keep output portable across machines.
            let mut last_slash = None;
            for (i, code_unit) in filename.code_units().enumerate() {
                if code_unit == u16::from(b'/') {
                    last_slash = Some(i);
                }
            }
            let filename = match last_slash {
                Some(last_slash) => {
                    filename.substring_view(last_slash + 1, filename.length_in_code_units() - last_slash - 1)
                }
                None => filename,
            };
            output.push(b' ');
            filename.append_as_wtf8_to(output);
            let _ = write!(
                output,
                ":{}:{}",
                first_source_map_entry.line, first_source_map_entry.column
            );
        } else {
            let _ = write!(
                output,
                " line {}, column {}",
                first_source_map_entry.line, first_source_map_entry.column
            );
        }
    }
    output.push(b'\n');
}

fn dump_metadata(output: &mut Vec<u8>, executable: &Executable) {
    const GREEN: &str = "\x1b[32m";
    const YELLOW: &str = "\x1b[33m";
    const BLUE: &str = "\x1b[34m";
    const CYAN: &str = "\x1b[36m";
    const RESET: &str = "\x1b[0m";

    let _ = writeln!(output, "  {GREEN}Registers{RESET}: {}", executable.number_of_registers);
    let _ = writeln!(
        output,
        "  {GREEN}Blocks{RESET}:    {}",
        count_bytecode_basic_blocks(executable)
    );

    if !executable.local_variable_names.is_empty() {
        let _ = write!(output, "  {GREEN}Locals{RESET}:    ");
        for (i, local_variable_name) in executable.local_variable_names.iter().enumerate() {
            if i != 0 {
                output.extend_from_slice(b", ");
            }
            output.extend_from_slice(BLUE.as_bytes());
            Utf16View::of_fly_string(local_variable_name).append_as_wtf8_to(output);
            let _ = write!(output, "~{i}{RESET}");
        }
        output.push(b'\n');
    }

    if !executable.constants.is_empty() {
        let _ = writeln!(output, "  {GREEN}Constants{RESET}:");
        for (i, &value) in executable.constants.iter().enumerate() {
            output.extend_from_slice(b"    ");
            let _ = write!(output, "{YELLOW}[{i}]{RESET} = ");
            output.extend_from_slice(CYAN.as_bytes());
            if value.is_empty() {
                output.extend_from_slice(b"<Empty>");
            } else if value.is_boolean() {
                let _ = write!(output, "Bool({})", value.as_bool());
            } else if value.is_int32() {
                let _ = write!(output, "Int32({})", value.as_i32());
            } else if value.is_double() {
                output.extend_from_slice(b"Double(");
                append_double_formatted_like_ak(output, value.as_f64());
                output.push(b')');
            } else if value.is_bigint() {
                output.extend_from_slice(b"BigInt(");
                Utf16View::of_string(&value.as_bigint().to_utf16_string()).append_as_wtf8_to(output);
                output.push(b')');
            } else if value.is_string() {
                output.extend_from_slice(b"String(\"");
                Utf16View::of_string(&value.as_string().utf16_string()).append_as_wtf8_to(output);
                output.extend_from_slice(b"\")");
            } else if value.is_undefined() {
                output.extend_from_slice(b"Undefined");
            } else if value.is_null() {
                output.extend_from_slice(b"Null");
            } else {
                output.extend_from_slice(b"Value(");
                append_value_formatted_like_ak(output, value);
                output.push(b')');
            }
            output.extend_from_slice(RESET.as_bytes());
            output.push(b'\n');
        }
    }
}

/// What AK's Formatter<double> writes for `value` when no precision is given: the shortest digits that round-trip,
/// laid out as Number::toString lays them out, except that NaN and the infinities are spelled nan, inf and -inf.
pub(crate) fn append_double_formatted_like_ak(output: &mut Vec<u8>, value: f64) {
    if value.is_nan() {
        output.extend_from_slice(b"nan");
    } else if value.is_infinite() {
        output.extend_from_slice(if value < 0.0 { b"-inf" } else { b"inf" });
    } else {
        output.extend_from_slice(number_to_string(value).as_bytes());
    }
}

/// What Formatter<JS::Value> writes: the value converted to a string without side effects.
pub(crate) fn append_value_formatted_like_ak(output: &mut Vec<u8>, value: Value) {
    if value.is_empty() {
        output.extend_from_slice(b"<empty>");
        return;
    }
    Utf16View::of_string(&value.to_utf16_string_without_side_effects()).append_as_wtf8_to(output);
}

fn constant_value(vm: &Vm, constant: &ConstantValue) -> Value {
    match constant {
        ConstantValue::Number(number) => Value::from_f64(*number),
        ConstantValue::Boolean(boolean) => Value::from_bool(*boolean),
        ConstantValue::Null => Value::NULL,
        ConstantValue::Undefined => Value::UNDEFINED,
        ConstantValue::Empty => Value::EMPTY,
        ConstantValue::String(string) => {
            Value::from_string(PrimitiveString::create(vm, ak::Utf16String::from_utf16(&string.0)))
        }
        ConstantValue::BigInt(literal) => Value::from_bigint(BigInt::create(vm, parse_big_int_literal(literal))),
        ConstantValue::WellKnownSymbol(WellKnownSymbolKind::SymbolIterator) => {
            Value::from_symbol(vm.well_known_symbols().iterator)
        }
        ConstantValue::WellKnownSymbol(WellKnownSymbolKind::SymbolAsyncIterator) => {
            Value::from_symbol(vm.well_known_symbols().async_iterator)
        }
        ConstantValue::AbstractOperation(operation) => {
            let intrinsics = vm
                .current_realm()
                .expect("an executable that calls abstract operations is created in a realm")
                .intrinsics();
            let function = match operation {
                AbstractOperationKind::ArraySpeciesCreate => {
                    return Value::from_object(intrinsics.array_species_create_abstract_operation_function(vm));
                }
                AbstractOperationKind::AsyncIteratorClose => {
                    intrinsics.async_iterator_close_abstract_operation_function(vm)
                }
                AbstractOperationKind::GetMethod => intrinsics.get_method_abstract_operation_function(vm),
                AbstractOperationKind::GetIteratorDirect => {
                    intrinsics.get_iterator_direct_abstract_operation_function(vm)
                }
                AbstractOperationKind::GetIteratorFromMethod => {
                    intrinsics.get_iterator_from_method_abstract_operation_function(vm)
                }
                AbstractOperationKind::IteratorComplete => intrinsics.iterator_complete_abstract_operation_function(vm),
            };
            Value::from_object(function)
        }
    }
}

/// The value of a BigInt literal without its `n` suffix, in any of the radixes a literal can use.
fn parse_big_int_literal(literal: &str) -> SignedBigInteger {
    let bytes = literal.as_bytes();
    let (radix, digits) = match bytes {
        [b'0', b'x' | b'X', _, ..] => (16, &bytes[2..]),
        [b'0', b'o' | b'O', _, ..] => (8, &bytes[2..]),
        [b'0', b'b' | b'B', _, ..] => (2, &bytes[2..]),
        _ => (10, bytes),
    };
    SignedBigInteger::parse_bytes(digits, radix).expect("the frontend only emits valid BigInt literals")
}

// SAFETY: Visits the constants, the for-in key snapshots, the environment shapes, the template object caches, the
// functions the bytecode creates and the literal values of its classes, which are all the cells an executable keeps
// alive so far. The inline caches do not keep the shapes and objects they remember alive.
unsafe impl Trace for Executable {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(&self.constants);
        self.property_keys.trace(visitor);
        for cache in &self.object_property_iterator_caches {
            cache.data.trace(visitor);
        }
        self.environment_shape_caches.trace(visitor);
        self.template_object_caches.trace(visitor);
        self.shared_function_data.trace(visitor);
        self.class_blueprints.trace(visitor);
    }
}
