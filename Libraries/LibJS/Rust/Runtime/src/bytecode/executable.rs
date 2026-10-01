/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use std::rc::Rc;

use crate::bytecode::class_blueprint::ClassBlueprint;
use crate::frontend_host::rust_free_compiled_regex;
use crate::gc::class::{GcCell, define_cell};
use crate::gc::heap::cell_is_dead;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::buffer::InterpreterBuffer;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::executable::ExecutableHead;
use crate::layout::object::Object;
pub use crate::layout::property_lookup_cache::{
    EnvironmentCoordinate, GlobalVariableCache, PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK, PropertyLookupCache,
    PropertyLookupCacheEntry, PropertyLookupCacheEntryType,
};
use crate::layout::shape::{PrototypeChainValidity, Shape};
use crate::layout::value::Value;
use crate::runtime::big_int::{BigInt, SignedBigInteger};
use crate::runtime::environment_shape::{EnvironmentShape, EnvironmentShapeCache};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::primitive_string::u64_hash;
use crate::runtime::shared_function_instance_data::SharedFunctionInstanceData;
use crate::source_code::SourceCode;
use libjs_rust::bytecode::constant::WellKnownSymbolKind;
use libjs_rust::bytecode::executable::ExecutableData;
use libjs_rust::bytecode::generator::{ConstantValue, ExceptionHandler};

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
        if self.entry_type != other.entry_type {
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
        self.from_shape.is_some_and(cell_is_dead)
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
}

