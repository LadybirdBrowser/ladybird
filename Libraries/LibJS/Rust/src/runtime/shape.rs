/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, Ref};
use core::mem::ManuallyDrop;
use core::ops::ControlFlow;
use std::collections::{HashMap, HashSet};

use indexmap::IndexMap;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::gc::sweep::Sweep;
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak::GcWeak;
use crate::interpreter::vm::Vm;
use crate::jit::dependencies::{CellId, Dependency};
use crate::layout::cell::{CellHeader, CellHeaderTail, Gc};
use crate::layout::member::{Member, NullableMember};
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::ObjectPropertyIteratorCacheData;
pub use crate::layout::shape::Shape;
use crate::layout::value::Value;
use crate::runtime::descriptor_array::{DescriptorArray, MAX_DESCRIPTOR_COUNT, PropertyMetadata};
use crate::runtime::object::IntegrityLevel;
use crate::runtime::property_attributes::PropertyAttributes;
use crate::runtime::property_key::{NonSymbolPropertyKey, PropertyKey};
use crate::runtime::realm::Realm;
use crate::runtime::special_property_cache::SpecialPropertyCache;
use crate::runtime::watchpoint_set::WatchpointSet;

type RandomState = foldhash::fast::RandomState;

#[derive(Clone, PartialEq, Eq, Hash)]
pub struct TransitionKey {
    pub property_key: PropertyKey,
    pub attributes: PropertyAttributes,
}

/// The bits of Shape::flags, the flags of a shape that are not its forward transition storage.
mod shape_flag {
    pub const DICTIONARY: u8 = 1 << 0;
    pub const HAS_PARAMETER_MAP: u8 = 1 << 1;
    /// Something relies on the value in a slot of the object of this prototype shape (see
    /// `Shape::watch_slot_for_replacements()`).
    pub const HAS_WATCHED_SLOTS: u8 = 1 << 2;
    /// See `Shape::is_non_extensible()`.
    pub const NON_EXTENSIBLE: u8 = 1 << 3;
    /// The flags a shape gets from the shape it comes from.
    pub const INHERITED: u8 = HAS_PARAMETER_MAP | NON_EXTENSIBLE;
    /// An object may have left the shape (see `Shape::is_stable()`).
    pub const UNSTABLE: u8 = 1 << 6;
    /// JIT code depends on the shape staying stable.
    pub const HAS_DEPENDENT_CODE: u8 = 1 << 7;
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PropertyCountChange {
    Preserve,
    Increment,
    Decrement,
}

type PropertyTable = IndexMap<PropertyKey, PropertyMetadata, RandomState>;
type ForwardTransitionMap = HashMap<TransitionKey, GcWeak<Shape>, RandomState>;

/// The properties of a dictionary shape, which it changes in place. A shape keeps it where other shapes keep their
/// descriptor array.
#[repr(C)]
pub struct DictionaryPropertyTable {
    header: CellHeader,
    table: GcRefCell<PropertyTable>,
}

define_cell!(DictionaryPropertyTable, Other);

// SAFETY: The keys are the only cells a table reaches.
unsafe impl Trace for DictionaryPropertyTable {
    fn trace(&self, visitor: &mut Visitor) {
        for property_key in self.table.borrow().keys() {
            property_key.trace(visitor);
        }
    }
}

/// What only some shapes have. The rest keep at most one forward transition, in the word of ShapeStorage.
struct ShapeExtra {
    forward_transitions: GcRefCell<ForwardTransitionMap>,
    property_iterator_cache: NullableMember<ObjectPropertyIteratorCacheData>,
    /// What Get of the properties that conversions look up gives on the objects of this shape (see
    /// `SpecialPropertyCache`).
    special_property_cache: NullableMember<SpecialPropertyCache>,
    rare_data: GcRefCell<Option<Box<RareData>>>,
}

// SAFETY: Visits the keys of the forward transitions, the caches and the rare data. The shapes of transitions are weak.
unsafe impl Trace for ShapeExtra {
    fn trace(&self, visitor: &mut Visitor) {
        // FIXME: The forward transition keys should be weak, but we have to mark them for now in case they go stale.
        for transition_key in self.forward_transitions.borrow().keys() {
            transition_key.property_key.trace(visitor);
        }
        self.property_iterator_cache.trace(visitor);
        self.special_property_cache.trace(visitor);
        self.rare_data.trace(visitor);
    }
}

#[derive(Default)]
struct RareData {
    /// Keyed by the address of the new prototype, which is only compared. A transition's shape keeps its prototype
    /// alive, so the shape of an entry whose prototype died is dead as well, and the entry is pruned when found.
    prototype_transitions: HashMap<usize, GcWeak<Shape>, RandomState>,
    delete_transitions: HashMap<PropertyKey, GcWeak<Shape>, RandomState>,
    child_prototype_shapes: Vec<GcWeak<Shape>>,
    own_string_keys: Option<OwnStringKeys>,
    assign_copy: Option<AssignCopy>,
    /// The shapes that sealing and freezing objects of this shape give them, by integrity level.
    integrity_level_shapes: [Option<Gc<Shape>>; 2],
    /// The shape that preventing extensions gives objects of this shape.
    non_extensible_transition: Option<GcWeak<Shape>>,
    /// What is known of the replacement of the values in the slots of the object that has this prototype shape, by
    /// offset (see `Shape::watch_slot_for_replacements()`).
    watched_slots: HashMap<u32, WatchedSlot, RandomState>,
    /// The offsets of the slots whose value changed while watched (see `Shape::was_slot_replaced()`).
    replaced_slots: Vec<u32>,
}

/// What a prototype shape knows of the replacement of the value in one of the slots of its object.
enum WatchedSlot {
    /// The set fires when the value changes.
    Watched(Gc<WatchpointSet>),
    /// A property lookup cache may write to the slot, without telling anything.
    WrittenByCaches,
}

/// What Object.assign of an object with a shape onto an empty object makes, as set_cached_assign_copy() keeps it.
#[derive(Clone, Copy)]
pub struct AssignCopy {
    /// The validity of the shape of the prototype of the empty object when the copy was found to be right, which stops
    /// being valid when that prototype changes its properties.
    pub prototype_validity: Gc<WatchpointSet>,
    /// The shape the empty object ends up with.
    pub copy_shape: Gc<Shape>,
}

/// A shape and its dictionary generation, which changes when a dictionary shape changes its properties in place. Shapes
/// that are not dictionaries keep generation 0.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ShapeKey {
    pub shape: Gc<Shape>,
    pub dictionary_generation: u32,
}

impl ShapeKey {
    pub fn of(shape: Gc<Shape>) -> Self {
        Self {
            shape,
            dictionary_generation: shape.dictionary_generation(),
        }
    }
}

/// The string keys of the properties of a shape, as with_own_string_keys() caches them.
struct OwnStringKeys {
    /// The dictionary generation of the shape when the keys were collected. Changing the properties of a shape in
    /// place changes its generation.
    generation: u32,
    keys: Box<[Value]>,
    enumerable: Box<[bool]>,
}

