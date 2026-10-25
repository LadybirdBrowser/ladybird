/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Watchpoint sets of a realm that stay valid while some properties of its intrinsics are as the realm made them. Fast
//! paths that skip steps of the iterator protocol rely on them instead of looking up those properties every time.
//!
//! - `array_iterator_next_watchpoint_set`: %ArrayIteratorPrototype% has its own `next`, the builtin of the realm. It
//!   depends on the set that fires when the value of the property changes (see `Shape::watch_slot_for_replacements()`).
//! - `array_iterator_return_watchpoint_set`: array iterators have no `return` method. %ArrayIteratorPrototype%,
//!   %IteratorPrototype% and %Object.prototype% have no `return` property, and the first two keep their prototypes. It
//!   depends on the set that fires when an object of that prototype chain changes (see
//!   `Shape::prototype_chain_validity()`).
//! - `regexp_primordial_properties`: %RegExp.prototype% and %RegExp% have the properties that the fast
//!   paths of RegExp skip reading as the realm made them (see `RegExpPrototype::primordial_properties_are_unchanged()`).
//!   It depends on the sets that fire when either object changes, and when the values of those properties change. The
//!   realm starts watching them when a fast path first asks, since it creates the RegExp intrinsics on demand.
//! - `array_iterator_method`: %Array.prototype%[@@iterator] is %Array.prototype.values% of the realm, so that arrays
//!   of the realm without an own @@iterator are iterated with the array iterators of the realm. It depends on the sets
//!   that fire when %Array.prototype% changes, and when the value of that property changes.
//! - `array_species`: %Array.prototype%.constructor is %Array%, whose @@species is the intrinsic getter, so that
//!   ArraySpeciesCreate makes a plain array for an array of the realm that has no own constructor. It depends on the
//!   sets that fire when either object changes, and when the values of those properties change.
//! - `array_prototypes_without_is_concat_spreadable`: %Array.prototype% and %Object.prototype% have no
//!   @@isConcatSpreadable, and %Array.prototype% inherits from %Object.prototype%, so that IsConcatSpreadable of an
//!   array of the realm without an own one is true. It depends on the sets that fire when either object changes.
//! - `string_objects_convert_to_their_string_data`: %String.prototype% has its builtin `valueOf` and `toString`, and
//!   neither it nor %Object.prototype%, which it inherits from, has a @@toPrimitive, so that ToPrimitive of a String
//!   object that inherits from %String.prototype% without own such properties gives its [[StringData]]. It depends on
//!   the sets that fire when either object changes, and when the values of those properties change.
//!
//! A change of an intrinsic fires the sets that depend on it, also one that leaves what they stand for in place, such
//! as a new property of %Object.prototype%. The next query of a fired set checks whether what it stands for still
//! holds, and if so, watches the intrinsics anew with a new set. Once it does not hold, the realm has no set anymore.

use core::cell::Cell;

use crate::gc::root::MarkedVec;
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::regexp_prototype::RegExpPrototype;
use crate::runtime::string_prototype::StringPrototype;
use crate::runtime::watchpoint_set::WatchpointSet;

/// Starts watching the intrinsics of a realm whose intrinsics were just created. Until then, changes of the watched
/// properties do not matter, since creating the intrinsics defines them.
pub fn arm(vm: &Vm, realm: Gc<Realm>) {
    // NB: Prototype shapes belong to one object each, and have the watchpoint sets of their slots and prototype chains.
    realm.array_iterator_prototype().convert_to_prototype_if_needed(vm);
    realm
        .intrinsics()
        .iterator_prototype(vm)
        .convert_to_prototype_if_needed(vm);
    realm.object_prototype().convert_to_prototype_if_needed(vm);

    realm
        .array_iterator_next_watchpoint_set
        .set(watch_array_iterator_next(vm, realm));
    realm
        .array_iterator_return_watchpoint_set
        .set(watch_array_iterator_return(vm, realm));
}

/// Whether array iterators of the realm have its builtin `next` method.
pub fn array_iterators_have_builtin_next(vm: &Vm, realm: Gc<Realm>) -> bool {
    let Some(set) = realm.array_iterator_next_watchpoint_set.get() else {
        return false;
    };
    if set.is_valid() {
        return true;
    }
    let set = watch_array_iterator_next(vm, realm);
    realm.array_iterator_next_watchpoint_set.set(set);
    set.is_some()
}

