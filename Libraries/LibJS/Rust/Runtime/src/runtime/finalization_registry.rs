/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ops::Deref;

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
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::job_callback::JobCallback;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ObjectMethods};
use crate::runtime::realm::Realm;

struct FinalizationRecord {
    /// Held weakly, and cleared once it died.
    target: Option<Gc<CellHeader>>,
    held_value: Value,
    unregister_token: Option<Gc<CellHeader>>,
}

#[derive(Default)]
struct FinalizationRecords(Vec<FinalizationRecord>);

// SAFETY: Visits the held value and the unregister token of every record. The targets are held weakly, and cleared in
// remove_dead_cells() once they die.
unsafe impl Trace for FinalizationRecords {
    fn trace(&self, visitor: &mut Visitor) {
        for record in &self.0 {
            record.held_value.trace(visitor);
            record.unregister_token.trace(visitor);
        }
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct FinalizationRegistry {
    base: Object,
    realm: Gc<Realm>,
    cleanup_callback: Gc<JobCallback>,
    records: GcRefCell<FinalizationRecords>,
}

static FINALIZATION_REGISTRY_METHODS: ObjectMethods = ObjectMethods {
    initialize: FinalizationRegistry::initialize,
    ..ORDINARY_OBJECT_METHODS
};

define_cell!(
    FinalizationRegistry,
    Object,
    extends: [Object],
    methods: FINALIZATION_REGISTRY_METHODS,
    finalize: finalize
);

impl Deref for FinalizationRegistry {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for FinalizationRegistry {
    fn finalize(&self) {
        drop(self.records.replace(FinalizationRecords::default()));
    }
}

impl FinalizationRegistry {
    pub fn new(
        vm: &Vm,
        realm: Gc<Realm>,
        cleanup_callback: Gc<JobCallback>,
        prototype: Gc<Object>,
    ) -> FinalizationRegistry {
        FinalizationRegistry {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            realm,
            cleanup_callback,
            records: GcRefCell::new(FinalizationRecords::default()),
        }
    }

    /// Registers the new FinalizationRegistry as a weak container.
    fn initialize(object: &Object, vm: &Vm, _: Gc<Realm>) {
        vm.register_weak_container(WeakContainer::new(
            object.as_gc(),
            FinalizationRegistry::remove_dead_cells,
        ));
    }

    pub fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub fn cleanup_callback(&self) -> Gc<JobCallback> {
        self.cleanup_callback
    }

    pub fn add_finalization_record(
        &self,
        target: Gc<CellHeader>,
        held_value: Value,
        unregister_token: Option<Gc<CellHeader>>,
    ) {
        assert!(!held_value.is_empty());
        self.records.borrow_mut().0.push(FinalizationRecord {
            target: Some(target),
            held_value,
            unregister_token,
        });
    }

    // Extracted from FinalizationRegistry.prototype.unregister ( unregisterToken )
    pub fn remove_by_token(&self, unregister_token: Gc<CellHeader>) -> bool {
        // 4. Let removed be false.
        let mut removed = false;

        // 5. For each Record { [[WeakRefTarget]], [[HeldValue]], [[UnregisterToken]] } cell of finalizationRegistry.[[Cells]], do
        self.records.borrow_mut().0.retain(|record| {
            //  a. If cell.[[UnregisterToken]] is not empty and SameValue(cell.[[UnregisterToken]], unregisterToken) is true, then
            if record.unregister_token == Some(unregister_token) {
                // i. Remove cell from finalizationRegistry.[[Cells]].
                // ii. Set removed to true.
                removed = true;
                return false;
            }
            true
        });

        // 6. Return removed.
        removed
    }

    pub fn has_empty_cells(&self) -> bool {
        self.records.borrow().0.iter().any(|record| record.target.is_none())
    }

    fn remove_dead_cells(object: &Object, vm: &Vm) {
        let finalization_registry = object
            .as_gc()
            .downcast::<FinalizationRegistry>()
            .expect("the weak container is a FinalizationRegistry");
        let mut any_cells_were_removed = false;
        for record in &mut finalization_registry.records.borrow_mut().0 {
            let Some(target) = record.target else {
                continue;
            };
            if !cell_is_dead(target) {
                continue;
            }
            record.target = None;
            any_cells_were_removed = true;
        }
        if any_cells_were_removed {
            // NOTE: The VM keeps the FinalizationRegistry alive even if a subsequent GC is triggered before the
            //       callback has a chance to run.
            vm.enqueue_finalization_registry_cleanup_job_after_collection(finalization_registry);
        }
    }

    // 9.13 CleanupFinalizationRegistry ( finalizationRegistry ), https://tc39.es/ecma262/#sec-cleanup-finalization-registry
    pub fn cleanup(&self, vm: &Vm, callback: Option<Gc<JobCallback>>) -> ThrowCompletionOr<()> {
        // 1. Assert: finalizationRegistry has [[Cells]] and [[CleanupCallback]] internal slots.
        // Note: Ensured by type.

        // 2. Let callback be finalizationRegistry.[[CleanupCallback]].
        let cleanup_callback = callback.unwrap_or(self.cleanup_callback);

        // 3. While finalizationRegistry.[[Cells]] contains a Record cell such that cell.[[WeakRefTarget]] is empty, an implementation may perform the following steps:
        loop {
            // a. Choose any such cell.
            // b. Remove cell from finalizationRegistry.[[Cells]].
            let held_value = {
                let mut records = self.records.borrow_mut();
                let Some(index) = records.0.iter().position(|record| record.target.is_none()) else {
                    break;
                };
                records.0.remove(index).held_value
            };

            // c. Perform ? HostCallJobCallback(callback, undefined, « cell.[[HeldValue]] »).
            vm.host_call_job_callback()(vm, cleanup_callback, Value::UNDEFINED, &[held_value])?;
        }

        // 4. Return unused.
        Ok(())
    }
}
