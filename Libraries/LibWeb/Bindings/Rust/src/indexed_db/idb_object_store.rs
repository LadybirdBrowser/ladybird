/*
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBObjectStore, https://w3c.github.io/IndexedDB/#object-store-interface

use std::rc::Rc;

use core::cell::RefCell;

use ak::Utf16String;
use libjs_rust::api::{Gc, GcRefCell, Must, Object, Realm, Trace, Value, Vm};

use crate::bindings::conversions::{CppInterface, DOMStringOrDOMStringSequence, ToJs};
use crate::bindings::interface::{Ref, Wrappable, allocate, with_temporary_execution_context_and_callbacks};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::idb_cursor::{AnyIDBCursor, CursorSourceHandle, IDBCursor, IDBCursorDirection};
use crate::indexed_db::idb_index::IDBIndex;
use crate::indexed_db::idb_request::{AnyIDBRequest, IDBRequest, IDBRequestSource};
use crate::indexed_db::idb_transaction::{IDBTransaction, TransactionState};
use crate::indexed_db::internal::algorithms::{
    KeyPath, RecordKind, RecordSource, asynchronously_execute_a_request,
    check_that_a_key_could_be_injected_into_a_value, clear_an_object_store, clone_in_realm,
    convert_a_key_path_to_a_value, convert_a_key_to_a_value, convert_a_value_to_a_key, convert_a_value_to_a_key_range,
    count_the_records_in_a_range, create_a_request_to_retrieve_multiple_items,
    create_a_request_to_retrieve_multiple_items_with_options, create_a_sorted_name_list,
    delete_records_from_an_object_store, extract_a_key_from_a_value_using_a_key_path, is_valid_key_path,
    iterate_a_cursor, queue_the_creation_of_an_index, retrieve_a_key_from_an_object_store,
    retrieve_a_value_from_an_object_store, store_a_record_into_an_object_store,
};
use crate::indexed_db::internal::function::{NoCells, create_operation};
use crate::indexed_db::internal::index::{Index, IndexRef};
use crate::indexed_db::internal::key::Key;
use crate::indexed_db::internal::object_store::ObjectStoreRef;

/// The dictionary IDBIndexParameters of IndexedDB/IDBObjectStore.idl.
pub(crate) struct IDBIndexParameters {
    pub(crate) multi_entry: bool,
    pub(crate) unique: bool,
}

/// The dictionary IDBGetAllOptions of IndexedDB/IDBObjectStore.idl.
pub(crate) struct IDBGetAllOptions {
    pub(crate) count: Option<u32>,
    pub(crate) direction: IDBCursorDirection,
    pub(crate) query: Value,
}

// https://w3c.github.io/IndexedDB/#object-store-interface
// https://w3c.github.io/IndexedDB/#object-store-handle-construct
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBObjectStore {
    // An object store handle has an associated object store and an associated transaction.
    store: ObjectStoreRef,
    transaction: Ref<IDBTransaction>,

    // An object store handle has a name, which is initialized to the name of the associated object store when the object store handle is created.
    #[gc(untraced)]
    name: RefCell<Utf16String>,

    // An object store handle has an index set
    indexes: GcRefCell<Vec<(Utf16String, IndexRef)>>,

    // NB: The keyPath getter returns the same Array every time it is inspected in the same realm.
    key_path_arrays: GcRefCell<Vec<(Gc<Object>, Gc<Object>)>>,

    // NB: Each call to index() on the same IDBObjectStore instance with the same name returns the same IDBIndex
    //     instance. These are keyed by index so that they survive renames.
    index_handles: GcRefCell<Vec<(IndexRef, Ref<IDBIndex>)>>,
}

impl Wrappable for IDBObjectStore {}

impl IDBObjectStore {
    pub(crate) fn create(vm: &Vm, store: ObjectStoreRef, transaction: Ref<IDBTransaction>) -> Ref<IDBObjectStore> {
        allocate(vm, |defer| IDBObjectStore {
            store,
            transaction,
            name: RefCell::new(store.name()),
            // An object store handle has an index set, which is initialized to the set of indexes that reference the associated object store when the object store handle is created.
            indexes: GcRefCell::new(store.index_set(defer)),
            key_path_arrays: GcRefCell::new(Vec::new()),
            index_handles: GcRefCell::new(Vec::new()),
        })
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.transaction.realm()
    }

    pub(crate) fn store(&self) -> ObjectStoreRef {
        self.store
    }

    pub(crate) fn transaction_object(&self) -> Ref<IDBTransaction> {
        self.transaction
    }

    pub(crate) fn index_set_get(&self, name: &Utf16String) -> Option<IndexRef> {
        self.indexes
            .borrow()
            .iter()
            .find(|(index_name, _)| index_name == name)
            .map(|(_, index)| *index)
    }

    pub(crate) fn rename_index_set_entry(&self, vm: &Vm, old_name: &Utf16String, name: Utf16String) {
        let mut indexes = self.indexes.borrow_mut(vm);
        let position = indexes
            .iter()
            .position(|(index_name, _)| index_name == old_name)
            .expect("the index is in the index set of its handle");
        let (_, index) = indexes.remove(position);
        match indexes.iter_mut().find(|(index_name, _)| *index_name == name) {
            Some(entry) => entry.1 = index,
            None => indexes.push((name, index)),
        }
    }

    pub(crate) fn index_set_clear(&self, vm: &Vm) {
        self.indexes.borrow_mut(vm).clear();
    }

    pub(crate) fn update_name(&self) {
        *self.name.borrow_mut() = self.store.name();
    }

    pub(crate) fn update_index_set(&self, vm: &Vm) {
        self.store.copy_index_set_into(&mut self.indexes.borrow_mut(vm));
    }

    fn index_handle(this: &Ref<Self>, vm: &Vm, index: IndexRef) -> Ref<IDBIndex> {
        if let Some((_, handle)) = this.index_handles.borrow().iter().find(|(entry, _)| *entry == index) {
            return *handle;
        }
        let handle = IDBIndex::create(vm, index, *this);
        this.index_handles.borrow_mut(vm).push((index, handle));
        handle
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The name getter steps are to return this’s name.
        Ok(self.name.borrow().clone())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-name
    pub(crate) fn set_name(&self, cx: &mut CallContext, value: Utf16String) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let name be the given value.
        let name = &value;

        // 2. Let transaction be this’s transaction.
        let transaction = self.transaction;

        // 3. Let store be this’s object store.
        let store = self.store;

        // 4. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 5. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
        if !transaction.is_upgrade_transaction() {
            return Err(invalid_state_error("Attempted to set name outside of version change"));
        }

        // 6. If transaction’s state is not active, throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while updating object store name",
            ));
        }

        // 7. If store’s name is equal to name, terminate these steps.
        if store.name() == *name {
            return Ok(());
        }

        // 8. If an object store named name already exists in store’s database, throw a "ConstraintError" DOMException.
        if store.database().object_store_with_name(name).is_some() {
            return Err(constraint_error("Object store with the given name already exists"));
        }

        // AD-HOC: Log the rename for potential revert on abort.
        store
            .mutation_log()
            .expect("an upgrade transaction logs the mutations of its stores")
            .note_object_store_renamed(vm, store.name());

        // 9. Set store’s name to name.
        store.set_name(name.clone());

        // 10. Set this’s name to name.
        *self.name.borrow_mut() = name.clone();

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-keypath
    pub(crate) fn key_path(&self) -> Option<&KeyPath> {
        self.store.key_path()
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-keypath
    pub(crate) fn key_path_value(&self, cx: &mut CallContext) -> WebIdlResult<Value> {
        let vm = cx.vm();

        // The keyPath getter steps are to return this’s object store’s key path, or null if none. The key path is
        // converted as a DOMString (if a string) or a sequence<DOMString> (if a list of strings), per [WEBIDL].
        let Some(key_path) = self.key_path() else {
            return Ok(Value::NULL);
        };

        // NOTE: The returned value is not the same instance that was used when the object store was created. However, if
        //       this attribute returns an object (specifically an Array), it returns the same object instance every time
        //       it is inspected. Changing the properties of the object has no effect on the object store.
        let global_object = cx.realm().global_object();
        if let Some((_, array)) = self
            .key_path_arrays
            .borrow()
            .iter()
            .find(|(global, _)| *global == global_object)
        {
            return Ok(Value::from_object(*array));
        }

        let value = convert_a_key_path_to_a_value(cx, key_path);
        if value.is_object() {
            self.key_path_arrays
                .borrow_mut(vm)
                .push((global_object, value.as_object()));
        }
        Ok(value)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-indexnames
    pub(crate) fn index_names(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // 1. Let names be a list of the names of the indexes in this's index set.
        let names = self.indexes.borrow().iter().map(|(name, _)| name.clone()).collect();

        // 2. Return the result (a DOMStringList) of creating a sorted name list with names.
        Ok(create_a_sorted_name_list(cx, names))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-transaction
    pub(crate) fn transaction(&self, _: &mut CallContext) -> WebIdlResult<Ref<IDBTransaction>> {
        // The transaction getter steps are to return this’s transaction.
        Ok(self.transaction)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-autoincrement
    pub(crate) fn auto_increment(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The autoIncrement getter steps are to return true if this’s object store has a key generator, and false otherwise.
        Ok(self.store.uses_a_key_generator())
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-createindex
    pub(crate) fn create_index(
        this: &Ref<Self>,
        cx: &mut CallContext,
        name: Utf16String,
        key_path: DOMStringOrDOMStringSequence,
        options: IDBIndexParameters,
    ) -> WebIdlResult<Ref<IDBIndex>> {
        let vm = cx.vm();
        let key_path = match key_path {
            DOMStringOrDOMStringSequence::DOMString(value) => KeyPath::String(value),
            DOMStringOrDOMStringSequence::Sequence(values) => KeyPath::Sequence(values),
        };

        // 1. Let transaction be this's transaction.
        let transaction = this.transaction;

        // 2. Let store be this's object store.
        let store = this.store;

        // 3. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
        if !transaction.is_upgrade_transaction() {
            return Err(invalid_state_error("Transaction is not an upgrade transaction"));
        }

        // 4. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while creating index",
            ));
        }

        // 6. If an index named name already exists in store, throw a "ConstraintError" DOMException.
        if store.index_set_get(&name).is_some() {
            return Err(constraint_error("An index with the given name already exists"));
        }

        // 7. If keyPath is not a valid key path, throw a "SyntaxError" DOMException.
        if !is_valid_key_path(&key_path) {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_utf8("Key path is not valid"),
            ));
        }

        // 8. Let unique be options’s unique member.
        let unique = options.unique;

        // 9. Let multiEntry be options’s multiEntry member.
        let multi_entry = options.multi_entry;

        // 10. If keyPath is a sequence and multiEntry is true, throw an "InvalidAccessError" DOMException.
        if matches!(key_path, KeyPath::Sequence(_)) && multi_entry {
            return Err(Exception::Dom(
                "InvalidAccessError",
                Utf16String::from_utf8("Key path is a sequence and multiEntry is true"),
            ));
        }

        // 11. Let index be a new index in store.
        //     Set index’s name to name, key path to keyPath, unique flag to unique, and multiEntry flag to multiEntry.
        let index = Index::create(vm, store, &name, &key_path, unique, multi_entry);

        // 12. Add index to this's index set.
        {
            let mut indexes = this.indexes.borrow_mut(vm);
            match indexes.iter_mut().find(|(index_name, _)| *index_name == name) {
                Some(entry) => entry.1 = index,
                None => indexes.push((name, index)),
            }
        }

        // AD-HOC: Log the creation for potential revert on abort.
        store
            .mutation_log()
            .expect("an upgrade transaction logs the mutations of its stores")
            .note_index_created(vm, index);

        // AD-HOC: Fill the index with the records of store, as a request after the ones made before.
        queue_the_creation_of_an_index(vm, *this, index);

        // 13. Return a new index handle associated with index and this.
        Ok(Self::index_handle(this, vm, index))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-index
    pub(crate) fn index(this: &Ref<Self>, cx: &mut CallContext, name: Utf16String) -> WebIdlResult<Ref<IDBIndex>> {
        let vm = cx.vm();

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction;

        // 2. Let store be this’s object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is finished, then throw an "InvalidStateError" DOMException.
        if transaction.state() == TransactionState::Finished {
            return Err(invalid_state_error("Transaction is finished"));
        }

        // 5. Let index be the index named name in this’s index set if one exists, or throw a "NotFoundError" DOMException otherwise.
        let Some(index) = this.index_set_get(&name) else {
            return Err(not_found_error("Index not found in object store"));
        };

        // 6. Return an index handle associated with index and this.
        // NOTE: Each call to this method on the same IDBObjectStore instance with the same name returns the same IDBIndex
        //       instance.
        Ok(Self::index_handle(this, vm, index))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-deleteindex
    pub(crate) fn delete_index(&self, cx: &mut CallContext, name: Utf16String) -> WebIdlResult<()> {
        let vm = cx.vm();

        // 1. Let transaction be this’s transaction.
        let transaction = self.transaction;

        // 2. Let store be this’s object store.
        let store = self.store;

        // 3. If transaction is not an upgrade transaction, throw an "InvalidStateError" DOMException.
        if !transaction.is_upgrade_transaction() {
            return Err(invalid_state_error("Transaction is not an upgrade transaction"));
        }

        // 4. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 5. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while deleting index",
            ));
        }

        // 6. Let index be the index named name in store if one exists, or throw a "NotFoundError" DOMException otherwise.
        let Some(index) = store.index_set_get(&name) else {
            return Err(not_found_error("Index not found while trying to delete it"));
        };

        // 7. Remove index from this’s index set.
        {
            let mut indexes = self.indexes.borrow_mut(vm);
            if let Some(position) = indexes.iter().position(|(index_name, _)| *index_name == name) {
                indexes.remove(position);
            }
        }

        // AD-HOC: Mark the index as deleted so that stale handles throw InvalidStateError.
        index.set_deleted(true);

        // AD-HOC: Log the deletion for potential revert on abort.
        store
            .mutation_log()
            .expect("an upgrade transaction logs the mutations of its stores")
            .note_index_deleted(vm, index);

        // 8. Destroy index.
        store.index_set_remove(vm, &name);

        Ok(())
    }

    // https://w3c.github.io/IndexedDB/#add-or-put
    fn add_or_put(
        handle: Ref<IDBObjectStore>,
        cx: &mut CallContext,
        value: Value,
        key: Option<Value>,
        no_overwrite: bool,
    ) -> WebIdlResult<AnyIDBRequest> {
        let vm = cx.vm();
        let realm = handle.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be handle’s transaction.
        let transaction = handle.transaction;

        // 2. Let store be handle’s object store.
        let store = handle.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while running add/put",
            ));
        }

        // 5. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
        if transaction.is_readonly() {
            return Err(Exception::Dom(
                "ReadOnlyError",
                Utf16String::from_utf8("Transaction is read-only"),
            ));
        }

        let key_was_given = key.is_some_and(|key| !key.is_undefined());

        // 6. If store uses in-line keys and key was given, throw a "DataError" DOMException.
        if store.uses_inline_keys() && key_was_given {
            return Err(data_error("Store uses in-line keys and key was given"));
        }

        // 7. If store uses out-of-line keys and has no key generator and key was not given, throw a "DataError" DOMException.
        if store.uses_out_of_line_keys() && !store.uses_a_key_generator() && !key_was_given {
            return Err(data_error(
                "Store uses out-of-line keys and has no key generator and key was not given",
            ));
        }

        let mut key_value: Option<Rc<Key>> = None;
        // 8. If key was given, then:
        if key_was_given {
            // 1. Let r be the result of converting a value to a key with key. Rethrow any exceptions.
            let r = convert_a_value_to_a_key(&realm_cx, key.expect("key was given"), None)?;

            // 2. If r is "invalid value" or "invalid type", throw a "DataError" DOMException.
            if r.is_invalid() {
                return Err(data_error("Key is invalid"));
            }

            // 3. Let key be r.
            key_value = Some(r);
        }

        // 9. Let targetRealm be a user-agent defined Realm.
        let target_cx = &realm_cx;

        // 10. Let clone be a clone of value in targetRealm during transaction. Rethrow any exceptions.
        let clone = clone_in_realm(target_cx, value, transaction)?;

        // 11. If store uses in-line keys, then:
        if store.uses_inline_keys() {
            let store_key_path = store.key_path().expect("the store uses in-line keys");

            // 1. Let kpk be the result of extracting a key from a value using a key path with clone and store’s key path. Rethrow any exceptions.
            let maybe_kpk = extract_a_key_from_a_value_using_a_key_path(&realm_cx, clone, store_key_path, false)?;

            // NOTE: Step 2 and 3 is reversed here, since we check for failure before validity.
            // 3. If kpk is not failure, let key be kpk.
            if let Ok(kpk) = maybe_kpk {
                // 2. If kpk is invalid, throw a "DataError" DOMException.
                if kpk.is_invalid() {
                    return Err(Exception::Dom("DataError", kpk.value_as_string().clone()));
                }
                key_value = Some(kpk);
            }
            // 4. Otherwise (kpk is failure):
            else {
                // 1. If store does not have a key generator, throw a "DataError" DOMException.
                if !store.uses_a_key_generator() {
                    return Err(data_error("Store does not have a key generator"));
                }

                // 2. If check that a key could be injected into a value with clone and store’s key path return false, throw a "DataError" DOMException.
                if !check_that_a_key_could_be_injected_into_a_value(&realm_cx, clone, store_key_path) {
                    return Err(data_error("Key could not be injected into value"));
                }
            }
        }

        // 12. Let operation be an algorithm to run store a record into an object store with store, clone, key, and no-overwrite flag.
        let operation = create_operation(
            vm,
            (realm, store, clone, NoCells(key_value), no_overwrite),
            |vm, &(realm, store, clone, ref key_value, no_overwrite)| {
                with_temporary_execution_context_and_callbacks(realm, || {
                    let cx = CallContext::new(vm, realm);
                    let optional_key =
                        store_a_record_into_an_object_store(&cx, store, clone, key_value.0.clone(), no_overwrite)?;

                    let Some(optional_key) = optional_key.filter(|key| !key.is_invalid()) else {
                        return Ok(Value::UNDEFINED);
                    };

                    Ok(convert_a_key_to_a_value(&cx, &optional_key))
                })
            },
        );

        // 13. Return the result (an IDBRequest) of running asynchronously execute a request with handle and operation.
        Ok(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::ObjectStore(handle),
            operation,
            None,
        ))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-add
    pub(crate) fn add(
        this: &Ref<Self>,
        cx: &mut CallContext,
        value: Value,
        key: Option<Value>,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        // The add(value, key) method steps are to return the result of running add or put with this, value, key and the no-overwrite flag true.
        Self::add_or_put(*this, cx, value, key, true).map(request_of)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-put
    pub(crate) fn put(
        this: &Ref<Self>,
        cx: &mut CallContext,
        value: Value,
        key: Option<Value>,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        // The put(value, key) method steps are to return the result of running add or put with this, value, key and the no-overwrite flag false.
        Self::add_or_put(*this, cx, value, key, false).map(request_of)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-count
    pub(crate) fn count(this: &Ref<Self>, cx: &mut CallContext, query: Option<Value>) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this's transaction.
        let transaction = this.transaction;

        // 2. Let store be this's object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while doing count",
            ));
        }

        // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, query, false)?;

        // 6. Let operation be an algorithm to run count the records in a range with store and range.
        let operation = create_operation(vm, (store, range), |_, &(store, range)| {
            Ok(count_the_records_in_a_range(RecordSource::ObjectStore(store), &range))
        });

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-get
    pub(crate) fn get(this: &Ref<Self>, cx: &mut CallContext, query: Value) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this's transaction.
        let transaction = this.transaction;

        // 2. Let store be this's object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error("Transaction is not active while getting"));
        }

        // 5. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, Some(query), true)?;

        // 6. Let operation be an algorithm to run retrieve a value from an object store with the current Realm record, store, and range.
        let operation = create_operation(vm, (realm, store, range), |vm, &(realm, store, range)| {
            retrieve_a_value_from_an_object_store(&CallContext::new(vm, realm), store, &range)
        });

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-opencursor
    pub(crate) fn open_cursor(
        this: &Ref<Self>,
        cx: &mut CallContext,
        query: Option<Value>,
        direction: IDBCursorDirection,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this's transaction.
        let transaction = this.transaction;

        // 2. Let store be this’s object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while opening cursor",
            ));
        }

        // 5. Let range be the result of converting a value to a key range with query. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, query, false)?;

        // 6. Let cursor be a new cursor with its source handle set to this, undefined position, direction set to direction,
        //    got value flag set to false, undefined key and value, range set to range, and key only flag set to false.
        let cursor = IDBCursor::create(
            vm,
            CursorSourceHandle::ObjectStore(*this),
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
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        ));

        // 9. Set cursor’s request to request.
        cursor.set_request(Some(request));

        // 10. Return request.
        Ok(request)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-delete
    pub(crate) fn delete(this: &Ref<Self>, cx: &mut CallContext, query: Value) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction;

        // 2. Let store be this’s object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while deleting object store",
            ));
        }

        // 5. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
        if transaction.is_readonly() {
            return Err(Exception::Dom(
                "ReadOnlyError",
                Utf16String::from_utf8("Transaction is read-only while deleting object store"),
            ));
        }

        // 6. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, Some(query), true)?;

        // 7. Let operation be an algorithm to run delete records from an object store with store and range.
        let operation = create_operation(vm, (store, range), |vm, &(store, range)| {
            Ok(delete_records_from_an_object_store(vm, store, &range))
        });

        // 8. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-clear
    pub(crate) fn clear(this: &Ref<Self>, cx: &mut CallContext) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction;

        // 2. Let store be this’s object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while clearing object store",
            ));
        }

        // 5. If transaction is a read-only transaction, throw a "ReadOnlyError" DOMException.
        if transaction.is_readonly() {
            return Err(Exception::Dom(
                "ReadOnlyError",
                Utf16String::from_utf8("Transaction is read-only while clearing object store"),
            ));
        }

        // 6. Let operation be an algorithm to run clear an object store with store.
        let operation = create_operation(vm, store, |vm, &store| Ok(clear_an_object_store(vm, store)));

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-getkey
    pub(crate) fn get_key(this: &Ref<Self>, cx: &mut CallContext, query: Value) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this’s transaction.
        let transaction = this.transaction;

        // 2. Let store be this’s object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
        }

        // 4. If transaction’s state is not active, then throw a "TransactionInactiveError" DOMException.
        if !transaction.is_active() {
            return Err(transaction_inactive_error(
                "Transaction is not active while getting key",
            ));
        }

        // 5. Let range be the result of converting a value to a key range with query and true. Rethrow any exceptions.
        let range = convert_a_value_to_a_key_range(&realm_cx, Some(query), true)?;

        // 6. Let operation be an algorithm to run retrieve a key from an object store with store and range.
        let operation = create_operation(vm, (realm, store, range), |vm, &(realm, store, range)| {
            Ok(retrieve_a_key_from_an_object_store(
                &CallContext::new(vm, realm),
                store,
                &range,
            ))
        });

        // 7. Return the result (an IDBRequest) of running asynchronously execute a request with this and operation.
        Ok(request_of(asynchronously_execute_a_request(
            vm,
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        )))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-getall
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
            IDBRequestSource::ObjectStore(*this),
            RecordKind::Value,
            query_or_options.unwrap_or(Value::UNDEFINED),
            count,
        )
        .map(request_of)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-openkeycursor
    pub(crate) fn open_key_cursor(
        this: &Ref<Self>,
        cx: &mut CallContext,
        query: Option<Value>,
        direction: IDBCursorDirection,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        let vm = cx.vm();
        let realm = this.realm();
        let realm_cx = CallContext::new(vm, realm);

        // 1. Let transaction be this's transaction.
        let transaction = this.transaction;

        // 2. Let store be this’s object store.
        let store = this.store;

        // 3. If store has been deleted, throw an "InvalidStateError" DOMException.
        if store.is_deleted() {
            return Err(invalid_state_error("Object store has been deleted"));
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
            CursorSourceHandle::ObjectStore(*this),
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
            IDBRequestSource::ObjectStore(*this),
            operation,
            None,
        ));

        // 9. Set cursor’s request to request.
        cursor.set_request(Some(request));

        // 10. Return request.
        Ok(request)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-getallkeys
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
            IDBRequestSource::ObjectStore(*this),
            RecordKind::Key,
            query_or_options.unwrap_or(Value::UNDEFINED),
            count,
        )
        .map(request_of)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbobjectstore-getallrecords
    pub(crate) fn get_all_records(
        this: &Ref<Self>,
        cx: &mut CallContext,
        options: IDBGetAllOptions,
    ) -> WebIdlResult<Ref<IDBRequest>> {
        // 1. Return the result of creating a request to retrieve multiple items with the current Realm record, this,
        //    "record", and options. Rethrow any exceptions.
        create_a_request_to_retrieve_multiple_items_with_options(
            &CallContext::new(cx.vm(), this.realm()),
            IDBRequestSource::ObjectStore(*this),
            RecordKind::Record,
            &options,
        )
        .map(request_of)
    }
}

pub(crate) fn request_of(request: AnyIDBRequest) -> Ref<IDBRequest> {
    match request {
        AnyIDBRequest::IDBRequest(request) => request,
        AnyIDBRequest::IDBOpenDBRequest(_) => unreachable!("a request against a transaction is not an open request"),
    }
}

pub(crate) fn idb_cursor_result(cx: &CallContext, cursor: Option<AnyIDBCursor>) -> Value {
    match cursor {
        None => Value::NULL,
        Some(cursor) => cursor.to_js(cx).must(),
    }
}

pub(crate) fn invalid_state_error(message: &str) -> Exception {
    Exception::Dom("InvalidStateError", Utf16String::from_utf8(message))
}

pub(crate) fn transaction_inactive_error(message: &str) -> Exception {
    Exception::Dom("TransactionInactiveError", Utf16String::from_utf8(message))
}

pub(crate) fn constraint_error(message: &str) -> Exception {
    Exception::Dom("ConstraintError", Utf16String::from_utf8(message))
}

pub(crate) fn not_found_error(message: &str) -> Exception {
    Exception::Dom("NotFoundError", Utf16String::from_utf8(message))
}

pub(crate) fn data_error(message: &str) -> Exception {
    Exception::Dom("DataError", Utf16String::from_utf8(message))
}