/// Whether array iterators of the realm have no `return` method.
pub fn array_iterators_have_no_return(vm: &Vm, realm: Gc<Realm>) -> bool {
    let Some(set) = realm.array_iterator_return_watchpoint_set.get() else {
        return false;
    };
    if set.is_valid() {
        return true;
    }
    let set = watch_array_iterator_return(vm, realm);
    realm.array_iterator_return_watchpoint_set.set(set);
    set.is_some()
}

/// The set that stays valid while %ArrayIteratorPrototype% has its own `next`, the builtin of the realm, if it has.
fn watch_array_iterator_next(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    let array_iterator_prototype = realm.array_iterator_prototype();
    let metadata = array_iterator_prototype.shape().lookup(&vm.names.next)?;
    let next = array_iterator_prototype.get_direct(metadata.offset);
    if next.is_accessor() || !next.is_function() {
        return None;
    }
    let next = next.as_function();
    if !next.is_array_prototype_next_builtin() || next.realm() != Some(realm) {
        return None;
    }
    // NB: A slot that property lookup caches may write to has no replacement watchpoint set. Caches write only to slots
    //     that the runtime wrote to before, so the realm stops watching once the property is written.
    let replacement_watchpoint_set = array_iterator_prototype
        .shape()
        .watch_slot_for_replacements(vm, metadata.offset)?;
    let set = WatchpointSet::create(vm);
    replacement_watchpoint_set.add_dependent_set(vm, set);
    Some(set)
}

/// The set that stays valid while array iterators of the realm have no `return` method, if they have none.
fn watch_array_iterator_return(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    let array_iterator_prototype = realm.array_iterator_prototype();
    let iterator_prototype = realm.intrinsics().iterator_prototype(vm);
    let object_prototype = realm.object_prototype();
    // NB: %Object.prototype% has an immutable prototype.
    let chain: [(Gc<Object>, Option<Gc<Object>>); 3] = [
        (array_iterator_prototype, Some(iterator_prototype)),
        (iterator_prototype, Some(object_prototype)),
        (object_prototype, None),
    ];
    for (object, expected_prototype) in chain {
        if expected_prototype.is_some() && object.shape().prototype() != expected_prototype {
            return None;
        }
        if object.shape().lookup(&vm.names.return_).is_some() {
            return None;
        }
    }
    // NB: The validity of the prototype chain of a prototype shape fires when the object or one of its prototypes
    //     changes its properties or its prototype.
    let prototype_chain_validity = array_iterator_prototype.shape().prototype_chain_validity()?;
    if !prototype_chain_validity.is_valid() {
        return None;
    }
    let set = WatchpointSet::create(vm);
    prototype_chain_validity.add_dependent_set(vm, set);
    Some(set)
}

/// What a realm knows of a condition on its intrinsics that it watches from when a fast path first asks about it.
#[derive(Default)]
pub struct WatchedCondition {
    /// The set that stays valid while the condition holds, since the realm last found it to hold.
    set: Cell<Option<Gc<WatchpointSet>>>,
    /// Whether the condition stopped holding, after which the realm does not watch it anymore.
    stopped_holding: Cell<bool>,
}

// SAFETY: Visits the one cell a watched condition holds.
unsafe impl Trace for WatchedCondition {
    fn trace(&self, visitor: &mut Visitor) {
        self.set.trace(visitor);
    }
}

impl WatchedCondition {
    /// Whether the condition holds. `watch` gives a new set that stays valid while it holds, if it holds.
    #[inline]
    fn holds(&self, vm: &Vm, realm: Gc<Realm>, watch: fn(&Vm, Gc<Realm>) -> Option<Gc<WatchpointSet>>) -> bool {
        if self.set.get().is_some_and(|set| set.is_valid()) {
            return true;
        }
        if self.stopped_holding.get() {
            return false;
        }
        let set = watch(vm, realm);
        self.set.set(set);
        self.stopped_holding.set(set.is_none());
        set.is_some()
    }

    /// The address of the cell that holds the set that stays valid while the condition holds, if that set is valid.
    fn valid_set_address(&self) -> Option<u64> {
        self.set
            .get()
            .is_some_and(|set| set.is_valid())
            .then_some(self.set.as_ptr() as u64)
    }
}

/// A set that stays valid while `objects` keep their properties and prototypes, and the properties `slots` of them keep
/// their values.
fn watch_intrinsics(vm: &Vm, objects: &[Gc<Object>], slots: &[(Gc<Object>, PropertyKey)]) -> Option<Gc<WatchpointSet>> {
    // NB: Prototype shapes belong to one object each, and have the watchpoint sets of their slots and prototype chains.
    for object in objects {
        object.convert_to_prototype_if_needed(vm);
    }
    let set = WatchpointSet::create(vm);
    for object in objects {
        let validity = object.shape().prototype_chain_validity()?;
        if !validity.is_valid() {
            return None;
        }
        validity.add_dependent_set(vm, set);
    }
    for (object, key) in slots {
        let metadata = object.shape().lookup(key)?;
        let replacement_watchpoint_set = object.shape().watch_slot_for_replacements(vm, metadata.offset)?;
        replacement_watchpoint_set.add_dependent_set(vm, set);
    }
    Some(set)
}

