/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ptr::NonNull;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::heap::cell_is_dead;
use crate::gc::visitor::{Trace, Visitor};
use crate::gc::weak_container::WeakContainer;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::realm::Realm;

#[repr(C)]
pub struct WeakRef {
    base: Object,
    /// The object or symbol the WeakRef refers to, which is empty once it died.
    value: Cell<Value>,
    last_execution_generation: Cell<u64>,
    /// The execution generation of the VM, which visiting reads.
    vm_execution_generation: NonNull<Cell<u64>>,
}

// SAFETY: Visits the target while the execution generation it was last used in is current, which keeps it alive for
// the rest of the job as AddToKeptObjects does. Otherwise it is held weakly, and remove_dead_cells() forgets it once
// it dies.
unsafe impl Trace for WeakRef {
    fn trace(&self, visitor: &mut Visitor) {
        self.base.trace(visitor);

        // SAFETY: The VM owns the heap and destroys it before anything else, so it outlives every WeakRef.
        let execution_generation = unsafe { self.vm_execution_generation.as_ref() }.get();
        if execution_generation == self.last_execution_generation.get() {
            self.value.get().trace(visitor);
        }
    }
}

define_object_class!(WeakRef, extends: [Object], methods: {
    initialize: WeakRef::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl WeakRef {
    /// A WeakRef to `value`, an object or a symbol.
    pub fn new(vm: &Vm, value: Value, prototype: Gc<Object>) -> WeakRef {
        assert!(value.is_object() || value.is_symbol());
        WeakRef {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            value: Cell::new(value),
            last_execution_generation: Cell::new(vm.head.execution_generation.get()),
            vm_execution_generation: NonNull::from(&vm.head.execution_generation),
        }
    }

    pub fn create(vm: &Vm, realm: Gc<Realm>, value: Value) -> Gc<WeakRef> {
        realm.create_object(vm, WeakRef::new(vm, value, realm.intrinsics().weak_ref_prototype(vm)))
    }

    /// Registers the new WeakRef as a weak container.
    fn initialize(object: &Object, vm: &Vm, _: Gc<Realm>) {
        vm.register_weak_container(WeakContainer::new(object.as_gc(), WeakRef::remove_dead_cells));
    }

    /// The target, or empty once it died.
    pub fn value(&self) -> Value {
        self.value.get()
    }

    pub fn update_execution_generation(&self, vm: &Vm) {
        self.last_execution_generation.set(vm.head.execution_generation.get());
    }

    fn remove_dead_cells(object: &Object, _: &Vm) {
        let weak_ref = object
            .as_gc()
            .downcast::<WeakRef>()
            .expect("the weak container is a WeakRef");
        let value = weak_ref.value.get();
        let is_alive = value.is_empty() || !cell_is_dead(value.as_cell());
        if is_alive {
            return;
        }

        weak_ref.value.set(Value::EMPTY);
    }
}
