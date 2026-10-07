/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::ControlFlow;
use std::collections::{HashMap, HashSet};

use indexmap::IndexMap;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak::GcWeak;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::property_lookup_cache::ObjectPropertyIteratorCacheData;
pub use crate::layout::shape::{PrototypeChainValidity, Shape};
use crate::runtime::descriptor_array::{DescriptorArray, MAX_DESCRIPTOR_COUNT, PropertyMetadata};
use crate::runtime::property_attributes::PropertyAttributes;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

type RandomState = foldhash::fast::RandomState;

#[derive(Clone, PartialEq, Eq, Hash)]
pub struct TransitionKey {
    pub property_key: PropertyKey,
    pub attributes: PropertyAttributes,
}

define_cell!(PrototypeChainValidity, Other);

// SAFETY: A validity holds no cells.
unsafe impl Trace for PrototypeChainValidity {
    fn trace(&self, _: &mut Visitor) {}
}

impl PrototypeChainValidity {
    pub fn create(vm: &Vm) -> Gc<PrototypeChainValidity> {
        vm.heap().allocate(PrototypeChainValidity {
            header: CellHeader::for_class(Self::CLASS),
            valid: Cell::new(true),
            padding: 0,
        })
    }

    pub fn is_valid(&self) -> bool {
        self.valid.get()
    }

    pub fn set_valid(&self, valid: bool) {
        self.valid.set(valid);
    }
}

/// The bits of Shape::flags, the flags of a shape that are not its forward transition storage.
mod shape_flag {
    pub const DICTIONARY: u8 = 1 << 0;
    pub const HAS_PARAMETER_MAP: u8 = 1 << 1;
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PropertyCountChange {
    Preserve,
    Increment,
    Decrement,
}

type PropertyTable = IndexMap<PropertyKey, PropertyMetadata, RandomState>;
type ForwardTransitionMap = HashMap<TransitionKey, GcWeak<Shape>, RandomState>;

enum PropertyStorage {
    Descriptors(Option<Gc<DescriptorArray>>),
    PropertyTable(Box<PropertyTable>),
}

enum ForwardTransitions {
    Empty,
    Single {
        property_key: PropertyKey,
        attributes: u8,
        target: GcWeak<Shape>,
    },
    Multiple(Box<ForwardTransitionMap>),
}

#[derive(Default)]
struct RareData {
    /// Keyed by the address of the new prototype, which is only compared. A transition's shape keeps its prototype
    /// alive, so the shape of an entry whose prototype died is dead as well, and the entry is pruned when found.
    prototype_transitions: HashMap<usize, GcWeak<Shape>, RandomState>,
    delete_transitions: HashMap<PropertyKey, GcWeak<Shape>, RandomState>,
    child_prototype_shapes: Vec<GcWeak<Shape>>,
}

/// The parts of a shape the interpreter does not read.
pub struct ShapeStorage {
    property_storage: GcRefCell<PropertyStorage>,
    forward_transitions: GcRefCell<ForwardTransitions>,
    rare_data: GcRefCell<Option<Box<RareData>>>,
}

impl Default for ShapeStorage {
    fn default() -> Self {
        Self {
            property_storage: GcRefCell::new(PropertyStorage::Descriptors(None)),
            forward_transitions: GcRefCell::new(ForwardTransitions::Empty),
            rare_data: GcRefCell::new(None),
        }
    }
}

// SAFETY: Visits the descriptor array and the keys this storage owns. The shapes of transitions are weak, and so are
// the keys of prototype transitions, which are only compared.
unsafe impl Trace for ShapeStorage {
    fn trace(&self, visitor: &mut Visitor) {
        match &*self.property_storage.borrow() {
            PropertyStorage::Descriptors(descriptors) => descriptors.trace(visitor),
            // Descriptor arrays mark their own keys; dictionary tables are not cells, so Shape marks their keys directly.
            PropertyStorage::PropertyTable(property_table) => {
                for property_key in property_table.keys() {
                    property_key.trace(visitor);
                }
            }
        }

        // FIXME: The forward transition keys should be weak, but we have to mark them for now in case they go stale.
        match &*self.forward_transitions.borrow() {
            ForwardTransitions::Empty => {}
            ForwardTransitions::Single { property_key, .. } => property_key.trace(visitor),
            ForwardTransitions::Multiple(map) => {
                for transition_key in map.keys() {
                    transition_key.property_key.trace(visitor);
                }
            }
        }

        // FIXME: The delete transition keys should be weak, but we have to mark them for now in case they go stale.
        if let Some(rare_data) = &*self.rare_data.borrow() {
            for property_key in rare_data.delete_transitions.keys() {
                property_key.trace(visitor);
            }
        }
    }
}

define_cell!(Shape, Other);

// SAFETY: Visits the realm, the prototype, the validity, the iterator cache and what the storage owns.
unsafe impl Trace for Shape {
    fn trace(&self, visitor: &mut Visitor) {
        self.realm.trace(visitor);
        self.prototype.trace(visitor);
        self.prototype_chain_validity.trace(visitor);
        self.property_iterator_cache.trace(visitor);
        self.storage.trace(visitor);
    }
}

fn prototype_address(prototype: Option<Gc<Object>>) -> usize {
    prototype.map_or(0, |prototype| prototype.as_ptr().addr())
}

impl Shape {
    fn new(realm: Gc<Realm>) -> Self {
        Self {
            header: CellHeader::for_class(Self::CLASS),
            flags: Cell::new(0),
            realm: Cell::new(realm),
            prototype: Cell::new(None),
            prototype_chain_validity: Cell::new(None),
            property_iterator_cache: Cell::new(None),
            property_count: Cell::new(0),
            dictionary_generation: Cell::new(0),
            storage: ShapeStorage::default(),
        }
    }