/// The parts of a shape the interpreter does not read: one word that is null, a forward transition (an owned GcWeak)
/// or, tagged, an owned ShapeExtra. The word of a shape with a transition holds no key: a transition that adds a
/// property leads to a shape whose descriptors have the key and the attributes, at the property count of the shape it
/// came from, and they live for as long as the transition is good for anything. The other forward transitions go in
/// ShapeExtra.
pub struct ShapeStorage {
    word: Cell<*mut ()>,
}

const SHAPE_EXTRA_TAG: usize = 1;

// NB: Every shape has these bytes.
const _: () = assert!(size_of::<Shape>() == 40);

enum ShapeStorageState<'a> {
    Empty,
    /// The target of a transition that adds a property, unless it died.
    PutTransition(Option<Gc<Shape>>),
    Extra(&'a ShapeExtra),
}

impl ShapeStorage {
    fn new(_: &DeferGc<'_>) -> Self {
        Self {
            word: Cell::new(core::ptr::null_mut()),
        }
    }

    fn state(&self) -> ShapeStorageState<'_> {
        let word = self.word.get();
        if word.is_null() {
            return ShapeStorageState::Empty;
        }
        if word.addr() & SHAPE_EXTRA_TAG != 0 {
            // SAFETY: A tagged word is a box this storage owns, which it keeps until it is dropped.
            return ShapeStorageState::Extra(unsafe {
                &*word.map_addr(|address| address & !SHAPE_EXTRA_TAG).cast::<ShapeExtra>()
            });
        }
        // SAFETY: Any other word is a weak reference this storage owns, which stays with it.
        let target = ManuallyDrop::new(unsafe { GcWeak::<Shape>::from_raw(word) });
        ShapeStorageState::PutTransition(target.get())
    }

    /// Replaces the word of a storage that has no extra with a new one, giving up the weak reference it owned.
    fn replace_put_transition(&self, word: *mut ()) {
        let old_word = self.word.replace(word);
        assert!(old_word.addr() & SHAPE_EXTRA_TAG == 0);
        if !old_word.is_null() {
            // SAFETY: The word was a weak reference this storage owned, which it no longer holds.
            drop(unsafe { GcWeak::<Shape>::from_raw(old_word) });
        }
    }

    /// A new weak reference to the target of the forward transition in the word.
    fn clone_put_transition(&self) -> GcWeak<Shape> {
        let word = self.word.get();
        assert!(!word.is_null() && word.addr() & SHAPE_EXTRA_TAG == 0);
        // SAFETY: Any other word is a weak reference this storage owns, which stays with it.
        (*ManuallyDrop::new(unsafe { GcWeak::<Shape>::from_raw(word) })).clone()
    }

    fn set_put_transition(&self, target: GcWeak<Shape>) {
        self.replace_put_transition(target.into_raw());
    }

    fn clear_put_transition(&self) {
        self.replace_put_transition(core::ptr::null_mut());
    }

    #[cfg_attr(
        ladybird_gc_lints,
        expect(
            gc_container_on_stack,
            reason = "the caller allocates nothing while it holds the box"
        )
    )]
    fn set_extra(&self, extra: Box<ShapeExtra>) -> &ShapeExtra {
        let extra = Box::into_raw(extra).cast::<()>();
        self.replace_put_transition(extra.map_addr(|address| address | SHAPE_EXTRA_TAG));
        // SAFETY: This storage owns the box from here on, and keeps it until it is dropped.
        unsafe { &*extra.cast::<ShapeExtra>() }
    }
}

impl Drop for ShapeStorage {
    fn drop(&mut self) {
        let word = self.word.get();
        if word.is_null() {
            return;
        }
        if word.addr() & SHAPE_EXTRA_TAG != 0 {
            // SAFETY: A tagged word is a box this storage owns.
            drop(unsafe { Box::from_raw(word.map_addr(|address| address & !SHAPE_EXTRA_TAG).cast::<ShapeExtra>()) });
        } else {
            // SAFETY: Any other word is a weak reference this storage owns.
            drop(unsafe { GcWeak::<Shape>::from_raw(word) });
        }
    }
}

// SAFETY: Visits what the extra reaches. The shape of a transition in the word is weak.
unsafe impl Trace for ShapeStorage {
    fn trace(&self, visitor: &mut Visitor) {
        if let ShapeStorageState::Extra(extra) = self.state() {
            extra.trace(visitor);
        }
    }
}

// SAFETY: Visits the keys.
unsafe impl Trace for OwnStringKeys {
    fn trace(&self, visitor: &mut Visitor) {
        visitor.visit_values(&self.keys);
    }
}

// SAFETY: Visits the keys of delete transitions, the own string keys, the assign copy, the shapes of integrity levels and
// the watchpoint sets of slots. The shapes of transitions and child prototype shapes are weak, and so are the keys of
// prototype transitions, which are only compared.
unsafe impl Trace for RareData {
    fn trace(&self, visitor: &mut Visitor) {
        // FIXME: The delete transition keys should be weak, but we have to mark them for now in case they go stale.
        for property_key in self.delete_transitions.keys() {
            property_key.trace(visitor);
        }
        self.own_string_keys.trace(visitor);
        if let Some(assign_copy) = &self.assign_copy {
            visitor.visit(assign_copy.prototype_validity);
            visitor.visit(assign_copy.copy_shape);
        }
        for shape in self.integrity_level_shapes.iter().flatten() {
            visitor.visit(*shape);
        }
        for watched_slot in self.watched_slots.values() {
            if let WatchedSlot::Watched(watchpoint_set) = watched_slot {
                visitor.visit(*watchpoint_set);
            }
        }
    }
}

define_cell!(Shape, Other);

// SAFETY: Visits the realm, the prototype, the validity, the previous shape, the descriptors or property table and what
// the storage owns.
unsafe impl Trace for Shape {
    fn trace(&self, visitor: &mut Visitor) {
        self.realm.trace(visitor);
        self.prototype.trace(visitor);
        self.prototype_chain_validity.trace(visitor);
        self.previous.trace(visitor);
        self.property_storage.trace(visitor);
        self.storage.trace(visitor);
    }
}

fn prototype_address(prototype: Option<Gc<Object>>) -> usize {
    prototype.map_or(0, |prototype| prototype.as_ptr().addr())
}

/// A prototype transition the VM's cache of them remembers: from shape `from` to prototype `prototype` (see
/// prototype_address()) leads to `target`.
#[derive(Clone, Copy, Default)]
struct PrototypeTransition {
    from: Option<Gc<Shape>>,
    prototype: usize,
    target: Option<Gc<Shape>>,
}

const PROTOTYPE_TRANSITION_CACHE_INDEX_BITS: u32 = 8;

