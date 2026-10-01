/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::runtime_functions::unimplemented_runtime_function;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::iterator::{IteratorRecord, IteratorRecordImpl};
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::realm::Realm;

/// %AsyncFromSyncIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct AsyncFromSyncIteratorPrototype {
    base: Object,
}

define_object_class!(AsyncFromSyncIteratorPrototype, extends: [Object], methods: {
    initialize: AsyncFromSyncIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl AsyncFromSyncIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<AsyncFromSyncIteratorPrototype> {
        realm.create_object(
            vm,
            AsyncFromSyncIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().async_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(_object: &Object, _vm: &Vm, _realm: Gc<Realm>) {
        // NB: %AsyncFromSyncIteratorPrototype%.next, .return and .throw come with the async-from-sync iterator
        //     builtins.
    }
}

// 27.1.6.1 CreateAsyncFromSyncIterator ( syncIteratorRecord ), https://tc39.es/ecma262/#sec-createasyncfromsynciterator
pub fn create_async_from_sync_iterator(_vm: &Vm, _sync_iterator_record: Gc<IteratorRecord>) -> IteratorRecordImpl {
    // 1. Let asyncIterator be OrdinaryObjectCreate(%AsyncFromSyncIteratorPrototype%, « [[SyncIteratorRecord]] »).
    // 2. Set asyncIterator.[[SyncIteratorRecord]] to syncIteratorRecord.
    unimplemented_runtime_function(
        "CreateAsyncFromSyncIterator, whose AsyncFromSyncIterator objects come with async functions",
        0,
    )
}
