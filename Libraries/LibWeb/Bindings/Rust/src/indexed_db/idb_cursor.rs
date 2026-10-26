/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBCursor, https://w3c.github.io/IndexedDB/#cursor-interface

use std::rc::Rc;

use core::cell::{Cell, RefCell};
use core::ops::Deref;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Realm, ThrowCompletionOr, Trace, Value, Vm};

use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::idb_cursor_with_value::IDBCursorWithValue;
use crate::indexed_db::idb_index::IDBIndex;
use crate::indexed_db::idb_key_range::IDBKeyRange;
use crate::indexed_db::idb_object_store::{
    IDBObjectStore, data_error, idb_cursor_result, invalid_state_error, request_of, transaction_inactive_error,
};
use crate::indexed_db::idb_request::{AnyIDBRequest, IDBRequest, IDBRequestSource};
use crate::indexed_db::idb_transaction::IDBTransaction;
use crate::indexed_db::internal::algorithms::{
    RecordSource, asynchronously_execute_a_request, clone_in_realm, convert_a_key_to_a_value, convert_a_value_to_a_key,
    delete_records_from_an_object_store, extract_a_key_from_a_value_using_a_key_path, iterate_a_cursor,
    store_a_record_into_an_object_store,
};
use crate::indexed_db::internal::function::{NoCells, create_operation};
use crate::indexed_db::internal::key::Key;
use crate::indexed_db::internal::object_store::ObjectStoreRef;

/// The enumeration IDBCursorDirection of IndexedDB/IDBCursor.idl.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum IDBCursorDirection {
    Next,
    Nextunique,
    Prev,
    Prevunique,
}

/// The union (IDBObjectStore or IDBIndex) of IndexedDB/IDBCursor.idl.
pub(crate) enum IDBObjectStoreOrIDBIndex {
    IDBObjectStore(Ref<IDBObjectStore>),
    IDBIndex(Ref<IDBIndex>),
}

/// The source handle of a cursor: the index handle or the object store handle that opened it.
#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum CursorSourceHandle {
    ObjectStore(Ref<IDBObjectStore>),
    Index(Ref<IDBIndex>),
}

impl From<CursorSourceHandle> for IDBRequestSource {
    fn from(source_handle: CursorSourceHandle) -> Self {
        match source_handle {
            CursorSourceHandle::ObjectStore(object_store) => IDBRequestSource::ObjectStore(object_store),
            CursorSourceHandle::Index(index) => IDBRequestSource::Index(index),
        }
    }
}

// NB: The key and primaryKey getters return the same object instance every time they are inspected, until the
//     cursor's key or effective key changes. These cache the converted values along with the keys they came from.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct CachedKeyValue {
    #[gc(untraced)]
    key: Option<Rc<Key>>,
    value: Value,
}

// https://w3c.github.io/IndexedDB/#cursor-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBCursor {
    // A cursor has a key and a value which represent the key and the value of the last iterated record.
    #[gc(untraced)]
    key: RefCell<Option<Rc<Key>>>,
    value: Cell<Option<Value>>,

    // A cursor has a position within its range.
    #[gc(untraced)]
    position: RefCell<Option<Rc<Key>>>,

    // When iterating indexes the cursor also has an object store position
    #[gc(untraced)]
    object_store_position: RefCell<Option<Rc<Key>>>,

    // A cursor has a direction that determines whether it moves in monotonically increasing or decreasing order of the record keys when iterated, and if it skips duplicated values when iterating indexes.
    #[gc(untraced)]
    direction: IDBCursorDirection,

    // A cursor has a got value flag.
    got_value: Cell<bool>,

    // A cursor has a source handle, which is the index handle or the object store handle that opened the cursor.
    source_handle: CursorSourceHandle,

    // A cursor has a range of records in either an index or an object store.
    range: Ref<IDBKeyRange>,

    // A cursor has a request, which is the request used to open the cursor.
    request: Cell<Option<Ref<IDBRequest>>>,

    // A cursor also has a key only flag, that indicates whether the cursor’s value is exposed via the API.
    key_only: bool,

    cached_key_value: GcRefCell<CachedKeyValue>,
    cached_primary_key_value: GcRefCell<CachedKeyValue>,

    // NB: The cursor as the interface it was created as, for the members of IDBCursor to return and pass on.
    this: Cell<Option<AnyIDBCursor>>,
}

