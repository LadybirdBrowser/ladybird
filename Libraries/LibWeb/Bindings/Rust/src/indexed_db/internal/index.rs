/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Indexes, https://w3c.github.io/IndexedDB/#index-construct, as a connection knows them: The backend holds their
//! records.

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::{EmbedderCell, EmbedderClass, EmbedderValue, Gc, Trace, Vm};

use crate::indexed_db::internal::algorithms::KeyPath;
use crate::indexed_db::internal::object_store::ObjectStoreRef;

// https://w3c.github.io/IndexedDB/#index-construct
#[derive(Trace)]
pub(crate) struct Index {
    // An index [...] has a referenced object store.
    object_store: ObjectStoreRef,

    // An index has a name, which is a name. At any one time, the name is unique within index’s referenced object store.
    #[gc(untraced)]
    name: RefCell<Utf16String>,

    // An index has a unique flag. When true, the index enforces that no two records in the index has the same key.
    unique: bool,

    // An index has a multiEntry flag. This flag affects how the index behaves when the result of evaluating the index’s key path yields an array key.
    multi_entry: bool,

    // The keys are derived from the referenced object store’s values using a key path.
    #[gc(untraced)]
    key_path: KeyPath,

    deleted: Cell<bool>,
}

static INDEX_CLASS: EmbedderClass<Index> = EmbedderClass::new("IndexedDBIndex");

impl EmbedderValue for Index {
    const CLASS: &'static EmbedderClass<Self> = &INDEX_CLASS;
}

pub(crate) type IndexRef = Gc<EmbedderCell<Index>>;

impl Index {
    pub(crate) fn create(
        vm: &Vm,
        store: ObjectStoreRef,
        name: &Utf16String,
        key_path: &KeyPath,
        unique: bool,
        multi_entry: bool,
    ) -> IndexRef {
        let index = INDEX_CLASS.allocate(vm, |_| Index {
            object_store: store,
            name: RefCell::new(name.clone()),
            unique,
            multi_entry,
            key_path: key_path.clone(),
            deleted: Cell::new(false),
        });
        store.index_set_set(vm, name.clone(), index);
        index
    }

    pub(crate) fn set_name(this: IndexRef, vm: &Vm, name: Utf16String) {
        // NOTE: Update the key in the map so it still matches the name
        let old_value = this
            .object_store
            .index_set_take(vm, &this.name())
            .expect("the index is in its object store's index set");
        this.object_store.index_set_set(vm, name.clone(), old_value);

        *this.name.borrow_mut() = name;
    }

    pub(crate) fn name(&self) -> Utf16String {
        self.name.borrow().clone()
    }

    pub(crate) fn unique(&self) -> bool {
        self.unique
    }

    pub(crate) fn multi_entry(&self) -> bool {
        self.multi_entry
    }

    pub(crate) fn object_store(&self) -> ObjectStoreRef {
        self.object_store
    }

    pub(crate) fn key_path(&self) -> &KeyPath {
        &self.key_path
    }

    pub(crate) fn is_deleted(&self) -> bool {
        self.deleted.get()
    }

    pub(crate) fn set_deleted(&self, deleted: bool) {
        self.deleted.set(deleted);
    }
}
