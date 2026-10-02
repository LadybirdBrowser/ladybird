/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::runtime::iterator::IteratorRecord;
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::realm::Realm;

// 27.1.4.3 Properties of Async-from-Sync Iterator Instances, https://tc39.es/ecma262/#sec-properties-of-async-from-sync-iterator-instances
#[repr(C)]
#[derive(Trace)]
pub struct AsyncFromSyncIterator {
    base: Object,
    sync_iterator_record: Cell<Gc<IteratorRecord>>, // [[SyncIteratorRecord]]
}

define_cell!(AsyncFromSyncIterator, Object, extends: [Object]);

impl Deref for AsyncFromSyncIterator {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl AsyncFromSyncIterator {
    pub fn create(vm: &Vm, realm: Gc<Realm>, sync_iterator_record: Gc<IteratorRecord>) -> Gc<AsyncFromSyncIterator> {
        realm.create_object(
            vm,
            AsyncFromSyncIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().async_from_sync_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                sync_iterator_record: Cell::new(sync_iterator_record),
            },
        )
    }

    pub fn sync_iterator_record(&self) -> Gc<IteratorRecord> {
        self.sync_iterator_record.get()
    }
}
