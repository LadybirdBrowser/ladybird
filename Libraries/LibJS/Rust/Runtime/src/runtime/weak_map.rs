/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;
use std::collections::HashMap;

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::heap::cell_is_dead;
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak_container::WeakContainer;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ObjectMethods};
use crate::runtime::realm::Realm;

/// The entries of a WeakMap. This stores cell pointers instead of object pointers to aide with sweeping. The keys are
/// weak: the VM's sweep callback removes the entries whose key died.
#[derive(Default)]
struct WeakMapValues(HashMap<Gc<CellHeader>, Value, foldhash::fast::RandomState>);

// SAFETY: Visits every value. The keys are held weakly, and removed in remove_dead_cells() once they die.
unsafe impl Trace for WeakMapValues {
    fn trace(&self, visitor: &mut Visitor) {
        for value in self.0.values() {
            value.trace(visitor);
        }
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct WeakMap {
    base: Object,
    values: GcRefCell<WeakMapValues>,
}

static WEAK_MAP_METHODS: ObjectMethods = ObjectMethods {
    initialize: WeakMap::initialize,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(WeakMap, Object, extends: [Object], methods: WEAK_MAP_METHODS, finalize: finalize);

impl Deref for WeakMap {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for WeakMap {
    fn finalize(&self) {
        drop(self.values.replace(WeakMapValues::default()));
    }
}

impl WeakMap {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> WeakMap {
        WeakMap {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            values: GcRefCell::new(WeakMapValues::default()),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakMap> {
        realm.create_object(vm, WeakMap::new(vm, realm.intrinsics().weak_map_prototype(vm)))
    }

    /// Registers the new WeakMap as a weak container.
    fn initialize(object: &Object, vm: &Vm, _: Gc<Realm>) {
        vm.register_weak_container(WeakContainer::new(object.as_gc(), WeakMap::remove_dead_cells));
    }

    pub fn weak_map_get(&self, key: Gc<CellHeader>) -> Option<Value> {
        self.values.borrow().0.get(&key).copied()
    }

    pub fn weak_map_has(&self, key: Gc<CellHeader>) -> bool {
        self.values.borrow().0.contains_key(&key)
    }

    pub fn weak_map_set(&self, key: Gc<CellHeader>, value: Value) {
        self.values.borrow_mut().0.insert(key, value);
    }

    pub fn weak_map_remove(&self, key: Gc<CellHeader>) -> bool {
        self.values.borrow_mut().0.remove(&key).is_some()
    }

    pub fn weak_map_size(&self) -> usize {
        self.values.borrow().0.len()
    }

    fn remove_dead_cells(object: &Object, _: &Vm) {
        let weak_map = object
            .as_gc()
            .downcast::<WeakMap>()
            .expect("the weak container is a WeakMap");
        weak_map.values.borrow_mut().0.retain(|&key, _| !cell_is_dead(key));
    }
}