impl Wrappable for IDBCursor {}

impl IDBCursor {
    #[allow(clippy::too_many_arguments, reason = "the C++ created a cursor with these")]
    fn new(
        source_handle: CursorSourceHandle,
        position: Option<Rc<Key>>,
        direction: IDBCursorDirection,
        got_value: bool,
        key: Option<Rc<Key>>,
        value: Option<Value>,
        range: Ref<IDBKeyRange>,
        key_only: bool,
    ) -> Self {
        Self {
            value: Cell::new(value),
            position: RefCell::new(position),
            object_store_position: RefCell::new(None),
            direction,
            got_value: Cell::new(got_value),
            key: RefCell::new(key),
            source_handle,
            range,
            request: Cell::new(None),
            key_only,
            cached_key_value: GcRefCell::new(CachedKeyValue {
                key: None,
                value: Value::UNDEFINED,
            }),
            cached_primary_key_value: GcRefCell::new(CachedKeyValue {
                key: None,
                value: Value::UNDEFINED,
            }),
            this: Cell::new(None),
        }
    }

    #[allow(clippy::too_many_arguments, reason = "the C++ created a cursor with these")]
    pub(crate) fn create(
        vm: &Vm,
        source_handle: CursorSourceHandle,
        position: Option<Rc<Key>>,
        direction: IDBCursorDirection,
        got_value: bool,
        key: Option<Rc<Key>>,
        value: Option<Value>,
        range: Ref<IDBKeyRange>,
        key_only: bool,
    ) -> AnyIDBCursor {
        let cursor = Self::new(
            source_handle,
            position,
            direction,
            got_value,
            key,
            value,
            range,
            key_only,
        );

        // A cursor that has its key only flag set to false implements the IDBCursorWithValue interface as well.
        let cursor = if key_only {
            AnyIDBCursor::IDBCursor(allocate(vm, |_| cursor))
        } else {
            AnyIDBCursor::IDBCursorWithValue(allocate(vm, |_| IDBCursorWithValue::new(cursor)))
        };
        cursor.this.set(Some(cursor));
        cursor
    }

    fn this(&self) -> AnyIDBCursor {
        self.this.get().expect("a cursor knows itself once it is created")
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        match self.source_handle {
            CursorSourceHandle::ObjectStore(object_store) => object_store.realm(),
            CursorSourceHandle::Index(index) => index.realm(),
        }
    }

    pub(crate) fn direction_value(&self) -> IDBCursorDirection {
        self.direction
    }

    pub(crate) fn range(&self) -> Ref<IDBKeyRange> {
        self.range
    }

    pub(crate) fn position(&self) -> Option<Rc<Key>> {
        self.position.borrow().clone()
    }

    pub(crate) fn current_key(&self) -> Option<Rc<Key>> {
        self.key.borrow().clone()
    }

    pub(crate) fn object_store_position(&self) -> Option<Rc<Key>> {
        self.object_store_position.borrow().clone()
    }

    pub(crate) fn key_only(&self) -> bool {
        self.key_only
    }

    pub(crate) fn current_value(&self) -> Option<Value> {
        self.value.get()
    }

    pub(crate) fn set_request(&self, request: Option<Ref<IDBRequest>>) {
        self.request.set(request);
    }

    pub(crate) fn set_position(&self, position: Option<Rc<Key>>) {
        *self.position.borrow_mut() = position;
    }

    pub(crate) fn set_got_value(&self, got_value: bool) {
        self.got_value.set(got_value);
    }

    pub(crate) fn set_key(&self, key: Option<Rc<Key>>) {
        *self.key.borrow_mut() = key;
    }