/// The prototype transitions made most recently, by the hash of the shape and the prototype, in front of the
/// transition tables of shapes. Objects are made with a prototype all the time (arrays, regular expression results,
/// instances of classes), and their shapes come from the realm's empty object shape, whose table of prototype
/// transitions has an entry for every prototype objects were ever made with.
///
/// Like the transition tables, the cache does not keep its shapes alive: the VM's sweep callback forgets transitions
/// whose shapes died. A target shape keeps its prototype alive, so the prototype of a remembered transition is alive.
pub struct PrototypeTransitionCache {
    entries: [Cell<PrototypeTransition>; 1 << PROTOTYPE_TRANSITION_CACHE_INDEX_BITS],
}

impl PrototypeTransitionCache {
    pub fn new() -> Self {
        Self {
            entries: core::array::from_fn(|_| Cell::new(PrototypeTransition::default())),
        }
    }

    fn entry(&self, from: Gc<Shape>, prototype: usize) -> &Cell<PrototypeTransition> {
        let hash = (from.as_ptr().addr() as u32 ^ prototype as u32).wrapping_mul(0x9e37_79b9);
        &self.entries[(hash >> (32 - PROTOTYPE_TRANSITION_CACHE_INDEX_BITS)) as usize]
    }

    fn get(&self, from: Gc<Shape>, prototype: usize) -> Option<Gc<Shape>> {
        let transition = self.entry(from, prototype).get();
        (transition.from == Some(from) && transition.prototype == prototype)
            .then_some(transition.target)
            .flatten()
    }

    fn set(&self, from: Gc<Shape>, prototype: usize, target: Gc<Shape>) {
        self.entry(from, prototype).set(PrototypeTransition {
            from: Some(from),
            prototype,
            target: Some(target),
        });
    }

    pub fn remove_dead_entries(&self, sweep: &Sweep<'_>) {
        for entry in &self.entries {
            let transition = entry.get();
            let is_dead = |shape| sweep.is_dead(shape);
            if transition.from.is_some_and(is_dead) || transition.target.is_some_and(is_dead) {
                entry.set(PrototypeTransition::default());
            }
        }
    }
}

impl Default for PrototypeTransitionCache {
    fn default() -> Self {
        Self::new()
    }
}

impl Shape {
    fn new(defer_gc: &DeferGc<'_>, realm: Gc<Realm>) -> Self {
        Self {
            realm: Member::new(realm),
            prototype: NullableMember::null(),
            header: CellHeaderTail::for_class(Self::CLASS),
            flags: Cell::new(0),
            property_count: Cell::new(0),
            prototype_chain_validity: NullableMember::null(),
            previous: NullableMember::null(),
            dictionary_generation: Cell::new(0),
            property_storage: NullableMember::null(),
            storage: ShapeStorage::new(defer_gc),
        }
    }

    fn new_from_previous_shape(
        defer_gc: &DeferGc<'_>,
        previous_shape: &Shape,
        property_count_change: PropertyCountChange,
    ) -> Self {
        let shape = Self::new(defer_gc, previous_shape.realm());
        shape.flags.set(previous_shape.flags.get() & shape_flag::INHERITED);
        shape.prototype.set(previous_shape.prototype());
        shape.previous.set(Some(previous_shape.as_gc()));
        let mut property_count = previous_shape.property_count();
        match property_count_change {
            PropertyCountChange::Preserve => {}
            PropertyCountChange::Increment => {
                assert!(property_count < u32::MAX);
                property_count += 1;
            }
            PropertyCountChange::Decrement => {
                assert!(property_count > 0);
                property_count -= 1;
            }
        }
        shape.property_count.set(property_count);
        shape
    }

    fn new_from_previous_shape_with_prototype(
        defer_gc: &DeferGc<'_>,
        previous_shape: &Shape,
        realm: Gc<Realm>,
        new_prototype: Option<Gc<Object>>,
    ) -> Self {
        let shape = Self::new(defer_gc, realm);
        shape.flags.set(previous_shape.flags.get() & shape_flag::INHERITED);
        shape.prototype.set(new_prototype);
        // NB: Unlike the other transitions, this one does not keep the shape it came from alive. An object that changes
        //     its prototype time and again (like a WindowProxy as its browsing context navigates) would otherwise keep
        //     every prototype it ever had alive, and with them the realms they belong to.
        shape.property_count.set(previous_shape.property_count());
        shape
    }

    /// A new unique shape with no properties and no prototype.
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Shape> {
        vm.heap().allocate(|defer_gc| Self::new(defer_gc, realm))
    }

    fn as_gc(&self) -> Gc<Shape> {
        Gc::from_ref(self)
    }

    fn descriptors(&self) -> Option<Gc<DescriptorArray>> {
        assert!(!self.is_dictionary());
        self.property_storage.get()
    }

    fn set_descriptors(&self, descriptors: Option<Gc<DescriptorArray>>) {
        assert!(!self.is_dictionary());
        self.property_storage.set(descriptors);
    }

    fn dictionary_property_table(&self) -> Gc<DictionaryPropertyTable> {
        assert!(self.is_dictionary());
        let cell = self
            .property_storage
            .get()
            .expect("a dictionary shape has a property table");
        // SAFETY: What a dictionary shape keeps there is its table, as become_dictionary_shape() put it.
        unsafe { Gc::from_non_null(cell.as_non_null().cast()) }
    }

    fn with_property_table<R>(&self, vm: &Vm, callback: impl FnOnce(&mut PropertyTable) -> R) -> R {
        let property_table = self.dictionary_property_table();
        callback(&mut property_table.table.borrow_mut(vm))
    }

    fn become_dictionary_shape(&self, vm: &Vm) {
        assert!(!self.is_dictionary());
        let property_table = vm.heap().allocate(|_| DictionaryPropertyTable {
            header: CellHeader::for_class(DictionaryPropertyTable::CLASS),
            table: GcRefCell::new(PropertyTable::default()),
        });
        // SAFETY: Only the dictionary flag set below reads the field as anything other than descriptors, and it reads
        //         it as the table it is.
        self.property_storage
            .set(Some(unsafe { Gc::from_non_null(property_table.as_non_null().cast()) }));
        self.flags.set(self.flags.get() | shape_flag::DICTIONARY);
    }

    fn extra(&self) -> Option<&ShapeExtra> {
        match self.storage.state() {
            ShapeStorageState::Extra(extra) => Some(extra),
            ShapeStorageState::Empty | ShapeStorageState::PutTransition(_) => None,
        }
    }

