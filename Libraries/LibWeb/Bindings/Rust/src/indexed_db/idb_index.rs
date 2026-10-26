/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBIndex, https://w3c.github.io/IndexedDB/#index-interface

use core::cell::RefCell;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Object, Realm, Trace, Value, Vm};

use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::indexed_db::idb_cursor::{CursorSourceHandle, IDBCursor, IDBCursorDirection};
use crate::indexed_db::idb_object_store::{
    IDBGetAllOptions, IDBObjectStore, constraint_error, idb_cursor_result, invalid_state_error, request_of,
    transaction_inactive_error,
};
use crate::indexed_db::idb_request::{IDBRequest, IDBRequestSource};
use crate::indexed_db::idb_transaction::IDBTransaction;
use crate::indexed_db::internal::algorithms::{
    KeyPath, RecordKind, RecordSource, asynchronously_execute_a_request, convert_a_key_path_to_a_value,
    convert_a_value_to_a_key_range, count_the_records_in_a_range, create_a_request_to_retrieve_multiple_items,
    create_a_request_to_retrieve_multiple_items_with_options, iterate_a_cursor,
    retrieve_a_referenced_value_from_an_index, retrieve_a_value_from_an_index,
};
use crate::indexed_db::internal::function::create_operation;
use crate::indexed_db::internal::index::{Index, IndexRef};

// https://w3c.github.io/IndexedDB/#index-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBIndex {
    // An index handle has an associated index and an associated object store handle.
    index: IndexRef,
    object_store_handle: Ref<IDBObjectStore>,

    // An index handle has a name, which is initialized to the name of the associated index when the index handle is created.
    #[gc(untraced)]
    name: RefCell<Utf16String>,

    // NB: The keyPath getter returns the same Array every time it is inspected in the same realm.
    key_path_arrays: GcRefCell<Vec<(Gc<Object>, Gc<Object>)>>,
}

impl Wrappable for IDBIndex {}

impl IDBIndex {
    pub(crate) fn create(vm: &Vm, index: IndexRef, object_store: Ref<IDBObjectStore>) -> Ref<IDBIndex> {
        let handle = allocate(vm, |_| IDBIndex {
            index,
            object_store_handle: object_store,
            name: RefCell::new(index.name()),
            key_path_arrays: GcRefCell::new(Vec::new()),
        });
        object_store.transaction_object().register_index_handle(vm, handle);
        handle
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.object_store_handle.realm()
    }

    // The transaction of an index handle is the transaction of its associated object store handle.
    pub(crate) fn transaction_object(&self) -> Ref<IDBTransaction> {
        self.object_store_handle.transaction_object()
    }

    pub(crate) fn index(&self) -> IndexRef {
        self.index
    }

    pub(crate) fn object_store_handle(&self) -> Ref<IDBObjectStore> {
        self.object_store_handle
    }