    pub(crate) fn set_value(&self, value: Option<Value>) {
        self.value.set(value);
    }

    pub(crate) fn set_object_store_position(&self, object_store_position: Option<Rc<Key>>) {
        *self.object_store_position.borrow_mut() = object_store_position;
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-source
    pub(crate) fn source(&self, _: &mut CallContext) -> WebIdlResult<IDBObjectStoreOrIDBIndex> {
        // The source getter steps are to return this’s source handle.
        Ok(match self.source_handle {
            CursorSourceHandle::ObjectStore(object_store) => IDBObjectStoreOrIDBIndex::IDBObjectStore(object_store),
            CursorSourceHandle::Index(index) => IDBObjectStoreOrIDBIndex::IDBIndex(index),
        })
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-direction
    pub(crate) fn direction(&self, _: &mut CallContext) -> WebIdlResult<IDBCursorDirection> {
        // The direction getter steps are to return this’s direction.
        Ok(self.direction)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-request
    pub(crate) fn request(&self, _: &mut CallContext) -> WebIdlResult<Ref<IDBRequest>> {
        // The request getter steps are to return this’s request.
        Ok(self.request.get().expect("a cursor has a request once it is returned"))
    }

    pub(crate) fn key(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The key getter steps are to return the result of converting a key to a
        // value with the cursor's current key.
        // NOTE: If key returns an object (e.g. a Date or Array), it returns the same object instance every time it is
        //       inspected, until the cursor's key is changed. This means that if the object is modified, those
        //       modifications will be seen by anyone inspecting the value of the cursor. However modifying such an object
        //       does not modify the contents of the database.
        let Some(key) = self.current_key() else {
            return Ok(Value::UNDEFINED);
        };

        let is_cached = self
            .cached_key_value
            .borrow()
            .key
            .as_ref()
            .is_some_and(|cached| Rc::ptr_eq(cached, &key));
        if !is_cached {
            let value = convert_a_key_to_a_value(cx, &key);
            *self.cached_key_value.borrow_mut(cx.vm()) = CachedKeyValue { key: Some(key), value };
        }
        Ok(self.cached_key_value.borrow().value)
    }

    pub(crate) fn primary_key(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The primaryKey getter steps are to return the result of converting a key
        // to a value with the cursor's current effective key.
        // NOTE: If primaryKey returns an object (e.g. a Date or Array), it returns the same object instance every time
        //       is inspected, until the cursor's effective key is changed. This means that if the object is modified,
        //       those modifications will be seen by anyone inspecting the value of the cursor. However modifying such an
        //       object does not modify the contents of the database.
        // AD-HOC: The effective key of a cursor whose key is undefined is undefined too, as in other engines.
        let Some(effective_key) = self.current_key().and_then(|_| self.effective_key()) else {
            return Ok(Value::UNDEFINED);
        };
        let is_cached = self
            .cached_primary_key_value
            .borrow()
            .key
            .as_ref()
            .is_some_and(|cached| Rc::ptr_eq(cached, &effective_key));
        if !is_cached {
            let value = convert_a_key_to_a_value(cx, &effective_key);
            *self.cached_primary_key_value.borrow_mut(cx.vm()) = CachedKeyValue {
                key: Some(effective_key),
                value,
            };
        }
        Ok(self.cached_primary_key_value.borrow().value)
    }

    pub(crate) fn is_source_or_object_store_deleted(&self) -> bool {
        match self.source_handle {
            CursorSourceHandle::ObjectStore(object_store) => object_store.store().is_deleted(),
            CursorSourceHandle::Index(index) => index.index().is_deleted() || index.index().object_store().is_deleted(),
        }
    }

    // https://w3c.github.io/IndexedDB/#cursor-transaction
    pub(crate) fn transaction_object(&self) -> Ref<IDBTransaction> {
        // A cursor has a transaction, which is the transaction from the cursor’s source handle.
        match self.source_handle {
            CursorSourceHandle::ObjectStore(object_store) => object_store.transaction_object(),
            CursorSourceHandle::Index(index) => index.transaction_object(),
        }
    }

    // https://w3c.github.io/IndexedDB/#cursor-source
    pub(crate) fn internal_source(&self) -> RecordSource {
        // A cursor has a source, which is an index or an object store from the cursor’s source handle.
        match self.source_handle {
            CursorSourceHandle::ObjectStore(object_store) => RecordSource::ObjectStore(object_store.store()),
            CursorSourceHandle::Index(index) => RecordSource::Index(index.index()),
        }
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-continue
    pub(crate) fn r#continue(&self, cx: &mut CallContext, key: Option<Value>) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = self.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this's transaction.
        let transaction = self.transaction_object();

        // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while continuing cursor",
            ));
        }

        // 3. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.
        if self.is_source_or_object_store_deleted() {
            return Err(invalid_state_error("Source or its object store has been deleted"));
        }

        // 4. If this's got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
        if !self.got_value.get() {
            return Err(invalid_state_error("Cursor is active or EOL while continuing"));
        }

        // 5. If key is given, then:
        let mut key_value: Option<Rc<Key>> = None;
        if let Some(key) = key {
            // 1. Let r be the result of converting a value to a key with key. Rethrow any exceptions.
            let r = convert_a_value_to_a_key(&realm_cx, key, None)?;

            // 2. If r is "invalid value" or "invalid type", throw a "DataError" DOMException.
            if r.is_invalid() {
                return Err(Exception::Dom("DataError", r.value_as_string().clone()));
            }

            // 3. Let key be r.
            let position = self.position().expect("a cursor that got a value has a position");
            key_value = Some(r.clone());

            // 4. If key is less than or equal to this's position and this's direction is "next" or "nextunique", then throw a "DataError" DOMException.
            let is_less_than_or_equal_to = Key::less_than(&r, &position) || Key::equals(&r, &position);
            if is_less_than_or_equal_to
                && (self.direction == IDBCursorDirection::Next || self.direction == IDBCursorDirection::Nextunique)
            {
                return Err(data_error("Key is less than or equal to cursor's position"));
            }

            // 5. If key is greater than or equal to this's position and this's direction is "prev" or "prevunique", then throw a "DataError" DOMException.
            let is_greater_than_or_equal_to = Key::greater_than(&r, &position) || Key::equals(&r, &position);
            if is_greater_than_or_equal_to
                && (self.direction == IDBCursorDirection::Prev || self.direction == IDBCursorDirection::Prevunique)
            {
                return Err(data_error("Key is greater than or equal to cursor's position"));
            }
        }

        // 6. Set this's got value flag to false.
        self.got_value.set(false);

        // 7. Let request be this's request.
        let request = self.request.get().expect("a cursor has a request");

        // 8. Set request’s processed flag to false.
        request.set_processed(false);

        // 9. Set request’s done flag to false.
        request.set_done(false);

        // 10. Let operation be an algorithm to run iterate a cursor with the current Realm record, this, and key (if given).
        let operation = create_operation(
            vm,
            (self.this(), realm, NoCells(key_value)),
            |vm, &(this, realm, ref key_value)| {
                let cx = CallContext::new(vm, realm);
                let result = iterate_a_cursor(&cx, this, key_value.0.clone(), None, 1)?;
                Ok(idb_cursor_result(&cx, result))
            },
        );

        // 11. Run asynchronously execute a request with this’s source handle, operation, and request.
        asynchronously_execute_a_request(
            vm,
            self.source_handle.into(),
            operation,
            Some(AnyIDBRequest::IDBRequest(request)),
        );

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#cursor-effective-key
    pub(crate) fn effective_key(&self) -> Option<Rc<Key>> {
        match self.source_handle {
            // If the source of a cursor is an object store, the effective key of the cursor is the cursor’s position
            CursorSourceHandle::ObjectStore(_) => self.position(),
            // If the source of a cursor is an index, the effective key is the cursor’s object store position.
            CursorSourceHandle::Index(_) => self.object_store_position(),
        }
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-advance
    pub(crate) fn advance(&self, cx: &mut CallContext, count: u32) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = self.realm();

        // 1. If count is 0 (zero), throw a TypeError.
        if count == 0 {
            return Err(Exception::type_error("Count must not be zero (0)"));
        }

        // 2. Let transaction be this’s transaction.
        let transaction = self.transaction_object();

        // 3. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while advancing cursor",
            ));
        }

        // 4. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.
        if self.is_source_or_object_store_deleted() {
            return Err(invalid_state_error("Source or its object store has been deleted"));
        }

        // 5. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
        if !self.got_value.get() {
            return Err(invalid_state_error("Cursor is active or EOL while advancing"));
        }

        // 6. Set this’s got value flag to false.
        self.got_value.set(false);

        // 7. Let request be this’s request.
        let request = self.request.get().expect("a cursor has a request");

        // 8. Set request’s processed flag to false.
        request.set_processed(false);

        // 9. Set request’s done flag to false.
        request.set_done(false);

        // 10. Let operation be an algorithm to run iterate a cursor with the current Realm record, this, and count.
        let operation = create_operation(vm, (self.this(), realm, count), |vm, &(this, realm, count)| {
            let cx = CallContext::new(vm, realm);
            let result = iterate_a_cursor(&cx, this, None, None, u64::from(count))?;
            Ok(idb_cursor_result(&cx, result))
        });

        // 11. Run asynchronously execute a request with this’s source handle, operation, and request.
        asynchronously_execute_a_request(
            vm,
            self.source_handle.into(),
            operation,
            Some(AnyIDBRequest::IDBRequest(request)),
        );

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-continueprimarykey
    pub(crate) fn continue_primary_key(
        &self,
        cx: &mut CallContext,
        key_param: Value,
        primary_key_param: Value,
    ) -> WebIdlResult<()> {
        let vm = cx.vm();
        let realm = self.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = self.transaction_object();

        // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while continuing cursor",
            ));
        }

        // 3. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.
        if self.is_source_or_object_store_deleted() {
            return Err(invalid_state_error("Source or its object store has been deleted"));
        }

        // 4. If this’s source is not an index throw an "InvalidAccessError" DOMException.
        if !matches!(self.source_handle, CursorSourceHandle::Index(_)) {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8("Cursor source is not an index"),
            ));
        }

        // 5. If this’s direction is not "next" or "prev", throw an "InvalidAccessError" DOMException.
        if self.direction != IDBCursorDirection::Next && self.direction != IDBCursorDirection::Prev {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8("Cursor direction is not next or prev"),
            ));
        }

        // 6. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
        if !self.got_value.get() {
            return Err(invalid_state_error("Cursor is active or EOL while continuing"));
        }

        // 7. Let r be the result of converting a value to a key with key. Rethrow any exceptions.
        let mut r = convert_a_value_to_a_key(&realm_cx, key_param, None)?;

        // 8. If r is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if r.is_invalid() {
            return Err(Exception::Dom("DataError", r.value_as_string().clone()));
        }

        // 9. Let key be r.
        let key = r;

        // 10. Let r be the result of converting a value to a key with primaryKey. Rethrow any exceptions.
        r = convert_a_value_to_a_key(&realm_cx, primary_key_param, None)?;

        // 11. If r is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if r.is_invalid() {
            return Err(Exception::Dom("DataError", r.value_as_string().clone()));
        }

        // 12. Let primaryKey be r.
        let primary_key = r;

        let position = self.position().expect("a cursor that got a value has a position");
        let object_store_position = self
            .object_store_position()
            .expect("a cursor of an index that got a value has an object store position");

        // 13. If key is less than this’s position and this’s direction is "next", throw a "DataError" DOMException.
        if Key::less_than(&key, &position) && self.direction == IDBCursorDirection::Next {
            return Err(data_error("Key is less than cursor's position"));
        }

        // 14. If key is greater than this’s position and this’s direction is "prev", throw a "DataError" DOMException.
        if Key::greater_than(&key, &position) && self.direction == IDBCursorDirection::Prev {
            return Err(data_error("Key is greater than cursor's position"));
        }

        // 15. If key is equal to this’s position and primaryKey is less than or equal to this’s object store position and this’s direction is "next", throw a "DataError" DOMException.
        if Key::equals(&key, &position)
            && (Key::less_than(&primary_key, &object_store_position)
                || Key::equals(&primary_key, &object_store_position))
            && self.direction == IDBCursorDirection::Next
        {
            return Err(data_error("Key is equal to cursor's position"));
        }

        // 16. If key is equal to this’s position and primaryKey is greater than or equal to this’s object store position and this’s direction is "prev", throw a "DataError" DOMException.
        if Key::equals(&key, &position)
            && (Key::greater_than(&primary_key, &object_store_position)
                || Key::equals(&primary_key, &object_store_position))
            && self.direction == IDBCursorDirection::Prev
        {
            return Err(data_error("Key is equal to cursor's position"));
        }

        // 17. Set this’s got value flag to false.
        self.got_value.set(false);

        // 18. Let request be this’s request.
        let request = self.request.get().expect("a cursor has a request");

        // 19. Set request’s processed flag to false.
        request.set_processed(false);

        // 20. Set request’s done flag to false.
        request.set_done(false);

        // 21. Let operation be an algorithm to run iterate a cursor with the current Realm record, this, key, and primaryKey.
        let operation = create_operation(
            vm,
            (self.this(), realm, NoCells((key, primary_key))),
            |vm, &(this, realm, ref keys)| {
                let cx = CallContext::new(vm, realm);
                let result = iterate_a_cursor(&cx, this, Some(keys.0.0.clone()), Some(keys.0.1.clone()), 1)?;
                Ok(idb_cursor_result(&cx, result))
            },
        );

        // 22. Run asynchronously execute a request with this’s source handle, operation, and request.
        asynchronously_execute_a_request(
            vm,
            self.source_handle.into(),
            operation,
            Some(AnyIDBRequest::IDBRequest(request)),
        );

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#cursor-effective-object-store
    pub(crate) fn effective_object_store(&self) -> ObjectStoreRef {
        match self.source_handle {
            CursorSourceHandle::ObjectStore(store) => {
                // If the source of a cursor is an object store, the effective object store of the cursor is that object store.
                store.store()
            }
            CursorSourceHandle::Index(index) => {
                // If the source of a cursor is an index, the effective object store of the cursor is that index’s referenced object store.
                index.object_store_handle().store()
            }
        }
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-update
    pub(crate) fn update(&self, cx: &mut CallContext, value: Value) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = self.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = self.transaction_object();

        // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while updating cursor",
            ));
        }

        // 3. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
        if transaction.is_readonly() {
            return Err(Exception::Dom(
                "ReadOnlyError",
                Utf16String::from_utf8("Transaction is read-only while updating cursor"),
            ));
        }

        // 4. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.
        if self.is_source_or_object_store_deleted() {
            return Err(invalid_state_error("Source or its object store has been deleted"));
        }

        // 5. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
        if !self.got_value.get() {
            return Err(invalid_state_error("Cursor is active or EOL while updating"));
        }

        // 6. If this’s key only flag is true, throw an "InvalidStateError" DOMException.
        if self.key_only {
            return Err(invalid_state_error("Cursor is key-only while updating"));
        }

        // 7. Let targetRealm be a user-agent defined Realm.
        // NOTE: this is 'realm' above

        // 8. Let clone be a clone of value in targetRealm during transaction. Rethrow any exceptions.
        let clone = clone_in_realm(&realm_cx, value, transaction)?;

        // 9. If this’s effective object store uses in-line keys, then:
        let effective_object_store = self.effective_object_store();
        if effective_object_store.uses_inline_keys() {
            // 1. Let kpk be the result of extracting a key from a value using a key path with clone and the key path of this’s effective object store. Rethrow any exceptions.
            let kpk = extract_a_key_from_a_value_using_a_key_path(
                &realm_cx,
                clone,
                effective_object_store.key_path().expect("the store uses in-line keys"),
                false,
            )?;

            // 2. If kpk is failure, invalid, or not equal to this’s effective key, throw a "DataError" DOMException.
            let Ok(kpk_value) = kpk else {
                return Err(data_error("Key path is invalid"));
            };

            if kpk_value.is_invalid() {
                return Err(data_error("Key path is invalid"));
            }

            if !Key::equals(
                &kpk_value,
                &self
                    .effective_key()
                    .expect("a cursor with a value has an effective key"),
            ) {
                return Err(data_error("Key path is not equal to effective key"));
            }
        }

        // 10. Let operation be an algorithm to run store a record into an object store with this’s effective object store, clone, this’s effective key, and false.
        let operation = create_operation(vm, (self.this(), realm, clone), |vm, &(this, realm, clone)| {
            with_temporary_execution_context_and_callbacks(realm, || {
                let cx = CallContext::new(vm, realm);
                let optional_key = store_a_record_into_an_object_store(
                    &cx,
                    this.effective_object_store(),
                    clone,
                    Some(
                        this.effective_key()
                            .expect("a cursor with a value has an effective key"),
                    ),
                    false,
                )?;

                let Some(optional_key) = optional_key.filter(|key| !key.is_invalid()) else {
                    return Ok(Value::UNDEFINED);
                };

                Ok(convert_a_key_to_a_value(&cx, &optional_key))
            })
        });

        // 11. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Cursor(self.this()),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbcursor-delete
    pub(crate) fn delete(&self, cx: &mut CallContext) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();

        // 1. Let transaction be this’s transaction.
        let transaction = self.transaction_object();

        // 2. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while deleting cursor",
            ));
        }

        // 3. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
        if transaction.is_readonly() {
            return Err(Exception::Dom(
                "ReadOnlyError",
                Utf16String::from_utf8("Transaction is read-only while deleting cursor"),
            ));
        }

        // 4. If this’s source or effective object store has been deleted, throw an "InvalidStateError" DOMException.
        if self.is_source_or_object_store_deleted() {
            return Err(invalid_state_error("Source or its object store has been deleted"));
        }

        // 5. If this’s got value flag is false, indicating that the cursor is being iterated or has iterated past its end, throw an "InvalidStateError" DOMException.
        if !self.got_value.get() {
            return Err(invalid_state_error("Cursor is active or EOL while deleting"));
        }

        // 6. If this’s key only flag is true, throw an "InvalidStateError" DOMException.
        if self.key_only {
            return Err(invalid_state_error("Cursor is key-only while deleting"));
        }

        // 7. Let operation be an algorithm to run delete records from an object store with this’s effective object store and this’s effective key.
        let operation = create_operation(vm, self.this(), |vm, &this| {
            let effective_key = this
                .effective_key()
                .expect("a cursor with a value has an effective key");
            let range = IDBKeyRange::create(vm, Some(effective_key.clone()), Some(effective_key), false, false);
            Ok(delete_records_from_an_object_store(
                vm,
                this.effective_object_store(),
                &range,
            ))
        });

        // 8. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Cursor(self.this()),
            operation,
            None,
        )))
    }
}

#[derive(Clone, Copy, PartialEq, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AnyIDBCursor {
    IDBCursor(Ref<IDBCursor>),
    IDBCursorWithValue(Ref<IDBCursorWithValue>),
}

impl Deref for AnyIDBCursor {
    type Target = IDBCursor;

    fn deref(&self) -> &IDBCursor {
        match self {
            Self::IDBCursor(cursor) => cursor,
            Self::IDBCursorWithValue(cursor) => {
                let cursor: &IDBCursorWithValue = cursor;
                cursor.parent()
            }
        }
    }
}

impl ToJs for AnyIDBCursor {
    fn to_js(self, cx: &CallContext) -> ThrowCompletionOr<Value> {
        match self {
            Self::IDBCursor(cursor) => cursor.to_js(cx),
            Self::IDBCursorWithValue(cursor) => cursor.to_js(cx),
        }
    }
}