    /// The extra of this shape, which it gets now if it had none, and which takes over its forward transition.
    fn ensure_extra(&self) -> &ShapeExtra {
        #[cfg_attr(
            ladybird_gc_lints,
            expect(gc_container_on_stack, reason = "nothing is allocated before the shape owns the map")
        )]
        let mut forward_transitions = ForwardTransitionMap::default();
        match self.storage.state() {
            ShapeStorageState::Extra(extra) => return extra,
            ShapeStorageState::Empty | ShapeStorageState::PutTransition(None) => {}
            ShapeStorageState::PutTransition(Some(target)) => {
                let (property_key, attributes) = target
                    .descriptors()
                    .expect("the target of a transition has descriptors")
                    .property_at_enum_index(self.property_count());
                forward_transitions.insert(
                    TransitionKey {
                        property_key,
                        attributes,
                    },
                    self.storage.clone_put_transition(),
                );
            }
        }
        self.storage.set_extra(Box::new(ShapeExtra {
            forward_transitions: GcRefCell::new(forward_transitions),
            property_iterator_cache: NullableMember::null(),
            special_property_cache: NullableMember::null(),
            rare_data: GcRefCell::new(None),
        }))
    }

    /// The rare data of this shape, if it has any.
    fn rare_data(&self) -> Option<Ref<'_, RareData>> {
        Ref::filter_map(self.extra()?.rare_data.borrow(), |rare_data| rare_data.as_deref()).ok()
    }

    fn with_existing_rare_data<R>(&self, vm: &Vm, callback: impl FnOnce(&mut RareData) -> R) -> Option<R> {
        let mut rare_data = self.extra()?.rare_data.borrow_mut(vm);
        Some(callback(rare_data.as_deref_mut()?))
    }

    fn with_rare_data<R>(&self, vm: &Vm, callback: impl FnOnce(&mut RareData) -> R) -> R {
        let mut rare_data = self.ensure_extra().rare_data.borrow_mut(vm);
        callback(rare_data.get_or_insert_with(Box::default))
    }

    pub fn create_dictionary_transition(&self, vm: &Vm) -> Gc<Shape> {
        self.mark_unstable(vm);
        let new_shape = Self::create(vm, self.realm());
        new_shape.become_dictionary_shape(vm);
        new_shape
            .flags
            .set(new_shape.flags.get() | (self.flags.get() & shape_flag::INHERITED));
        new_shape.prototype.set(self.prototype());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        self.copy_properties_to_dictionary_shape(vm, &new_shape);
        new_shape
    }

    /// How many properties objects with the shape got after it at most, up to `limit`: the length of the longest chain of
    /// forward transitions from it, looking at no more than `limit` shapes in all.
    pub fn longest_forward_transition_chain(&self, limit: u32) -> u32 {
        let mut budget = limit;
        self.longest_forward_transition_chain_within(limit, &mut budget)
    }

    fn longest_forward_transition_chain_within(&self, depth_limit: u32, budget: &mut u32) -> u32 {
        if depth_limit == 0 || *budget == 0 {
            return 0;
        }
        *budget -= 1;
        let mut longest = 0;
        let mut reaches_depth_limit = |target: Gc<Shape>| {
            let length = 1 + target.longest_forward_transition_chain_within(depth_limit - 1, budget);
            longest = longest.max(length);
            longest >= depth_limit
        };
        match self.storage.state() {
            ShapeStorageState::Empty | ShapeStorageState::PutTransition(None) => {}
            ShapeStorageState::PutTransition(Some(target)) => {
                reaches_depth_limit(target);
            }
            ShapeStorageState::Extra(extra) => {
                for target in extra.forward_transitions.borrow().values().filter_map(GcWeak::get) {
                    if reaches_depth_limit(target) {
                        break;
                    }
                }
            }
        }
        longest
    }

    fn get_or_prune_cached_forward_transition(&self, vm: &Vm, key: &TransitionKey) -> Option<Gc<Shape>> {
        if self.is_prototype_shape() {
            return None;
        }
        match self.storage.state() {
            ShapeStorageState::Empty => None,
            ShapeStorageState::PutTransition(None) => {
                // The cached forward transition has gone stale (from garbage collection). Prune it.
                self.storage.clear_put_transition();
                None
            }
            ShapeStorageState::PutTransition(Some(target)) => {
                let descriptors = target
                    .descriptors()
                    .expect("the target of a transition has descriptors");
                descriptors
                    .has_property_at_enum_index(self.property_count(), &key.property_key, key.attributes)
                    .then_some(target)
            }
            ShapeStorageState::Extra(extra) => {
                let mut forward_transitions = extra.forward_transitions.borrow_mut(vm);
                let target = forward_transitions.get(key)?;
                let Some(shape) = target.get() else {
                    // The cached forward transition has gone stale (from garbage collection). Prune it.
                    forward_transitions.remove(key);
                    return None;
                };
                Some(shape)
            }
        }
    }

    /// Remembers a transition that adds a property, which the word of the storage can hold when it is the only one.
    fn cache_put_transition(&self, vm: &Vm, key: TransitionKey, shape: Gc<Shape>) {
        assert!(!self.is_prototype_shape());
        match self.storage.state() {
            ShapeStorageState::Empty | ShapeStorageState::PutTransition(None) => {
                self.storage.set_put_transition(GcWeak::new(vm.heap(), shape));
            }
            ShapeStorageState::PutTransition(Some(_)) | ShapeStorageState::Extra(_) => {
                self.cache_forward_transition(vm, key, shape);
            }
        }
    }

    fn cache_forward_transition(&self, vm: &Vm, key: TransitionKey, shape: Gc<Shape>) {
        assert!(!self.is_prototype_shape());
        let target = GcWeak::new(vm.heap(), shape);
        self.ensure_extra()
            .forward_transitions
            .borrow_mut(vm)
            .insert(key, target);
    }

    fn get_or_prune_cached_delete_transition(&self, vm: &Vm, key: &PropertyKey) -> Option<Gc<Shape>> {
        if self.is_prototype_shape() {
            return None;
        }
        self.with_existing_rare_data(vm, |rare_data| {
            let target = rare_data.delete_transitions.get(key)?;
            let Some(shape) = target.get() else {
                // The cached delete transition has gone stale (from garbage collection). Prune it.
                rare_data.delete_transitions.remove(key);
                return None;
            };
            Some(shape)
        })
        .flatten()
    }

    fn get_or_prune_cached_prototype_transition(&self, vm: &Vm, prototype: Option<Gc<Object>>) -> Option<Gc<Shape>> {
        if self.is_prototype_shape() {
            return None;
        }
        let key = prototype_address(prototype);
        self.with_existing_rare_data(vm, |rare_data| {
            let target = rare_data.prototype_transitions.get(&key)?;
            let Some(shape) = target.get() else {
                // The cached prototype transition has gone stale (from garbage collection). Prune it.
                rare_data.prototype_transitions.remove(&key);
                return None;
            };
            Some(shape)
        })
        .flatten()
    }

    pub fn create_put_transition(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
    ) -> Gc<Shape> {
        self.mark_unstable(vm);
        // OPTIMIZATION: Most shapes have one transition, which most additions of a property take, and which needs no
        // key to look at.
        if !self.is_prototype_shape()
            && let ShapeStorageState::PutTransition(Some(target)) = self.storage.state()
            && target
                .descriptors()
                .expect("the target of a transition has descriptors")
                .has_property_at_enum_index(self.property_count(), property_key, attributes)
        {
            return target;
        }
        let key = TransitionKey {
            property_key: property_key.clone(),
            attributes,
        };
        if let Some(existing_shape) = self.get_or_prune_cached_forward_transition(vm, &key) {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(|defer_gc| Self::new_from_previous_shape(defer_gc, self, PropertyCountChange::Increment));

        let property_count = self.property_count();
        let metadata = PropertyMetadata {
            offset: property_count,
            attributes,
        };
        match self.descriptors() {
            Some(descriptors) if descriptors.size() == property_count => {
                descriptors.set(vm, property_key, metadata, property_count);
                new_shape.set_descriptors(Some(descriptors));
            }
            _ => {
                let descriptors = self.copy_descriptors(vm);
                new_shape.set_descriptors(Some(descriptors));
                descriptors.set(vm, property_key, metadata, property_count);
            }
        }
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        if !self.is_prototype_shape() {
            self.cache_put_transition(vm, key, new_shape);
        }
        new_shape
    }

    pub fn create_configure_transition(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
    ) -> Gc<Shape> {
        self.mark_unstable(vm);
        let key = TransitionKey {
            property_key: property_key.clone(),
            attributes,
        };
        if let Some(existing_shape) = self.get_or_prune_cached_forward_transition(vm, &key) {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(|defer_gc| Self::new_from_previous_shape(defer_gc, self, PropertyCountChange::Preserve));
        let descriptors = self.copy_descriptors(vm);
        new_shape.set_descriptors(Some(descriptors));
        descriptors.set_attributes(vm, property_key, attributes, self.property_count());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        if !self.is_prototype_shape() {
            self.cache_forward_transition(vm, key, new_shape);
        }
        new_shape
    }

    pub fn create_prototype_transition(&self, vm: &Vm, new_prototype: Option<Gc<Object>>) -> Gc<Shape> {
        self.mark_unstable(vm);
        // NB: Dictionary shapes change in place, and prototype shapes have no cached transitions.
        let cache = (!self.is_dictionary() && !self.is_prototype_shape()).then(|| vm.prototype_transition_cache());
        let prototype = prototype_address(new_prototype);
        if let Some(existing_shape) = cache.and_then(|cache| cache.get(self.as_gc(), prototype)) {
            return existing_shape;
        }
        if let Some(existing_shape) = self.get_or_prune_cached_prototype_transition(vm, new_prototype) {
            if let Some(cache) = cache {
                cache.set(self.as_gc(), prototype, existing_shape);
            }
            return existing_shape;
        }
        let new_shape = self.build_prototype_transition(vm, self.realm(), new_prototype);
        if !self.is_prototype_shape() {
            let target = GcWeak::new(vm.heap(), new_shape);
            self.with_rare_data(vm, |rare_data| {
                rare_data.prototype_transitions.insert(prototype, target)
            });
        }
        if let Some(cache) = cache {
            cache.set(self.as_gc(), prototype, new_shape);
        }
        new_shape
    }

    /// Like create_prototype_transition(), for an object that moves to `realm`. The transition caches are keyed without
    /// the realm, so the result is not cached.
    pub fn create_prototype_transition_into_realm(
        &self,
        vm: &Vm,
        realm: Gc<Realm>,
        new_prototype: Option<Gc<Object>>,
    ) -> Gc<Shape> {
        self.mark_unstable(vm);
        self.build_prototype_transition(vm, realm, new_prototype)
    }

    fn build_prototype_transition(&self, vm: &Vm, realm: Gc<Realm>, new_prototype: Option<Gc<Object>>) -> Gc<Shape> {
        if let Some(new_prototype) = new_prototype {
            new_prototype.convert_to_prototype_if_needed(vm);
        }
        let new_shape = vm
            .heap()
            .allocate(|defer_gc| Self::new_from_previous_shape_with_prototype(defer_gc, self, realm, new_prototype));
        if self.is_dictionary() && self.property_count() > MAX_DESCRIPTOR_COUNT {
            new_shape.become_dictionary_shape(vm);
            self.copy_properties_to_dictionary_shape(vm, &new_shape);
        } else if self.is_dictionary() {
            new_shape.set_descriptors(Some(self.copy_descriptors(vm)));
        } else {
            new_shape.set_descriptors(self.descriptors());
        }
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        new_shape
    }

    pub fn create_delete_transition(&self, vm: &Vm, property_key: &PropertyKey) -> Gc<Shape> {
        self.mark_unstable(vm);
        if let Some(existing_shape) = self.get_or_prune_cached_delete_transition(vm, property_key) {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(|defer_gc| Self::new_from_previous_shape(defer_gc, self, PropertyCountChange::Decrement));
        let descriptors = self.copy_descriptors(vm);
        new_shape.set_descriptors(Some(descriptors));
        descriptors.remove(vm, property_key, self.property_count());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        let target = GcWeak::new(vm.heap(), new_shape);
        self.with_rare_data(vm, |rare_data| {
            rare_data.delete_transitions.insert(property_key.clone(), target)
        });
        new_shape
    }

    /// The shape objects of this shape get when they stop being extensible, which has the same properties.
    pub fn create_non_extensible_transition(&self, vm: &Vm) -> Gc<Shape> {
        assert!(!self.is_dictionary() && !self.is_non_extensible());
        self.mark_unstable(vm);
        if !self.is_prototype_shape()
            && let Some(existing_shape) = self
                .rare_data()
                .and_then(|rare_data| rare_data.non_extensible_transition.as_ref()?.get())
        {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(|defer_gc| Self::new_from_previous_shape(defer_gc, self, PropertyCountChange::Preserve));
        new_shape.flags.set(new_shape.flags.get() | shape_flag::NON_EXTENSIBLE);
        new_shape.set_descriptors(self.descriptors());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        if !self.is_prototype_shape() {
            let target = GcWeak::new(vm.heap(), new_shape);
            self.with_rare_data(vm, |rare_data| rare_data.non_extensible_transition = Some(target));
        }
        new_shape
    }

    /// Like create_non_extensible_transition(), for a dictionary shape, which changes in place.
    pub fn set_non_extensible_without_transition(&self, vm: &Vm) {
        self.invalidate_prototype_if_needed_for_change_without_transition(vm);
        assert!(self.is_dictionary());
        self.flags.set(self.flags.get() | shape_flag::NON_EXTENSIBLE);
        self.increment_dictionary_generation();
    }

    fn ensure_descriptor_array(&self, vm: &Vm) {
        assert!(!self.is_dictionary());
        if self.descriptors().is_some() {
            return;
        }
        self.set_descriptors(Some(DescriptorArray::create(vm)));
    }

    fn copy_descriptors(&self, vm: &Vm) -> Gc<DescriptorArray> {
        assert!(self.property_count() <= MAX_DESCRIPTOR_COUNT);
        if !self.is_dictionary()
            && let Some(descriptors) = self.descriptors()
        {
            return DescriptorArray::create_copy(vm, &descriptors, self.property_count());
        }

        let descriptors = DescriptorArray::create(vm);
        self.for_each_property_in_insertion_order(|property_key, metadata| {
            descriptors.set(vm, property_key, metadata, descriptors.size());
            ControlFlow::Continue(())
        });
        descriptors
    }

    fn copy_properties_to_dictionary_shape(&self, vm: &Vm, shape: &Shape) {
        assert!(shape.is_dictionary());
        let property_count = shape.with_property_table(vm, |property_table| {
            self.for_each_property_in_insertion_order(|property_key, metadata| {
                property_table.insert(property_key.clone(), metadata);
                ControlFlow::Continue(())
            });
            property_table.len()
        });
        shape
            .property_count
            .set(u32::try_from(property_count).expect("the property count fits in u32"));
    }

    pub fn add_property_without_transition(&self, vm: &Vm, property_key: &PropertyKey, attributes: PropertyAttributes) {
        self.invalidate_prototype_if_needed_for_change_without_transition(vm);
        let property_count = self.property_count();
        let metadata = PropertyMetadata {
            offset: property_count,
            attributes,
        };
        if self.is_dictionary() {
            let inserted_new_entry = self.with_property_table(vm, |property_table| {
                property_table.insert(property_key.clone(), metadata).is_none()
            });
            if inserted_new_entry {
                assert!(property_count < u32::MAX);
                self.property_count.set(property_count + 1);
                self.increment_dictionary_generation();
            }
            return;
        }

        // NB: Only the premade shapes of intrinsics get properties without transitions when they are not
        //     dictionaries, before any object has them, so nothing can have cached them. Their dictionary generation
        //     stays 0 like that of every other shape that is no dictionary, which caches compare it with.
        self.ensure_descriptor_array(vm);
        let descriptors = self.descriptors().expect("the shape has a descriptor array");
        if descriptors.lookup(property_key, property_count).is_none() {
            assert!(property_count < u32::MAX);
            descriptors.set(vm, property_key, metadata, property_count);
            self.property_count.set(property_count + 1);
            return;
        }
        descriptors.set(vm, property_key, metadata, property_count);
    }

    pub fn set_property_attributes_without_transition(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
    ) {
        self.invalidate_prototype_if_needed_for_change_without_transition(vm);
        assert!(self.is_dictionary());
        self.with_property_table(vm, |property_table| {
            property_table
                .get_mut(property_key)
                .expect("the dictionary has the property")
                .attributes = attributes;
        });
        self.increment_dictionary_generation();
    }

    pub fn remove_property_without_transition(&self, vm: &Vm, property_key: &PropertyKey, offset: u32) {
        self.invalidate_prototype_if_needed_for_change_without_transition(vm);
        assert!(self.is_dictionary());
        let removed = self.with_property_table(vm, |property_table| {
            let removed = property_table.shift_remove(property_key).is_some();
            for metadata in property_table.values_mut() {
                assert!(metadata.offset != offset);
                if metadata.offset > offset {
                    metadata.offset -= 1;
                }
            }
            removed
        });
        if removed {
            self.property_count.set(self.property_count() - 1);
        }
        self.increment_dictionary_generation();
    }

    /// Calls `callback` with the string keys of the properties of this shape in insertion order, as string values, and
    /// whether each of the properties is enumerable. They are made the first time they are asked for, and kept on the
    /// shape until its properties change. The callback must not allocate cells.
    pub fn with_own_string_keys<R>(&self, vm: &Vm, callback: impl FnOnce(&[Value], &[bool]) -> R) -> R {
        let generation = self.dictionary_generation();
        let is_cached = self.rare_data().is_some_and(|rare_data| {
            rare_data
                .own_string_keys
                .as_ref()
                .is_some_and(|own_string_keys| own_string_keys.generation == generation)
        });
        if !is_cached {
            let mut property_keys = Vec::with_capacity(self.property_count() as usize);
            self.for_each_property_in_insertion_order(|property_key, metadata| {
                if property_key.is_string() {
                    property_keys.push((
                        NonSymbolPropertyKey::from(property_key.as_string().clone()),
                        metadata.attributes.is_enumerable(),
                    ));
                }
                ControlFlow::Continue(())
            });
            let keys = MarkedVec::with_capacity(vm, property_keys.len());
            for (property_key, _) in &property_keys {
                keys.push(property_key.to_value(vm));
            }
            self.with_rare_data(vm, |rare_data| {
                rare_data.own_string_keys = Some(OwnStringKeys {
                    generation,
                    keys: keys.values().iter().copied().collect(),
                    enumerable: property_keys.iter().map(|(_, enumerable)| *enumerable).collect(),
                });
            });
        }
        let rare_data = self.rare_data().expect("the shape has its own string keys");
        let own_string_keys = rare_data
            .own_string_keys
            .as_ref()
            .expect("the shape has its own string keys");
        callback(&own_string_keys.keys, &own_string_keys.enumerable)
    }

    /// What Object.assign of an object with this shape onto an empty object made, if it was kept.
    pub fn cached_assign_copy(&self) -> Option<AssignCopy> {
        self.rare_data().and_then(|rare_data| rare_data.assign_copy)
    }

    pub fn set_cached_assign_copy(&self, vm: &Vm, assign_copy: AssignCopy) {
        assert!(!self.is_dictionary());
        self.with_rare_data(vm, |rare_data| rare_data.assign_copy = Some(assign_copy));
    }

    /// The shape that SetIntegrityLevel with `level` gave objects of this shape that have no accessor properties, if
    /// it was kept.
    pub fn cached_integrity_level_shape(&self, level: IntegrityLevel) -> Option<Gc<Shape>> {
        self.rare_data()
            .and_then(|rare_data| rare_data.integrity_level_shapes[level as usize])
    }

    pub fn set_cached_integrity_level_shape(&self, vm: &Vm, level: IntegrityLevel, shape: Gc<Shape>) {
        assert!(!self.is_dictionary() && !self.is_prototype_shape());
        self.with_rare_data(vm, |rare_data| {
            rare_data.integrity_level_shapes[level as usize] = Some(shape);
        });
    }

    fn increment_dictionary_generation(&self) {
        let generation = self.dictionary_generation.get();
        assert!(generation < u32::MAX);
        self.dictionary_generation.set(generation + 1);
    }

    pub fn clone_for_prototype(&self, vm: &Vm) -> Gc<Shape> {
        self.mark_unstable(vm);
        assert!(!self.is_prototype_shape());
        assert!(self.prototype_chain_validity.get().is_none());
        let new_shape = Self::create(vm, self.realm());
        new_shape
            .flags
            .set(new_shape.flags.get() | (self.flags.get() & shape_flag::INHERITED));
        new_shape.prototype.set(self.prototype());
        if self.is_dictionary() && self.property_count() > MAX_DESCRIPTOR_COUNT {
            new_shape.become_dictionary_shape(vm);
            self.copy_properties_to_dictionary_shape(vm, &new_shape);
        } else if self.is_dictionary() {
            new_shape.set_descriptors(Some(self.copy_descriptors(vm)));
            new_shape.property_count.set(self.property_count());
        } else {
            new_shape.set_descriptors(self.descriptors());
            new_shape.property_count.set(self.property_count());
        }
        new_shape.prototype_chain_validity.set(Some(WatchpointSet::create(vm)));
        if let Some(prototype) = new_shape.prototype() {
            prototype.shape().add_child_prototype_shape(vm, new_shape);
        }
        new_shape
    }

    pub fn set_prototype_without_transition(&self, vm: &Vm, new_prototype: Gc<Object>) {
        new_prototype.convert_to_prototype_if_needed(vm);
        self.prototype.set(Some(new_prototype));
    }

    pub fn set_prototype_shape(&self, vm: &Vm) {
        assert!(!self.is_prototype_shape());
        self.prototype_chain_validity.set(Some(WatchpointSet::create(vm)));
        if let Some(prototype) = self.prototype() {
            prototype.shape().add_child_prototype_shape(vm, self.as_gc());
        }
    }

    fn add_child_prototype_shape(&self, vm: &Vm, child: Gc<Shape>) {
        assert!(self.is_prototype_shape());
        assert!(child.is_prototype_shape());
        let child = GcWeak::new(vm.heap(), child);
        self.with_rare_data(vm, |rare_data| rare_data.child_prototype_shapes.push(child));
    }

    fn invalidate_prototype_if_needed_for_new_prototype(&self, vm: &Vm, new_prototype_shape: Gc<Shape>) {
        if !self.is_prototype_shape() {
            return;
        }
        new_prototype_shape.set_prototype_shape(vm);
        self.prototype_shape_validity().fire(vm);
        self.fire_slot_watchpoints(vm);

        self.invalidate_all_prototype_chains_leading_to_this(vm);

        // The owning object is keeping the same [[Prototype]], so its existing
        // children descend from new_prototype_shape going forward.
        let child_prototype_shapes = self
            .with_existing_rare_data(vm, |rare_data| core::mem::take(&mut rare_data.child_prototype_shapes))
            .filter(|child_prototype_shapes| !child_prototype_shapes.is_empty());
        if let Some(child_prototype_shapes) = child_prototype_shapes {
            new_prototype_shape.with_rare_data(vm, |rare_data| {
                rare_data.child_prototype_shapes = child_prototype_shapes;
            });
        }
    }

    fn invalidate_prototype_if_needed_for_change_without_transition(&self, vm: &Vm) {
        if !self.is_prototype_shape() {
            return;
        }
        self.prototype_shape_validity().fire(vm);
        self.prototype_chain_validity.set(Some(WatchpointSet::create(vm)));
        self.fire_slot_watchpoints(vm);

        self.invalidate_all_prototype_chains_leading_to_this(vm);
    }

    fn prototype_shape_validity(&self) -> Gc<WatchpointSet> {
        self.prototype_chain_validity
            .get()
            .expect("a prototype shape has a validity")
    }

    fn invalidate_all_prototype_chains_leading_to_this(&self, vm: &Vm) {
        let has_child_prototype_shapes = self
            .rare_data()
            .is_some_and(|rare_data| !rare_data.child_prototype_shapes.is_empty());
        if !has_child_prototype_shapes {
            return;
        }

        let mut shapes_already_enqueued = HashSet::new();
        let shapes_to_invalidate = MarkedVec::new(vm);
        let worklist = MarkedVec::new(vm);
        let enqueue_children_of = |shape: &Shape, shapes_already_enqueued: &mut HashSet<usize>| {
            shape.with_existing_rare_data(vm, |rare_data| {
                // Prune dead weak refs and enqueue the live ones in one pass.
                rare_data.child_prototype_shapes.retain(|weak| {
                    let Some(child) = weak.get() else {
                        return false;
                    };
                    if shapes_already_enqueued.insert(child.as_ptr().addr()) {
                        shapes_to_invalidate.push(child);
                        worklist.push(child);
                    }
                    true
                });
            });
        };
        enqueue_children_of(self, &mut shapes_already_enqueued);
        while let Some(shape) = worklist.pop() {
            enqueue_children_of(&shape, &mut shapes_already_enqueued);
        }

        for index in 0..shapes_to_invalidate.len() {
            let shape: Gc<Shape> = shapes_to_invalidate.get(index).expect("the index is in bounds");
            shape.prototype_shape_validity().fire(vm);
            shape.prototype_chain_validity.set(Some(WatchpointSet::create(vm)));
        }
    }

    pub fn is_dictionary(&self) -> bool {
        self.flags.get() & shape_flag::DICTIONARY != 0
    }

    /// Whether no object has left the shape, so that objects that have it keep it (V8's stable maps). Objects leave a
    /// shape over a transition away from it, so creating one makes the shape unstable, as does an object adopting
    /// another shape. Dictionary shapes change in place and are never stable. JIT code may depend on objects keeping a
    /// stable shape (see `Dependency::StableShape`).
    pub fn is_stable(&self) -> bool {
        self.flags.get() & (shape_flag::UNSTABLE | shape_flag::DICTIONARY) == 0
    }

    /// Notes that an object may leave the shape, which invalidates the JIT code that depends on it staying stable.
    pub fn mark_unstable(&self, vm: &Vm) {
        let flags = self.flags.get();
        if flags & shape_flag::UNSTABLE != 0 {
            return;
        }
        self.flags.set(flags | shape_flag::UNSTABLE);
        if flags & shape_flag::HAS_DEPENDENT_CODE != 0 {
            crate::jit::dependencies::invalidate_dependents(
                vm,
                Dependency::StableShape(CellId(core::ptr::from_ref(self) as u64)),
            );
        }
    }

    /// Notes that JIT code depends on the shape staying stable.
    pub fn set_has_dependent_code(&self) {
        self.flags.set(self.flags.get() | shape_flag::HAS_DEPENDENT_CODE);
    }

    /// Whether the objects of this shape are not extensible. Preventing extensions moves an object to such a shape, so
    /// that caches of additions to objects of the shape it had no longer apply to it.
    pub fn is_non_extensible(&self) -> bool {
        self.flags.get() & shape_flag::NON_EXTENSIBLE != 0
    }

    pub fn has_parameter_map(&self) -> bool {
        self.flags.get() & shape_flag::HAS_PARAMETER_MAP != 0
    }

    pub fn set_has_parameter_map(&self) {
        self.flags.set(self.flags.get() | shape_flag::HAS_PARAMETER_MAP);
    }

    pub fn dictionary_generation(&self) -> u32 {
        self.dictionary_generation.get()
    }

    pub fn is_prototype_shape(&self) -> bool {
        self.prototype_chain_validity.get().is_some()
    }

    pub fn prototype_chain_validity(&self) -> Option<Gc<WatchpointSet>> {
        self.prototype_chain_validity.get()
    }

    pub fn special_property_cache(&self) -> Option<Gc<SpecialPropertyCache>> {
        self.extra()?.special_property_cache.get()
    }

    pub fn set_special_property_cache(&self, cache: Gc<SpecialPropertyCache>) {
        assert!(!self.is_dictionary() && !self.is_prototype_shape());
        self.ensure_extra().special_property_cache.set(Some(cache));
    }

    /// The watchpoint set that fires when the value in slot `offset` of the object of this prototype shape changes, or
    /// when the object leaves the shape. There is none for a slot that property lookup caches may write to, since they
    /// write without telling anything.
    pub fn watch_slot_for_replacements(&self, vm: &Vm, offset: u32) -> Option<Gc<WatchpointSet>> {
        if !self.is_prototype_shape() {
            return None;
        }
        match self
            .rare_data()
            .as_ref()
            .and_then(|rare_data| rare_data.watched_slots.get(&offset))
        {
            Some(WatchedSlot::WrittenByCaches) => return None,
            Some(WatchedSlot::Watched(watchpoint_set)) if watchpoint_set.is_valid() => return Some(*watchpoint_set),
            _ => {}
        }
        let watchpoint_set = WatchpointSet::create(vm);
        self.with_rare_data(vm, |rare_data| {
            rare_data
                .watched_slots
                .insert(offset, WatchedSlot::Watched(watchpoint_set))
        });
        self.flags.set(self.flags.get() | shape_flag::HAS_WATCHED_SLOTS);
        Some(watchpoint_set)
    }

    /// Whether something relies on the values in the slots of the object of this prototype shape.
    #[inline]
    pub fn has_watched_slots(&self) -> bool {
        self.flags.get() & shape_flag::HAS_WATCHED_SLOTS != 0
    }

    /// Whether something relies on the value in slot `offset` of the object of this prototype shape.
    pub fn is_slot_watched(&self, offset: u32) -> bool {
        self.has_watched_slots()
            && self.rare_data().is_some_and(|rare_data| {
                matches!(
                    rare_data.watched_slots.get(&offset),
                    Some(WatchedSlot::Watched(watchpoint_set)) if watchpoint_set.is_valid()
                )
            })
    }

    /// Whether the value in slot `offset` of the object of this prototype shape changed while something relied on it,
    /// which compiled code takes as a sign that it may change again, rather than rely on it once more.
    pub fn was_slot_replaced(&self, offset: u32) -> bool {
        self.rare_data()
            .is_some_and(|rare_data| rare_data.replaced_slots.contains(&offset))
    }

    /// Notes that the value in slot `offset` of the object of this prototype shape changes, which fires the watchpoint
    /// set of the slot.
    pub fn note_slot_replacement(&self, vm: &Vm, offset: u32) {
        if !self.has_watched_slots() {
            return;
        }
        let watchpoint_set = self.with_existing_rare_data(vm, |rare_data| match rare_data.watched_slots.get(&offset) {
            Some(WatchedSlot::Watched(watchpoint_set)) => {
                let watchpoint_set = *watchpoint_set;
                rare_data.watched_slots.remove(&offset);
                if !rare_data.replaced_slots.contains(&offset) {
                    rare_data.replaced_slots.push(offset);
                }
                Some(watchpoint_set)
            }
            Some(WatchedSlot::WrittenByCaches) | None => None,
        });
        if let Some(watchpoint_set) = watchpoint_set.flatten() {
            watchpoint_set.fire(vm);
        }
    }

    /// Notes that property lookup caches may write to slot `offset` of the object of this prototype shape from now on,
    /// which fires the watchpoint set of the slot, and keeps the slot from getting another one.
    pub fn note_slot_written_by_caches(&self, vm: &Vm, offset: u32) {
        if !self.is_prototype_shape() {
            return;
        }
        let watched_slot = self.with_rare_data(vm, |rare_data| {
            rare_data.watched_slots.insert(offset, WatchedSlot::WrittenByCaches)
        });
        if let Some(WatchedSlot::Watched(watchpoint_set)) = watched_slot {
            watchpoint_set.fire(vm);
        }
    }

    /// Fires the watchpoint sets of the slots of the object of this prototype shape, as the object left the shape or
    /// changed it in place, after which the offsets of its properties may be others. Which slots caches write to stays
    /// known, since the caches that write to the shape in place may still do so.
    fn fire_slot_watchpoints(&self, vm: &Vm) {
        if !self.has_watched_slots() {
            return;
        }
        self.flags.set(self.flags.get() & !shape_flag::HAS_WATCHED_SLOTS);
        let watchpoint_sets = MarkedVec::new(vm);
        self.with_existing_rare_data(vm, |rare_data| {
            rare_data.watched_slots.retain(|_, watched_slot| match watched_slot {
                WatchedSlot::Watched(watchpoint_set) => {
                    watchpoint_sets.push(*watchpoint_set);
                    false
                }
                WatchedSlot::WrittenByCaches => true,
            });
        });
        for watchpoint_set in watchpoint_sets.values().iter() {
            watchpoint_set.fire(vm);
        }
    }

    pub fn property_iterator_cache(&self) -> Option<Gc<ObjectPropertyIteratorCacheData>> {
        self.extra()?.property_iterator_cache.get()
    }

    pub fn set_property_iterator_cache(&self, cache: Gc<ObjectPropertyIteratorCacheData>) {
        self.ensure_extra().property_iterator_cache.set(Some(cache));
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm.get()
    }

    pub fn prototype(&self) -> Option<Gc<Object>> {
        self.prototype.get()
    }

    pub fn property_count(&self) -> u32 {
        self.property_count.get()
    }

    pub fn lookup(&self, property_key: &PropertyKey) -> Option<PropertyMetadata> {
        if self.property_count() == 0 {
            return None;
        }
        if self.is_dictionary() {
            return self
                .dictionary_property_table()
                .table
                .borrow()
                .get(property_key)
                .copied();
        }
        self.descriptors()?.lookup(property_key, self.property_count())
    }

    /// Calls `callback` with each property in insertion order, until it breaks. The shape stays borrowed meanwhile, so
    /// the callback must not allocate or call anything that takes the VM; callers that need to copy the keys out first.
    pub fn for_each_property_in_insertion_order(
        &self,
        mut callback: impl FnMut(&PropertyKey, PropertyMetadata) -> ControlFlow<()>,
    ) {
        if self.is_dictionary() {
            let property_table = self.dictionary_property_table();
            for (property_key, metadata) in property_table.table.borrow().iter() {
                if callback(property_key, *metadata).is_break() {
                    return;
                }
            }
        } else if let Some(descriptors) = self.descriptors() {
            descriptors.for_each_in_insertion_order(callback, self.property_count());
        }
    }
}