    pub(crate) fn update_name(&self) {
        *self.name.borrow_mut() = self.index.name();
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The name getter steps are to return this’s name.
        Ok(self.name.borrow().clone())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-name
    pub(crate) fn set_name(&self, cx: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let name be the given value.
        let name = &value;

        // 2. Let transaction be this’s transaction.
        let transaction = self.transaction_object();

        // 3. Let index be this’s index.
        let index = self.index;

        // 4. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
        if !transaction.is_upgrade_transaction() {
            return Err(invalid_state_error("Transaction is not an upgrade transaction"));
        }

        // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while updating index name",
            ));
        }

        // 6. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.
        if index.is_deleted() || index.object_store().is_deleted() {
            return Err(invalid_state_error("Index or its object store has been deleted"));
        }

        // 7. If index’s name is equal to name, terminate these steps.
        if index.name() == *name {
            return Ok(());
        }

        // 8. If an index named name already exists in index’s object store, throw a "ConstraintError" DOMException.
        if index.object_store().index_set_get(name).is_some() {
            return Err(constraint_error("An index with the given name already exists"));
        }

        // AD-HOC: Log the rename for potential revert on abort.
        self.object_store_handle
            .store()
            .mutation_log()
            .expect("an upgrade transaction logs the mutations of its stores")
            .note_index_renamed(vm, index, index.name());

        // 9. Set index’s name to name.
        Index::set_name(index, vm, name.clone());

        // NOTE: Update the key in the map so it still matches the name
        self.object_store_handle
            .rename_index_set_entry(vm, &self.name.borrow(), name.clone());

        // 10. Set this’s name to name.
        *self.name.borrow_mut() = name.clone();

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-objectstore
    pub(crate) fn object_store(&self, _: &mut CallContext) -> WebIdlResult<Ref<IDBObjectStore>> {
        // The objectStore getter steps are to return this’s object store handle.
        Ok(self.object_store_handle)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-keypath
    pub(crate) fn key_path(&self) -> &KeyPath {
        self.index.key_path()
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-keypath
    pub(crate) fn key_path_value(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        // The keyPath getter steps are to return this’s index’s key path. The key path is converted as a DOMString (if a
        // string) or a sequence<DOMString> (if a list of strings), per [WEBIDL].
        // NOTE: The returned value is not the same instance that was used when the index was created. However, if this
        //       attribute returns an object (specifically an Array), it returns the same object instance every time it is
        //       inspected. Changing the properties of the object has no effect on the index.
        let global_object = cx.realm().global_object();
        if let Some((_, array)) = self
            .key_path_arrays
            .borrow()
            .iter()
            .find(|(global, _)| *global == global_object)
        {
            return Ok(Value::from_object(*array));
        }

        let value = convert_a_key_path_to_a_value(cx, self.key_path());
        if value.is_object() {
            self.key_path_arrays
                .borrow_mut(cx.vm())
                .push((global_object, value.as_object()));
        }
        Ok(value)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-multientry
    pub(crate) fn multi_entry(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The multiEntry getter steps are to return this’s index’s multiEntry flag.
        Ok(self.index.multi_entry())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-unique
    pub(crate) fn unique(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The unique getter steps are to return this’s index’s unique flag.
        Ok(self.index.unique())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-opencursor
    pub(crate) fn open_cursor(
        this: &Ref<Self>,
        cx: &mut CallContext,
        query: Option<Value>,
        direction: IDBCursorDirection,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction_object();

        // 2. Let index be this’s index.
        let index = this.index;

        // 3. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.
        if index.is_deleted() || index.object_store().is_deleted() {
            return Err(invalid_state_error("Index or its object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while opening cursor",
            ));
        }

        // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, query, false)?;

        // 6. Let cursor be a new cursor with its source handle set to this, undefined position, direction set to direction, got value flag set to false, undefined key and value, range set to range, and key only flag set to false.
        let cursor = IDBCursor::create(
            vm,
            CursorSourceHandle::Index(*this),
            None,
            direction,
            false,
            None,
            None,
            range,
            false,
        );

        // 7. Let operation be an algorithm to run iterate a cursor with the current Realm record and cursor.
        let operation = create_operation(vm, (realm, cursor), |vm, &(realm, cursor)| {
            let cx = CallContext::new(vm, realm);
            let result = iterate_a_cursor(&cx, cursor, None, None, 1)?;
            Ok(idb_cursor_result(&cx, result))
        });

        // 8. Let request be the result of running asynchronously execute a request with this and operation.
        let request = request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Index(*this),
            operation,
            None,
        ));

        // 9. Set cursor’s request to request.
        cursor.set_request(Some(request));

        // 10. Return request.
        Ok(request)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-get
    pub(crate) fn get(this: &Ref<Self>, cx: &mut CallContext, query: Value) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction_object();

        // 2. Let index be this’s index.
        let index = this.index;

        // 3. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.
        if index.is_deleted() || index.object_store().is_deleted() {
            return Err(invalid_state_error("Index or its object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error("Transaction is not active while getting"));
        }

        // 5. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, Some(query), true)?;

        // 6. Let operation be an algorithm to run retrieve a referenced value from an index with the current Realm record, index, and range.
        let operation = create_operation(vm, (realm, index, range), |vm, &(realm, index, range)| {
            retrieve_a_referenced_value_from_an_index(&CallContext::new(vm, realm), index, &range)
        });

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Index(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-getkey
    pub(crate) fn get_key(this: &Ref<Self>, cx: &mut CallContext, query: Value) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction_object();

        // 2. Let index be this’s index.
        let index = this.index;

        // 3. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.
        if index.is_deleted() || index.object_store().is_deleted() {
            return Err(invalid_state_error("Index or its object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while getting key",
            ));
        }

        // 5. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, Some(query), true)?;

        // 6. Let operation be an algorithm to run retrieve a value from an index with index and range.
        let operation = create_operation(vm, (realm, index, range), |vm, &(realm, index, range)| {
            Ok(retrieve_a_value_from_an_index(
                &CallContext::new(vm, realm),
                index,
                &range,
            ))
        });

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Index(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-getall
    pub(crate) fn get_all(
        this: &Ref<Self>,
        cx: &mut CallContext,
        query_or_options: Option<Value>,
        count: Option<u32>,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        // 1. Return the result of creating a request to retrieve multiple items with the current Realm record, this,
        //    "value", queryOrOptions, and count if given. Rethrow any exceptions.
        create_a_request_to_retrieve_multiple_items(
            &CallContext::new(cx.vm(), this.realm()),
            IDBRequestSource::Index(*this),
            RecordKind::Value,
            query_or_options.unwrap_or(Value::UNDEFINED),
            count,
        )
        .map(request_of)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-getallkeys
    pub(crate) fn get_all_keys(
        this: &Ref<Self>,
        cx: &mut CallContext,
        query_or_options: Option<Value>,
        count: Option<u32>,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        // 1. Return the result of creating a request to retrieve multiple items with the current Realm record, this, "key",
        //    queryOrOptions, and count if given. Rethrow any exceptions.
        create_a_request_to_retrieve_multiple_items(
            &CallContext::new(cx.vm(), this.realm()),
            IDBRequestSource::Index(*this),
            RecordKind::Key,
            query_or_options.unwrap_or(Value::UNDEFINED),
            count,
        )
        .map(request_of)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-count
    pub(crate) fn count(this: &Ref<Self>, cx: &mut CallContext, query: Option<Value>) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction_object();

        // 2. Let index be this’s index.
        let index = this.index;

        // 3. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.
        if index.is_deleted() || index.object_store().is_deleted() {
            return Err(invalid_state_error("Index or its object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error("Transaction is not active while counting"));
        }

        // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, query, false)?;

        // 6. Let operation be an algorithm to run count the records in a range with index and range.
        let operation = create_operation(vm, (index, range), |_, &(index, range)| {
            Ok(count_the_records_in_a_range(RecordSource::Index(index), &range))
        });

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Index(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-openkeycursor
    pub(crate) fn open_key_cursor(
        this: &Ref<Self>,
        cx: &mut CallContext,
        query: Option<Value>,
        direction: IDBCursorDirection,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction_object();

        // 2. Let index be this’s index.
        let index = this.index;

        // 3. If index or index’s object store has been deleted, throw an "InvalidStateError" DOMException.
        if index.is_deleted() || index.object_store().is_deleted() {
            return Err(invalid_state_error("Index or its object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while opening key cursor",
            ));
        }

        // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, query, false)?;

        // 6. Let cursor be a new cursor with its source handle set to this, undefined position, direction set to direction, got value flag set to false, undefined key and value, range set to range, and key only flag set to true.
        let cursor = IDBCursor::create(
            vm,
            CursorSourceHandle::Index(*this),
            None,
            direction,
            false,
            None,
            None,
            range,
            true,
        );

        // 7. Let operation be an algorithm to run iterate a cursor with the current Realm record and cursor.
        let operation = create_operation(vm, (realm, cursor), |vm, &(realm, cursor)| {
            let cx = CallContext::new(vm, realm);
            let result = iterate_a_cursor(&cx, cursor, None, None, 1)?;
            Ok(idb_cursor_result(&cx, result))
        });

        // 8. Let request be the result of running asynchronously execute a request with this and operation.
        let request = request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::Index(*this),
            operation,
            None,
        ));

        // 9. Set cursor’s request to request.
        cursor.set_request(Some(request));

        // 10. Return request.
        Ok(request)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbindex-getallrecords
    pub(crate) fn get_all_records(
        this: &Ref<Self>,
        cx: &mut CallContext,
        options: IDBGetAllOptions,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        // 1. Return the result of creating a request to retrieve multiple items with the current Realm record, this,
        //    "record", and options. Rethrow any exceptions.
        create_a_request_to_retrieve_multiple_items_with_options(
            &CallContext::new(cx.vm(), this.realm()),
            IDBRequestSource::Index(*this),
            RecordKind::Record,
            &options,
        )
        .map(request_of)
    }
}