    fn new_from_previous_shape(previous_shape: &Shape, property_count_change: PropertyCountChange) -> Self {
        let shape = Self::new(previous_shape.realm());
        shape
            .flags
            .set(previous_shape.flags.get() & shape_flag::HAS_PARAMETER_MAP);
        shape.prototype.set(previous_shape.prototype());
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

    fn new_from_previous_shape_with_prototype(previous_shape: &Shape, new_prototype: Option<Gc<Object>>) -> Self {
        let shape = Self::new(previous_shape.realm());
        shape
            .flags
            .set(previous_shape.flags.get() & shape_flag::HAS_PARAMETER_MAP);
        shape.prototype.set(new_prototype);
        shape.property_count.set(previous_shape.property_count());
        shape
    }

    /// A new unique shape with no properties and no prototype.
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<Shape> {
        vm.heap().allocate(Self::new(realm))
    }

    fn as_gc(&self) -> Gc<Shape> {
        // SAFETY: Shapes only exist as cells, since every way to create one allocates it.
        unsafe { Gc::from_ref(self) }
    }

    fn descriptors(&self) -> Option<Gc<DescriptorArray>> {
        assert!(!self.is_dictionary());
        match &*self.storage.property_storage.borrow() {
            PropertyStorage::Descriptors(descriptors) => *descriptors,
            PropertyStorage::PropertyTable(_) => unreachable!("a shape that is not a dictionary has descriptors"),
        }
    }

    fn set_descriptors(&self, descriptors: Option<Gc<DescriptorArray>>) {
        assert!(!self.is_dictionary());
        *self.storage.property_storage.borrow_mut() = PropertyStorage::Descriptors(descriptors);
    }

    fn with_property_table<R>(&self, callback: impl FnOnce(&mut PropertyTable) -> R) -> R {
        assert!(self.is_dictionary());
        match &mut *self.storage.property_storage.borrow_mut() {
            PropertyStorage::PropertyTable(property_table) => callback(property_table),
            PropertyStorage::Descriptors(_) => unreachable!("a dictionary shape has a property table"),
        }
    }

    fn become_dictionary_shape(&self) {
        assert!(!self.is_dictionary());
        *self.storage.property_storage.borrow_mut() = PropertyStorage::PropertyTable(Box::default());
        self.flags.set(self.flags.get() | shape_flag::DICTIONARY);
    }

    fn with_rare_data<R>(&self, callback: impl FnOnce(&mut RareData) -> R) -> R {
        let mut rare_data = self.storage.rare_data.borrow_mut();
        callback(rare_data.get_or_insert_with(Box::default))
    }

    pub fn create_dictionary_transition(&self, vm: &Vm) -> Gc<Shape> {
        let new_shape = Self::create(vm, self.realm());
        new_shape.become_dictionary_shape();
        new_shape
            .flags
            .set(new_shape.flags.get() | (self.flags.get() & shape_flag::HAS_PARAMETER_MAP));
        new_shape.prototype.set(self.prototype());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        self.copy_properties_to_dictionary_shape(&new_shape);
        new_shape
    }

    fn get_or_prune_cached_forward_transition(&self, key: &TransitionKey) -> Option<Gc<Shape>> {
        if self.is_prototype_shape() {
            return None;
        }
        let mut forward_transitions = self.storage.forward_transitions.borrow_mut();
        match &mut *forward_transitions {
            ForwardTransitions::Empty => None,
            ForwardTransitions::Single {
                property_key,
                attributes,
                target,
            } => {
                let Some(shape) = target.get() else {
                    // The cached forward transition has gone stale (from garbage collection). Prune it.
                    *forward_transitions = ForwardTransitions::Empty;
                    return None;
                };
                if !(*property_key == key.property_key && *attributes == key.attributes.bits()) {
                    return None;
                }
                Some(shape)
            }
            ForwardTransitions::Multiple(map) => {
                let target = map.get(key)?;
                let Some(shape) = target.get() else {
                    // The cached forward transition has gone stale (from garbage collection). Prune it.
                    map.remove(key);
                    return None;
                };
                Some(shape)
            }
        }
    }

    fn cache_forward_transition(&self, vm: &Vm, key: TransitionKey, shape: Gc<Shape>) {
        assert!(!self.is_prototype_shape());
        let target = GcWeak::new(vm.heap(), shape);

        let mut forward_transitions = self.storage.forward_transitions.borrow_mut();
        match core::mem::replace(&mut *forward_transitions, ForwardTransitions::Empty) {
            ForwardTransitions::Empty => {
                *forward_transitions = ForwardTransitions::Single {
                    property_key: key.property_key,
                    attributes: key.attributes.bits(),
                    target,
                };
            }
            ForwardTransitions::Single {
                property_key,
                attributes,
                target: existing_target,
            } => {
                if existing_target.get().is_none() {
                    *forward_transitions = ForwardTransitions::Single {
                        property_key: key.property_key,
                        attributes: key.attributes.bits(),
                        target,
                    };
                    return;
                }

                if property_key == key.property_key && attributes == key.attributes.bits() {
                    *forward_transitions = ForwardTransitions::Single {
                        property_key,
                        attributes,
                        target,
                    };
                    return;
                }

                let existing_key = TransitionKey {
                    property_key,
                    attributes: PropertyAttributes::new(attributes),
                };
                let mut map = Box::new(ForwardTransitionMap::default());
                map.insert(existing_key, existing_target);
                map.insert(key, target);
                *forward_transitions = ForwardTransitions::Multiple(map);
            }
            ForwardTransitions::Multiple(mut map) => {
                map.insert(key, target);
                *forward_transitions = ForwardTransitions::Multiple(map);
            }
        }
    }

    fn get_or_prune_cached_delete_transition(&self, key: &PropertyKey) -> Option<Gc<Shape>> {
        if self.is_prototype_shape() {
            return None;
        }
        let mut rare_data = self.storage.rare_data.borrow_mut();
        let delete_transitions = &mut rare_data.as_mut()?.delete_transitions;
        let target = delete_transitions.get(key)?;
        let Some(shape) = target.get() else {
            // The cached delete transition has gone stale (from garbage collection). Prune it.
            delete_transitions.remove(key);
            return None;
        };
        Some(shape)
    }

    fn get_or_prune_cached_prototype_transition(&self, prototype: Option<Gc<Object>>) -> Option<Gc<Shape>> {
        if self.is_prototype_shape() {
            return None;
        }
        let mut rare_data = self.storage.rare_data.borrow_mut();
        let prototype_transitions = &mut rare_data.as_mut()?.prototype_transitions;
        let key = prototype_address(prototype);
        let target = prototype_transitions.get(&key)?;
        let Some(shape) = target.get() else {
            // The cached prototype transition has gone stale (from garbage collection). Prune it.
            prototype_transitions.remove(&key);
            return None;
        };
        Some(shape)
    }

    pub fn create_put_transition(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
    ) -> Gc<Shape> {
        let key = TransitionKey {
            property_key: property_key.clone(),
            attributes,
        };
        if let Some(existing_shape) = self.get_or_prune_cached_forward_transition(&key) {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(Self::new_from_previous_shape(self, PropertyCountChange::Increment));

        let property_count = self.property_count();
        let metadata = PropertyMetadata {
            offset: property_count,
            attributes,
        };
        match self.descriptors() {
            Some(descriptors) if descriptors.size() == property_count => {
                descriptors.set(property_key, metadata, property_count);
                new_shape.set_descriptors(Some(descriptors));
            }
            _ => {
                let descriptors = self.copy_descriptors(vm);
                new_shape.set_descriptors(Some(descriptors));
                descriptors.set(property_key, metadata, property_count);
            }
        }
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        if !self.is_prototype_shape() {
            self.cache_forward_transition(vm, key, new_shape);
        }
        new_shape
    }

    pub fn create_configure_transition(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
    ) -> Gc<Shape> {
        let key = TransitionKey {
            property_key: property_key.clone(),
            attributes,
        };
        if let Some(existing_shape) = self.get_or_prune_cached_forward_transition(&key) {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(Self::new_from_previous_shape(self, PropertyCountChange::Preserve));
        let descriptors = self.copy_descriptors(vm);
        new_shape.set_descriptors(Some(descriptors));
        descriptors.set_attributes(property_key, attributes, self.property_count());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        if !self.is_prototype_shape() {
            self.cache_forward_transition(vm, key, new_shape);
        }
        new_shape
    }

    pub fn create_prototype_transition(&self, vm: &Vm, new_prototype: Option<Gc<Object>>) -> Gc<Shape> {
        if let Some(existing_shape) = self.get_or_prune_cached_prototype_transition(new_prototype) {
            return existing_shape;
        }
        if let Some(new_prototype) = new_prototype {
            new_prototype.convert_to_prototype_if_needed(vm);
        }
        let new_shape = vm
            .heap()
            .allocate(Self::new_from_previous_shape_with_prototype(self, new_prototype));
        if self.is_dictionary() && self.property_count() > MAX_DESCRIPTOR_COUNT {
            new_shape.become_dictionary_shape();
            self.copy_properties_to_dictionary_shape(&new_shape);
        } else if self.is_dictionary() {
            new_shape.set_descriptors(Some(self.copy_descriptors(vm)));
        } else {
            new_shape.set_descriptors(self.descriptors());
        }
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        if !self.is_prototype_shape() {
            let target = GcWeak::new(vm.heap(), new_shape);
            self.with_rare_data(|rare_data| {
                rare_data
                    .prototype_transitions
                    .insert(prototype_address(new_prototype), target)
            });
        }
        new_shape
    }

    pub fn create_delete_transition(&self, vm: &Vm, property_key: &PropertyKey) -> Gc<Shape> {
        if let Some(existing_shape) = self.get_or_prune_cached_delete_transition(property_key) {
            return existing_shape;
        }
        let new_shape = vm
            .heap()
            .allocate(Self::new_from_previous_shape(self, PropertyCountChange::Decrement));
        let descriptors = self.copy_descriptors(vm);
        new_shape.set_descriptors(Some(descriptors));
        descriptors.remove(property_key, self.property_count());
        self.invalidate_prototype_if_needed_for_new_prototype(vm, new_shape);
        let target = GcWeak::new(vm.heap(), new_shape);
        self.with_rare_data(|rare_data| rare_data.delete_transitions.insert(property_key.clone(), target));
        new_shape
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
            descriptors.set(property_key, metadata, descriptors.size());
            ControlFlow::Continue(())
        });
        descriptors
    }

    fn copy_properties_to_dictionary_shape(&self, shape: &Shape) {
        assert!(shape.is_dictionary());
        let property_count = shape.with_property_table(|property_table| {
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
            let inserted_new_entry = self
                .with_property_table(|property_table| property_table.insert(property_key.clone(), metadata).is_none());
            if inserted_new_entry {
                assert!(property_count < u32::MAX);
                self.property_count.set(property_count + 1);
                self.increment_dictionary_generation();
            }
            return;
        }

        self.ensure_descriptor_array(vm);
        let descriptors = self.descriptors().expect("the shape has a descriptor array");
        if descriptors.lookup(property_key, property_count).is_none() {
            assert!(property_count < u32::MAX);
            descriptors.set(property_key, metadata, property_count);
            self.property_count.set(property_count + 1);
            self.increment_dictionary_generation();
            return;
        }
        descriptors.set(property_key, metadata, property_count);
    }

    pub fn set_property_attributes_without_transition(
        &self,
        vm: &Vm,
        property_key: &PropertyKey,
        attributes: PropertyAttributes,
    ) {
        self.invalidate_prototype_if_needed_for_change_without_transition(vm);
        assert!(self.is_dictionary());
        self.with_property_table(|property_table| {
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
        let removed = self.with_property_table(|property_table| {
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

    fn increment_dictionary_generation(&self) {
        self.dictionary_generation
            .set(self.dictionary_generation.get().wrapping_add(1));
    }

    pub fn clone_for_prototype(&self, vm: &Vm) -> Gc<Shape> {
        assert!(!self.is_prototype_shape());
        assert!(self.prototype_chain_validity.get().is_none());
        let new_shape = Self::create(vm, self.realm());
        new_shape
            .flags
            .set(new_shape.flags.get() | (self.flags.get() & shape_flag::HAS_PARAMETER_MAP));
        new_shape.prototype.set(self.prototype());
        if self.is_dictionary() && self.property_count() > MAX_DESCRIPTOR_COUNT {
            new_shape.become_dictionary_shape();
            self.copy_properties_to_dictionary_shape(&new_shape);
        } else if self.is_dictionary() {
            new_shape.set_descriptors(Some(self.copy_descriptors(vm)));
            new_shape.property_count.set(self.property_count());
        } else {
            new_shape.set_descriptors(self.descriptors());
            new_shape.property_count.set(self.property_count());
        }
        new_shape
            .prototype_chain_validity
            .set(Some(PrototypeChainValidity::create(vm)));
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
        self.prototype_chain_validity
            .set(Some(PrototypeChainValidity::create(vm)));
        if let Some(prototype) = self.prototype() {
            prototype.shape().add_child_prototype_shape(vm, self.as_gc());
        }
    }

    fn add_child_prototype_shape(&self, vm: &Vm, child: Gc<Shape>) {
        assert!(self.is_prototype_shape());
        assert!(child.is_prototype_shape());
        let child = GcWeak::new(vm.heap(), child);
        self.with_rare_data(|rare_data| rare_data.child_prototype_shapes.push(child));
    }

    fn invalidate_prototype_if_needed_for_new_prototype(&self, vm: &Vm, new_prototype_shape: Gc<Shape>) {
        if !self.is_prototype_shape() {
            return;
        }
        new_prototype_shape.set_prototype_shape(vm);
        self.prototype_shape_validity().set_valid(false);

        self.invalidate_all_prototype_chains_leading_to_this(vm);

        // The owning object is keeping the same [[Prototype]], so its existing
        // children descend from new_prototype_shape going forward.
        let child_prototype_shapes = self
            .storage
            .rare_data
            .borrow_mut()
            .as_mut()
            .filter(|rare_data| !rare_data.child_prototype_shapes.is_empty())
            .map(|rare_data| core::mem::take(&mut rare_data.child_prototype_shapes));
        if let Some(child_prototype_shapes) = child_prototype_shapes {
            new_prototype_shape.with_rare_data(|rare_data| rare_data.child_prototype_shapes = child_prototype_shapes);
        }
    }

    fn invalidate_prototype_if_needed_for_change_without_transition(&self, vm: &Vm) {
        if !self.is_prototype_shape() {
            return;
        }
        self.prototype_shape_validity().set_valid(false);
        self.prototype_chain_validity
            .set(Some(PrototypeChainValidity::create(vm)));

        self.invalidate_all_prototype_chains_leading_to_this(vm);
    }

    fn prototype_shape_validity(&self) -> Gc<PrototypeChainValidity> {
        self.prototype_chain_validity
            .get()
            .expect("a prototype shape has a validity")
    }

    fn invalidate_all_prototype_chains_leading_to_this(&self, vm: &Vm) {
        let has_child_prototype_shapes = self
            .storage
            .rare_data
            .borrow()
            .as_ref()
            .is_some_and(|rare_data| !rare_data.child_prototype_shapes.is_empty());
        if !has_child_prototype_shapes {
            return;
        }

        let mut shapes_already_enqueued = HashSet::new();
        let shapes_to_invalidate = MarkedVec::new(vm);
        let worklist = MarkedVec::new(vm);
        let enqueue_children_of = |shape: &Shape, shapes_already_enqueued: &mut HashSet<usize>| {
            let mut rare_data = shape.storage.rare_data.borrow_mut();
            let Some(rare_data) = rare_data.as_mut() else {
                return;
            };
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
        };
        enqueue_children_of(self, &mut shapes_already_enqueued);
        while let Some(shape) = worklist.pop() {
            enqueue_children_of(&shape, &mut shapes_already_enqueued);
        }

        for index in 0..shapes_to_invalidate.len() {
            let shape: Gc<Shape> = shapes_to_invalidate.get(index).expect("the index is in bounds");
            shape.prototype_shape_validity().set_valid(false);
            shape
                .prototype_chain_validity
                .set(Some(PrototypeChainValidity::create(vm)));
        }
    }

    pub fn is_dictionary(&self) -> bool {
        self.flags.get() & shape_flag::DICTIONARY != 0
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

    pub fn prototype_chain_validity(&self) -> Option<Gc<PrototypeChainValidity>> {
        self.prototype_chain_validity.get()
    }

    pub fn property_iterator_cache(&self) -> Option<Gc<ObjectPropertyIteratorCacheData>> {
        self.property_iterator_cache.get()
    }

    pub fn set_property_iterator_cache(&self, cache: Gc<ObjectPropertyIteratorCacheData>) {
        self.property_iterator_cache.set(Some(cache));
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
            return self.with_property_table(|property_table| property_table.get(property_key).copied());
        }
        self.descriptors()?.lookup(property_key, self.property_count())
    }

    /// Calls `callback` with each property in insertion order, until it breaks. The shape stays borrowed meanwhile, so
    /// the callback must not allocate or call anything that takes the VM; callers that need to copy the keys out first.
    pub fn for_each_property_in_insertion_order(
        &self,
        mut callback: impl FnMut(&PropertyKey, PropertyMetadata) -> ControlFlow<()>,
    ) {
        let property_storage = self.storage.property_storage.borrow();
        match &*property_storage {
            PropertyStorage::PropertyTable(property_table) => {
                for (property_key, metadata) in property_table.iter() {
                    if callback(property_key, *metadata).is_break() {
                        return;
                    }
                }
            }
            PropertyStorage::Descriptors(None) => {}
            PropertyStorage::Descriptors(Some(descriptors)) => {
                descriptors.for_each_in_insertion_order(callback, self.property_count());
            }
        }
    }
}
