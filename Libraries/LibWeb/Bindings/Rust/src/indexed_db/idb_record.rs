/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBRecord, https://w3c.github.io/IndexedDB/#record-interface

use std::rc::Rc;

use libjs_rust::api::{Trace, Value, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::indexed_db::internal::algorithms::convert_a_key_to_a_value;
use crate::indexed_db::internal::key::Key;

/// The bytes of a Web::HTML::StorageSerializationRecord, which records share.
pub(crate) type StorageSerializationRecord = Rc<[u8]>;

// https://w3c.github.io/IndexedDB/#object-store-record
#[derive(Clone)]
pub(crate) struct ObjectStoreRecord {
    pub(crate) key: Rc<Key>,
    pub(crate) value: StorageSerializationRecord,
}

// https://w3c.github.io/IndexedDB/#index-list-of-records
#[derive(Clone)]
pub(crate) struct IndexRecord {
    pub(crate) key: Rc<Key>,
    pub(crate) value: Rc<Key>,
}

// https://w3c.github.io/IndexedDB/#record-snapshot
// https://w3c.github.io/IndexedDB/#record-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBRecord {
    #[gc(untraced)]
    key: Rc<Key>,
    value: Value,
    #[gc(untraced)]
    primary_key: Rc<Key>,
}

impl Wrappable for IDBRecord {}

impl IDBRecord {
    pub(crate) fn create(vm: &Vm, key: Rc<Key>, value: Value, primary_key: Rc<Key>) -> Ref<IDBRecord> {
        allocate(vm, |_| IDBRecord {
            key,
            value,
            primary_key,
        })
    }

    pub(crate) fn key(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The key getter steps are to return the result of converting a key to a
        // value with this's key.
        Ok(convert_a_key_to_a_value(cx, &self.key))
    }

    pub(crate) fn primary_key(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The primaryKey getter steps are to return the result of converting a key
        // to a value with this's primary key.
        Ok(convert_a_key_to_a_value(cx, &self.primary_key))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbrecord-value
    pub(crate) fn value(&self, _: &mut CallContext) -> WebIdlResult<Value> {
        // The value getter steps are to return this’s value.
        Ok(self.value)
    }
}
