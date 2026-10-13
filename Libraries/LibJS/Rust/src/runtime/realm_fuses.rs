/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Fuses: flags of a realm that stay set while some properties of its intrinsics are as the realm made them. Fast
//! paths that skip steps of the iterator protocol rely on them instead of looking up those properties every time.
//!
//! - `array_iterator_next_fuse`: %ArrayIteratorPrototype% has its own `next`, the builtin of the realm. Any change of
//!   that property pops it, also a write of the same value: caches write only to slots that the runtime wrote to
//!   before, so the first write pops the fuse before a cache can write to the slot.
//! - `array_iterator_return_fuse`: array iterators have no `return` method. A `return` property on
//!   %ArrayIteratorPrototype%, %IteratorPrototype% or %Object.prototype%, or a new prototype of one of the first two,
//!   pops it. %Object.prototype% has an immutable prototype.
//!
//! A fuse never becomes intact again. Popping it invalidates the JIT code that depends on it.

use core::cell::Cell;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::jit::dependencies::{CellId, Dependency};
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;

pub use crate::layout::realm::Fuse;

define_cell!(Fuse, Other);

// SAFETY: A fuse holds no cells.
unsafe impl Trace for Fuse {
    fn trace(&self, _: &mut Visitor) {}
}

impl Fuse {
    fn create(vm: &Vm) -> Gc<Fuse> {
        vm.heap().allocate(Fuse {
            header: CellHeader::for_class(Self::CLASS),
            intact: Cell::new(true),
            padding: 0,
        })
    }

    pub fn is_intact(&self) -> bool {
        self.intact.get()
    }

    fn pop(&self, vm: &Vm) {
        if !self.is_intact() {
            return;
        }
        self.intact.set(false);
        crate::jit::dependencies::invalidate_dependents(vm, Dependency::Fuse(CellId(core::ptr::from_ref(self) as u64)));
    }
}

/// Sets the fuses of a realm whose intrinsics were just created. Until then, changes of the watched properties do not
/// pop anything, since creating the intrinsics defines them.
pub fn arm(vm: &Vm, realm: Gc<Realm>) {
    // NB: Prototype shapes belong to one object each, so that caches of other objects never write to the slots of
    //     the watched objects.
    realm.array_iterator_prototype().convert_to_prototype_if_needed(vm);
    realm
        .intrinsics()
        .iterator_prototype(vm)
        .convert_to_prototype_if_needed(vm);
    realm.object_prototype().convert_to_prototype_if_needed(vm);

    realm.array_iterator_next_fuse.set(Some(Fuse::create(vm)));
    realm.array_iterator_return_fuse.set(Some(Fuse::create(vm)));
}

/// Whether array iterators of the realm have its builtin `next` method.
pub fn array_iterators_have_builtin_next(realm: &Realm) -> bool {
    realm
        .array_iterator_next_fuse
        .get()
        .is_some_and(|fuse| fuse.is_intact())
}

/// Whether array iterators of the realm have no `return` method.
pub fn array_iterators_have_no_return(realm: &Realm) -> bool {
    realm
        .array_iterator_return_fuse
        .get()
        .is_some_and(|fuse| fuse.is_intact())
}

/// Notes that property `key` of `object` changes: it is added, written, deleted or gets other attributes.
#[inline(always)]
pub fn note_property_change(vm: &Vm, object: &Object, key: &PropertyKey) {
    if *key == vm.names.next || *key == vm.names.return_ {
        note_watched_property_change(vm, object, key);
    }
}

#[cold]
fn note_watched_property_change(vm: &Vm, object: &Object, key: &PropertyKey) {
    let realm = object.shape().realm();
    if *key == vm.names.next {
        if let Some(fuse) = realm.array_iterator_next_fuse.get()
            && is(object, realm.array_iterator_prototype())
        {
            fuse.pop(vm);
        }
        return;
    }
    if let Some(fuse) = realm.array_iterator_return_fuse.get()
        && (is_prototype_of_array_iterators(vm, &realm, object) || is(object, realm.object_prototype()))
    {
        fuse.pop(vm);
    }
}

/// Notes that the prototype of `object` changes.
pub fn note_prototype_change(vm: &Vm, object: &Object) {
    let realm = object.shape().realm();
    if let Some(fuse) = realm.array_iterator_return_fuse.get()
        && is_prototype_of_array_iterators(vm, &realm, object)
    {
        fuse.pop(vm);
    }
}

/// Whether `object` is %ArrayIteratorPrototype% or %IteratorPrototype% of the realm, whose fuses are armed.
fn is_prototype_of_array_iterators(vm: &Vm, realm: &Realm, object: &Object) -> bool {
    is(object, realm.array_iterator_prototype()) || is(object, realm.intrinsics().iterator_prototype(vm))
}

/// Whether `object` is `intrinsic`. NB: The hooks also see objects that are not allocated yet.
fn is(object: &Object, intrinsic: Gc<Object>) -> bool {
    core::ptr::eq(object, intrinsic.as_ptr())
}
