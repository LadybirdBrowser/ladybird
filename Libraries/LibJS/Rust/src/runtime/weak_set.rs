/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;
use std::collections::HashSet;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::heap::cell_is_dead;
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak_container::WeakContainer;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ObjectMethods};
use crate::runtime::realm::Realm;

/// The values of a WeakSet. This stores cell pointers instead of object pointers to aide with sweeping. The values are
/// weak: the VM's sweep callback removes the ones that died.
#[derive(Default)]
struct WeakSetValues(HashSet<Gc<CellHeader>, foldhash::fast::RandomState>);

// SAFETY: Visits nothing, since every value is held weakly and removed in remove_dead_cells() once it dies.
unsafe impl Trace for WeakSetValues {
    fn trace(&self, _: &mut Visitor) {}
}

#[repr(C)]
#[derive(Trace)]
pub struct WeakSet {
    base: Object,
    values: GcRefCell<WeakSetValues>,
}

static WEAK_SET_METHODS: ObjectMethods = ObjectMethods {
    initialize: WeakSet::initialize,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(WeakSet, Object, extends: [Object], methods: WEAK_SET_METHODS, finalize: finalize);

impl Deref for WeakSet {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for WeakSet {
    fn finalize(&self) {
        drop(self.values.replace(WeakSetValues::default()));
    }
}

impl WeakSet {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> WeakSet {
        WeakSet {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            values: GcRefCell::new(WeakSetValues::default()),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakSet> {
        realm.create_object(vm, WeakSet::new(vm, realm.intrinsics().weak_set_prototype(vm)))
    }

    /// Registers the new WeakSet as a weak container.
    fn initialize(object: &Object, vm: &Vm, _: Gc<Realm>) {
        vm.register_weak_container(WeakContainer::new(object.as_gc(), WeakSet::remove_dead_cells));
    }

    pub fn weak_set_has(&self, value: Gc<CellHeader>) -> bool {
        self.values.borrow().0.contains(&value)
    }

    pub fn weak_set_add(&self, value: Gc<CellHeader>) {
        self.values.borrow_mut().0.insert(value);
    }

    pub fn weak_set_remove(&self, value: Gc<CellHeader>) -> bool {
        self.values.borrow_mut().0.remove(&value)
    }

    pub fn weak_set_size(&self) -> usize {
        self.values.borrow().0.len()
    }

    fn remove_dead_cells(object: &Object, _: &Vm) {
        let weak_set = object
            .as_gc()
            .downcast::<WeakSet>()
            .expect("the weak container is a WeakSet");
        weak_set.values.borrow_mut().0.retain(|&cell| !cell_is_dead(cell));
    }
}