const _: () = assert!(align_of::<MonomorphicData>() > PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
const _: () = assert!(align_of::<PolymorphicData>() > PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
const _: () = assert!(align_of::<MegamorphicData>() > PROPERTY_LOOKUP_CACHE_DATA_TAG_MASK);
const _: () = assert!(core::mem::offset_of!(MonomorphicData, entry) == 0);
const _: () = assert!(core::mem::offset_of!(PolymorphicData, entries) == 0);
const _: () = assert!(core::mem::offset_of!(MegamorphicData, entry) == 0);

fn megamorphic_primary_index(shape: Gc<Shape>) -> usize {
    let hash = u64_hash(shape.as_ptr().addr() as u64) as usize;
    hash & (MEGAMORPHIC_PRIMARY_CACHE_SIZE - 1)
}

fn megamorphic_secondary_index(shape: Gc<Shape>) -> usize {
    let hash = u64_hash(shape.as_ptr().addr() as u64) as usize;
    (hash >> 8) & (MEGAMORPHIC_SECONDARY_CACHE_SIZE - 1)
}

fn insert_megamorphic_entry(data: &MegamorphicData, entry: &PropertyLookupCacheEntryData) {
    let lookup_shape = entry.lookup_shape().expect("a megamorphic entry has a lookup shape");

    let primary_entry = &data.primary_entries[megamorphic_primary_index(lookup_shape)];
    let primary = primary_entry.get();
    if let Some(displaced_shape) = primary.lookup_shape()
        && displaced_shape != lookup_shape
    {
        data.secondary_entries[megamorphic_secondary_index(displaced_shape)].set(primary);
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

    pub fn first_entry(&self) -> Option<PropertyLookupCacheEntryData> {
        if let Some(data) = self.monomorphic_data() {
            return Some(data.entry.get());
        }
        if let Some(data) = self.polymorphic_data() {
            return Some(data.entries[0].get());
        }
        if let Some(data) = self.megamorphic_data() {
            return Some(data.entry.get());
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

    /// The entries that may be for `shape`. A megamorphic cache finds the one for the shape and moves it first, where
    /// the interpreter looks.
    pub fn entries_for_shape(&self, shape: Gc<Shape>) -> PropertyLookupCacheEntries {
        let Some(data) = self.megamorphic_data() else {
            return Self::copy_entries(self.entries());
        };

        let find_entry = |entries: &[PropertyLookupCacheEntry], index: usize| {
            let entry = entries[index].get();
            (entry.lookup_shape() == Some(shape)).then_some(entry)
        };

        let entry = find_entry(&data.primary_entries, megamorphic_primary_index(shape))
            .or_else(|| find_entry(&data.secondary_entries, megamorphic_secondary_index(shape)));
        let Some(entry) = entry else {
            return Self::copy_entries(&[]);
        };

        data.entry.set(entry);
        Self::copy_entries(core::slice::from_ref(&data.entry))
    }

    /// Records a new entry of `entry_type`, filled in by `callback`, moving the cache to the next tier when it has no
    /// room for it.
    pub fn update(
        &self,
        entry_type: PropertyLookupCacheEntryType,
        callback: impl FnOnce(&mut PropertyLookupCacheEntryData),
    ) {
        let mut new_entry = PropertyLookupCacheEntryData {
            entry_type,
            ..Default::default()
        };
        callback(&mut new_entry);

        if let Some(data) = self.megamorphic_data() {
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
            let new_data = Box::new(MegamorphicData {
                entry: PropertyLookupCacheEntry::new(),
                primary_entries: core::array::from_fn(|_| PropertyLookupCacheEntry::new()),
                secondary_entries: core::array::from_fn(|_| PropertyLookupCacheEntry::new()),
            });
            for entry in entries.iter().rev() {
                let entry = entry.get();
                if entry.lookup_shape().is_some() {
                    insert_megamorphic_entry(&new_data, &entry);
                }
            }
            insert_megamorphic_entry(&new_data, &new_entry);
            new_data.entry.set(new_entry);
            self.clear();
            self.set_data(new_data, MEGAMORPHIC_DATA_TAG);
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

    pub fn clear(&self) {
        let data = self.data.get();
        if data == 0 {
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
    /// Assigns a default GlobalVariableCache, as `cache = {}` does in C++.
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

/// Defines the call sites of the runtime that keep a property lookup cache of their own, the C++ static
/// StaticPropertyLookupCache locals.
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

/// A unit of bytecode: a script, a module, a function body or an eval, with what the interpreter needs to run it.
#[repr(C)]
pub struct Executable {
    pub head: ExecutableHead,
    bytecode: Box<[u8]>,
    constants: Box<[Value]>,
    property_lookup_caches: Box<[PropertyLookupCache]>,
    global_variable_caches: Box<[GlobalVariableCache]>,
    environment_coordinate_caches: Box<[Cell<EnvironmentCoordinate>]>,
    environment_shape_caches: Box<[Cell<Option<Gc<EnvironmentShape>>>]>,
    pub number_of_registers: u32,
    pub number_of_arguments: u32,
    pub is_strict_mode: bool,
    pub identifier_table: Vec<ak::Utf16FlyString>,
    pub property_key_table: Vec<ak::Utf16FlyString>,
    pub string_table: Vec<ak::Utf16FlyString>,
    /// Sorted by start offset, and not overlapping.
    pub exception_handlers: Box<[ExceptionHandler]>,
    /// The functions the bytecode creates, which NewFunction and NewClass refer to by index.
    shared_function_data: Box<[Gc<SharedFunctionInstanceData>]>,
    class_blueprints: Box<[ClassBlueprint]>,
}

define_cell!(Executable, Other);

const _: () = assert!(core::mem::offset_of!(Executable, head) == 0);

/// The cache counts an executable's bytecode refers to.
pub struct ExecutableCacheCounts {
    pub property_lookup_caches: u32,
    pub global_variable_caches: u32,
    pub environment_coordinate_caches: u32,
    pub environment_shape_caches: u32,
}

fn interpreter_buffer<T>(elements: &[T]) -> InterpreterBuffer<T> {
    InterpreterBuffer {
        data: Cell::new(elements.as_ptr().cast_mut()),
        size: Cell::new(elements.len()),
        capacity: Cell::new(elements.len()),
    }
}

impl Executable {
    /// Moves `executable` into the heap and has the VM prune its inline caches after every collection, as C++
    /// executables do as weak containers.
    pub fn create_from_parts(vm: &Vm, executable: Executable) -> Gc<Executable> {
        let executable = vm.heap().allocate(executable);
        vm.register_executable(executable);
        executable
    }

    pub fn property_lookup_cache(&self, index: usize) -> &PropertyLookupCache {
        &self.property_lookup_caches[index]
    }

    /// Forgets the cells the inline caches remember that died in this collection.
    pub fn remove_dead_cells(&self) {
        for cache in &self.property_lookup_caches {
            cache.remove_dead_entries();
        }
        for cache in &self.global_variable_caches {
            cache.entry.clear_if_it_has_a_dead_cell();
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
            bytecode_data: Cell::new(bytecode.as_ptr()),
            bytecode_size: Cell::new(bytecode.len()),
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
            number_of_registers,
            number_of_arguments,
            is_strict_mode,
            identifier_table: Vec::new(),
            property_key_table: Vec::new(),
            string_table: Vec::new(),
            exception_handlers: Box::new([]),
            shared_function_data: Box::new([]),
            class_blueprints: Box::new([]),
        }
    }

    /// Creates the executable for what the frontend compiled from code it does not know the source of.
    pub fn create(vm: &Vm, data: ExecutableData) -> Gc<Executable> {
        Self::create_with_source_code(vm, data, None)
    }

    /// Creates the executable for what the frontend compiled from `source_code`, with the functions and classes it
    /// declares, as ffi::create_executable does for the C++ runtime.
    pub fn create_with_source_code(
        vm: &Vm,
        mut data: ExecutableData,
        source_code: Option<&Rc<SourceCode>>,
    ) -> Gc<Executable> {
        // The shared function data and the literal values of class elements stay rooted until the executable that
        // holds them is allocated.
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
        let rooted_literal_values = MarkedVec::new(vm);
        let class_blueprints: Box<[ClassBlueprint]> = data
            .class_blueprints
            .iter()
            .map(|blueprint| ClassBlueprint::create(vm, blueprint, source_code, &rooted_literal_values))
            .collect();
        let shared_function_data: Box<[Gc<SharedFunctionInstanceData>]> =
            rooted_shared_function_data.to_vec().into_boxed_slice();

        // The regexes were only compiled to report early errors; the runtime compiles them again when it runs.
        for regex in data.compiled_regexes {
            // SAFETY: Each handle came from rust_compile_regex and is freed once.
            unsafe { rust_free_compiled_regex(regex) };
        }
        // The constants stay rooted until the executable that holds them is allocated.
        let rooted_constants = MarkedVec::with_capacity(vm, data.constants.len());
        for constant in &data.constants {
            rooted_constants.push(constant_value(vm, constant));
        }
        let constants: Box<[Value]> = rooted_constants.to_vec().into_boxed_slice();
        let counts = ExecutableCacheCounts {
            property_lookup_caches: data.cache_counts.property_lookup,
            global_variable_caches: data.cache_counts.global_variable,
            environment_coordinate_caches: data.cache_counts.environment_coordinate,
            environment_shape_caches: data.cache_counts.environment_shape,
        };
        let number_of_locals = u32::try_from(data.local_variables.len()).expect("local count fits in u32");
        let mut executable = Self::new(
            data.bytecode.into_boxed_slice(),
            data.number_of_registers,
            number_of_locals,
            data.number_of_arguments,
            constants,
            &counts,
            data.is_strict,
        );
        executable.identifier_table = data.identifier_table;
        executable.property_key_table = data.property_key_table;
        executable.string_table = data.string_table;
        executable.exception_handlers = data.exception_handlers.into_boxed_slice();
        executable.shared_function_data = shared_function_data;
        executable.class_blueprints = class_blueprints;
        let executable = Self::create_from_parts(vm, executable);
        drop(rooted_constants);
        drop(rooted_literal_values);
        drop(rooted_shared_function_data);
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

    pub fn bytecode(&self) -> &[u8] {
        &self.bytecode
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
        ConstantValue::AbstractOperation(_) => unimplemented_runtime_function("abstract operation constants", 0),
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

// SAFETY: Visits the constants, the environment shapes, the functions the bytecode creates and the literal values of
// its classes, which are all the cells an executable keeps alive so far. The inline caches do not keep the shapes and
// objects they remember alive.
unsafe impl Trace for Executable {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(&self.constants);
        self.environment_shape_caches.trace(visitor);
        self.shared_function_data.trace(visitor);
        self.class_blueprints.trace(visitor);
    }
}
