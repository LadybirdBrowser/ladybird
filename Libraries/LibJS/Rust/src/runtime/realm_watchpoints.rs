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
//!
//! A change of an intrinsic fires the sets that depend on it, also one that leaves what they stand for in place, such
//! as a new property of %Object.prototype%. The next query of a fired set checks whether what it stands for still
//! holds, and if so, watches the intrinsics anew with a new set. Once it does not hold, the realm has no set anymore.

use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::realm::Realm;
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