/// Whether %RegExp.prototype% and %RegExp% of the realm have the properties that the fast paths of RegExp skip reading
/// as the realm made them.
pub fn regexp_primordial_properties_are_unchanged(vm: &Vm, realm: Gc<Realm>) -> bool {
    realm
        .storage
        .regexp_primordial_properties
        .holds(vm, realm, watch_regexp_primordial_properties)
}

/// The set that stays valid while %RegExp.prototype% and %RegExp% have the properties that the fast paths of RegExp skip
/// reading as the realm made them, if they have.
fn watch_regexp_primordial_properties(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    if !RegExpPrototype::primordial_properties_are_unchanged(vm, realm) {
        return None;
    }
    let regexp_prototype = realm.intrinsics().regexp_prototype(vm);
    let regexp_constructor: Gc<Object> = realm.intrinsics().regexp_constructor(vm).upcast();
    let slots = MarkedVec::new(vm);
    for key in RegExpPrototype::primordial_property_keys(vm) {
        slots.push((regexp_prototype, key.clone()));
    }
    slots.push((regexp_prototype, PropertyKey::from(vm.well_known_symbols().match_)));
    slots.push((regexp_constructor, PropertyKey::from(vm.well_known_symbols().species)));
    watch_intrinsics(vm, &[regexp_prototype, regexp_constructor], &slots.values())
}

/// Whether ArraySpeciesCreate makes a plain array for an array of the realm that has no own constructor:
/// %Array.prototype%.constructor is %Array%, whose @@species is the intrinsic getter.
pub fn array_species_is_unchanged(vm: &Vm, realm: Gc<Realm>) -> bool {
    realm.storage.array_species.holds(vm, realm, watch_array_species)
}

/// The set that stays valid while ArraySpeciesCreate makes a plain array for an array of the realm that has no own
/// constructor, if it does.
fn watch_array_species(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    let array_prototype = realm.array_prototype();
    let array_constructor = realm.intrinsics().array_constructor(vm);
    let constructor = array_prototype
        .shape()
        .lookup(&vm.names.constructor)
        .map(|metadata| array_prototype.get_direct(metadata.offset));
    if constructor.is_none_or(|constructor| {
        !constructor.is_object() || constructor.as_object() != array_constructor.upcast::<Object>()
    }) || !array_constructor.has_intrinsic_symbol_species_getter(vm)
    {
        return None;
    }
    let array_constructor: Gc<Object> = array_constructor.upcast();
    watch_intrinsics(
        vm,
        &[array_prototype, array_constructor],
        &[
            (array_prototype, vm.names.constructor.clone()),
            (array_constructor, PropertyKey::from(vm.well_known_symbols().species)),
        ],
    )
}

/// Whether %Array.prototype%[@@iterator] of the realm is its %Array.prototype.values%.
pub fn arrays_have_builtin_iterator_method(vm: &Vm, realm: Gc<Realm>) -> bool {
    realm
        .storage
        .array_iterator_method
        .holds(vm, realm, watch_array_iterator_method)
}

/// The address of the cell of the realm that holds the set that stays valid while %Array.prototype%[@@iterator] of the
/// realm is its %Array.prototype.values%, if it holds a valid one. JIT code reads the cell to tell whether that held
/// when the realm last found out (see `arrays_have_builtin_iterator_method()`).
pub fn array_iterator_method_watchpoint_set_address(realm: Gc<Realm>) -> Option<u64> {
    realm.storage.array_iterator_method.valid_set_address()
}

/// The address of the cell of the realm that holds the set that stays valid while array iterators of the realm have its
/// builtin `next` method, if it holds a valid one. JIT code reads the cell to tell whether they had it when the realm
/// last found out (see `array_iterators_have_builtin_next()`).
pub fn array_iterator_next_watchpoint_set_address(realm: Gc<Realm>) -> Option<u64> {
    let cell = &realm.array_iterator_next_watchpoint_set;
    cell.get()
        .is_some_and(|set| set.is_valid())
        .then_some(cell.as_ptr() as u64)
}

