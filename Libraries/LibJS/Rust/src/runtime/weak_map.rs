/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

use hashbrown::HashMap;
use libgc_api::Trace;

use crate::gc::class::{ExternalMemorySize, Finalize, GcCell, define_cell};
use crate::gc::defer_gc::DeferGc;
use crate::gc::gc_ref_cell::GcRefCell;
use crate::gc::sweep::Sweep;
use crate::gc::visitor::Visitor;
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

// SAFETY: Visits every value as reachable only while its key is. The keys are held weakly, and removed in
//         remove_dead_cells() once they die, together with their values, which then also hands the slots of the
//         surviving entries to a collection that moves cells.
unsafe impl Trace for WeakMapValues {
    fn trace<'c>(&'c self, visitor: &mut Visitor<'c>) {
        for (&key, value) in &self.0 {
            // SAFETY: See above.
            unsafe { visitor.visit_ephemeron_without_pinning(key, value) };
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

define_cell!(
    WeakMap,
    Object,
    extends: [Object],
    methods: WEAK_MAP_METHODS,
    finalize: finalize,
    external_memory_size: external_memory_size
);

impl ExternalMemorySize for WeakMap {
    fn external_memory_size(&self) -> usize {
        self.base
            .external_memory_size()
            .saturating_add(self.values.borrow().0.allocation_size())
    }
}

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
    pub fn new(_: &DeferGc<'_>, vm: &Vm, realm: Gc<Realm>, prototype: Gc<Object>) -> WeakMap {
        WeakMap {
            base: Object::new_with_prototype(
                vm,
                Self::CLASS,
                realm,
                Some(prototype),
                MayInterfereWithIndexedPropertyAccess::No,
            ),
            values: GcRefCell::new(WeakMapValues::default()),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<WeakMap> {
        realm.create_object(vm, |defer_gc| {
            WeakMap::new(defer_gc, vm, realm, realm.intrinsics().weak_map_prototype(vm))
        })
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

    pub fn weak_map_set(&self, vm: &Vm, key: Gc<CellHeader>, value: Value) {
        let mut values = self.values.borrow_mut(vm);
        let old_size = values.0.allocation_size();
        values.0.insert(key, value);
        let new_size = values.0.allocation_size();
        drop(values);
        vm.heap().account_external_memory_change(old_size, new_size);
    }

    pub fn weak_map_remove(&self, vm: &Vm, key: Gc<CellHeader>) -> bool {
        self.values.borrow_mut(vm).0.remove(&key).is_some()
    }

    pub fn weak_map_size(&self) -> usize {
        self.values.borrow().0.len()
    }

    fn remove_dead_cells(object: &Object, sweep: &Sweep<'_>) {
        let weak_map = object
            .as_gc()
            .downcast::<WeakMap>()
            .expect("the weak container is a WeakMap");
        let mut values = weak_map.values.borrow_mut_in_sweep(sweep);
        values.0.retain(|&key, _| !sweep.is_dead(key));
        if sweep.moves_cells() {
            // NB: The keys are hashed by their identity hash, which moves do not change, so the table stays as it is.
            for (key, value) in &values.0 {
                // SAFETY: Only the map's own methods change the table, and none runs until the collection is over.
                unsafe {
                    sweep.record_slot(key);
                    sweep.record_value_slot(value);
                }
            }
        }
    }
}