/// The set that stays valid while %Array.prototype%[@@iterator] of the realm is its %Array.prototype.values%, if it is.
fn watch_array_iterator_method(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    let array_prototype = realm.array_prototype();
    let iterator_key = PropertyKey::from(vm.well_known_symbols().iterator);
    let metadata = array_prototype.shape().lookup(&iterator_key)?;
    let iterator_method = array_prototype.get_direct(metadata.offset);
    if !iterator_method.is_object()
        || iterator_method.as_object() != realm.array_prototype_values_function().upcast::<Object>()
    {
        return None;
    }
    watch_intrinsics(vm, &[array_prototype], &[(array_prototype, iterator_key)])
}

/// Whether `array` is iterated with the array iterators of `realm` without anything observing them: it inherits
/// %Array.prototype%[@@iterator] of the realm, which is its %Array.prototype.values%, and the iterators have the
/// builtin `next` method.
pub fn array_is_iterated_unobservably(vm: &Vm, realm: Gc<Realm>, array: &Object) -> bool {
    if array.prototype() != Some(realm.array_prototype())
        || !array_iterators_have_builtin_next(vm, realm)
        || !arrays_have_builtin_iterator_method(vm, realm)
    {
        return false;
    }
    // NB: The array inherits the iterator method from %Array.prototype%, which holds it as a data property, unless it
    //     has an own @@iterator. Arrays without named properties have none.
    let shape = array.shape();
    shape.property_count() == 0
        || shape
            .lookup(&PropertyKey::from(vm.well_known_symbols().iterator))
            .is_none()
}

/// Whether %Array.prototype% and %Object.prototype% of the realm have no @@isConcatSpreadable.
pub fn array_prototypes_have_no_is_concat_spreadable(vm: &Vm, realm: Gc<Realm>) -> bool {
    realm.storage.array_prototypes_without_is_concat_spreadable.holds(
        vm,
        realm,
        watch_array_prototypes_without_is_concat_spreadable,
    )
}

/// The set that stays valid while %Array.prototype% and %Object.prototype% of the realm have no @@isConcatSpreadable,
/// if they have none.
fn watch_array_prototypes_without_is_concat_spreadable(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    let array_prototype = realm.array_prototype();
    let object_prototype = realm.object_prototype();
    let key = PropertyKey::from(vm.well_known_symbols().is_concat_spreadable);
    // NB: %Object.prototype% has an immutable prototype.
    if array_prototype.shape().prototype() != Some(object_prototype)
        || array_prototype.shape().lookup(&key).is_some()
        || object_prototype.shape().lookup(&key).is_some()
    {
        return None;
    }
    watch_intrinsics(vm, &[array_prototype, object_prototype], &[])
}

/// Whether ToPrimitive of a String object of the realm that inherits from %String.prototype% without own `valueOf`,
/// `toString` or @@toPrimitive gives its [[StringData]].
pub fn string_objects_convert_to_their_string_data(vm: &Vm, realm: Gc<Realm>) -> bool {
    realm.storage.string_objects_convert_to_their_string_data.holds(
        vm,
        realm,
        watch_string_objects_convert_to_their_string_data,
    )
}

/// The set that stays valid while ToPrimitive of a String object of the realm that inherits from %String.prototype%
/// without own `valueOf`, `toString` or @@toPrimitive gives its [[StringData]], if it does.
fn watch_string_objects_convert_to_their_string_data(vm: &Vm, realm: Gc<Realm>) -> Option<Gc<WatchpointSet>> {
    let string_prototype = realm.string_prototype(vm);
    let object_prototype = realm.object_prototype();
    let to_primitive_key = PropertyKey::from(vm.well_known_symbols().to_primitive);
    // NB: An accessor property holds an accessor, which is not a function.
    let own_property_value = |key: &PropertyKey| {
        let metadata = string_prototype.shape().lookup(key)?;
        Some(string_prototype.get_direct(metadata.offset))
    };
    // NB: %Object.prototype% has an immutable prototype.
    if string_prototype.shape().prototype() != Some(object_prototype)
        || string_prototype.shape().lookup(&to_primitive_key).is_some()
        || object_prototype.shape().lookup(&to_primitive_key).is_some()
        || !own_property_value(&vm.names.valueOf).is_some_and(|value| StringPrototype::is_value_of_function(vm, value))
        || !own_property_value(&vm.names.toString)
            .is_some_and(|value| StringPrototype::is_to_string_function(vm, value))
    {
        return None;
    }
    watch_intrinsics(
        vm,
        &[string_prototype, object_prototype],
        &[
            (string_prototype, vm.names.valueOf.clone()),
            (string_prototype, vm.names.toString.clone()),
        ],
    )
}
