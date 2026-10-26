/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2024-2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! IDBFactory, https://w3c.github.io/IndexedDB/#factory-interface

use ak::Utf16String;
use libjs_rust::api::{Array, Gc, Must, Object, PrimitiveString, PropertyKey, Realm, Trace, Value, Vm};

use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::{WebIdlPromise, create_promise, reject_promise, resolve_promise};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::indexed_db::cpp::{obtain_a_storage_key, run_in_parallel};
use crate::indexed_db::idb_open_db_request::IDBOpenDBRequest;
use crate::indexed_db::idb_request::AnyIDBRequest;
use crate::indexed_db::internal::algorithms::{
    DeleteResult, OpenResult, convert_a_value_to_a_key, delete_a_database, fire_a_version_change_event,
    open_a_database_connection, queue_a_database_task, set_idb_request_result,
};
use crate::indexed_db::internal::database::{Database, DatabaseRef};
use crate::indexed_db::internal::function::{NoCells, create_function, create_function_with};
use crate::indexed_db::internal::key::Key;

/// The dictionary IDBDatabaseInfo of IndexedDB/IDBFactory.idl.
pub(crate) struct IDBDatabaseInfo {
    pub(crate) name: Option<Utf16String>,
    pub(crate) version: Option<u64>,
}

// https://w3c.github.io/IndexedDB/#idbfactory
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct IDBFactory {
    realm: Gc<Realm>,
}

impl Wrappable for IDBFactory {
    fn relevant_realm(&self, _: &Vm) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl IDBFactory {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<IDBFactory> {
        allocate(vm, |_| IDBFactory { realm })
    }

    // https://w3c.github.io/IndexedDB/#dom-idbfactory-open
    pub(crate) fn open(
        &self,
        cx: &mut CallContext,
        name: Utf16String,
        version: Option<u64>,
    ) -> WebIdlResult<Ref<IDBOpenDBRequest>> {
        let vm = cx.vm();
        let realm = self.realm;

        // 1. If version is 0 (zero), throw a TypeError.
        if version == Some(0) {
            return Err(Exception::type_error("The version provided must not be 0"));
        }

        // 2. Let environment be this's relevant settings object.
        // 3. Let storageKey be the result of running obtain a storage key given environment.
        //    If failure is returned, then throw a "SecurityError" DOMException and abort these steps.
        let Some(storage_key) = obtain_a_storage_key(realm) else {
            return Err(Exception::Dom(
                "SecurityError",
                Utf16String::from_utf8("Failed to obtain a storage key"),
            ));
        };

        // 4. Let request be a new open request.
        let request = IDBOpenDBRequest::create(vm, realm);

        // 5. Run these steps in parallel:
        // NB: We could defer these steps, but it wouldn't be observable anyway, since open_a_database_connection() will
        //     put this work into a queue and then process it later in the event loop regardless.

        // 1. Let result be the result of opening a database connection, with storageKey, name, version if given and undefined otherwise, and request.
        open_a_database_connection(
            vm,
            realm,
            storage_key,
            name,
            version,
            AnyIDBRequest::IDBOpenDBRequest(request),
            create_function_with(vm, (realm, request), |vm, &(realm, request), result: OpenResult| {
                // 2. Set request’s processed flag to true.
                let request = AnyIDBRequest::IDBOpenDBRequest(request);
                request.set_processed(true);

                // 3. Queue a database task to run these steps:
                queue_a_database_task(
                    vm,
                    create_function(vm, (realm, request, result), |vm, &(realm, request, ref result), ()| {
                        match result {
                            // 1. If result is an error, then:
                            Err(error) => {
                                // 1. Set request’s result to undefined.
                                request.set_result(Value::UNDEFINED);

                                // 2. Set request’s error to result.
                                request.set_error(vm, Some(error.clone()));

                                // 3. Set request’s done flag to true.
                                request.set_done(true);

                                // 4. Fire an event named error at request with its bubbles and cancelable attributes initialized to true.
                                request.event_target().dispatch_event(&CppEvent::create(
                                    request.realm(),
                                    "error",
                                    true,
                                    true,
                                    false,
                                ));
                            }
                            // 2. Otherwise:
                            Ok(connection) => {
                                // 1. Set request’s result to result.
                                set_idb_request_result(vm, realm, request, *connection);

                                // 2. Set request’s done flag to true.
                                request.set_done(true);

                                // 3. Fire an event named success at request.
                                request.event_target().dispatch_event(&CppEvent::create(
                                    request.realm(),
                                    "success",
                                    false,
                                    false,
                                    false,
                                ));
                            }
                        }
                    }),
                );
            }),
        );

        // 6. Return a new IDBOpenDBRequest object for request.
        Ok(request)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbfactory-cmp
    pub(crate) fn cmp(&self, cx: &mut CallContext, first: Value, second: Value) -> WebIdlResult<i16> {
        let cx = CallContext::new(cx.vm(), self.realm);

        // 1. Let a be the result of converting a value to a key with first. Rethrow any exceptions.
        let a = convert_a_value_to_a_key(&cx, first, None)?;

        // 2. If a is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if a.is_invalid() {
            return Err(Exception::Dom(
                "DataError",
                Utf16String::from_utf8("Failed to convert a value to a key"),
            ));
        }

        // 3. Let b be the result of converting a value to a key with second. Rethrow any exceptions.
        let b = convert_a_value_to_a_key(&cx, second, None)?;

        // 4. If b is "invalid value" or "invalid type", throw a "DataError" DOMException.
        if b.is_invalid() {
            return Err(Exception::Dom(
                "DataError",
                Utf16String::from_utf8("Failed to convert a value to a key"),
            ));
        }

        // 5. Return the results of comparing two keys with a and b.
        Ok(i16::from(Key::compare_two_keys(&a, &b)))
    }

    // https://w3c.github.io/IndexedDB/#dom-idbfactory-deletedatabase
    pub(crate) fn delete_database(
        &self,
        cx: &mut CallContext,
        name: Utf16String,
    ) -> WebIdlResult<Ref<IDBOpenDBRequest>> {
        let vm = cx.vm();
        let realm = self.realm;

        // 1. Let environment be this's relevant settings object.
        // 2. Let storageKey be the result of running obtain a storage key given environment.
        //    If failure is returned, then throw a "SecurityError" DOMException and abort these steps.
        let Some(storage_key) = obtain_a_storage_key(realm) else {
            return Err(Exception::Dom(
                "SecurityError",
                Utf16String::from_utf8("Failed to obtain a storage key"),
            ));
        };

        // 3. Let request be a new open request.
        let request = IDBOpenDBRequest::create(vm, realm);

        // 4. Run these steps in parallel:
        // NB: We could defer these steps, but it wouldn't be observable anyway, since delete_a_database() will
        //     put this work into a queue and then process it later in the event loop regardless.

        // 1. Let result be the result of deleting a database, with storageKey, name, and request.
        delete_a_database(
            vm,
            realm,
            storage_key,
            name,
            AnyIDBRequest::IDBOpenDBRequest(request),
            create_function_with(vm, (realm, request), |vm, &(realm, request), result: DeleteResult| {
                // 2. Set request’s processed flag to true.
                let request = AnyIDBRequest::IDBOpenDBRequest(request);
                request.set_processed(true);

                // 3. Queue a database task to run these steps:
                queue_a_database_task(
                    vm,
                    create_function(vm, (realm, request, result), |vm, &(realm, request, ref result), ()| {
                        match result {
                            // 1. If result is an error, set request’s error to result, set request’s done flag to true, and fire an event
                            //    named error at request with its bubbles and cancelable attributes initialized to true.
                            Err(error) => {
                                request.set_error(vm, Some(error.clone()));
                                request.set_done(true);
                                request.event_target().dispatch_event(&CppEvent::create(
                                    request.realm(),
                                    "error",
                                    true,
                                    true,
                                    false,
                                ));
                            }
                            // 2. Otherwise, set request’s result to undefined, set request’s done flag to true, and fire a version change
                            //    event named success at request with result and null.
                            Ok(value) => {
                                request.set_result(Value::UNDEFINED);
                                request.set_done(true);
                                fire_a_version_change_event(vm, realm, "success", request.event_target(), *value, None);
                            }
                        }
                    }),
                );
            }),
        );

        // 5. Return a new IDBOpenDBRequest object for request.
        Ok(request)
    }

    // https://w3c.github.io/IndexedDB/#dom-idbfactory-databases
    pub(crate) fn databases(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = self.realm;
        let promise = create_promise(vm, realm);

        // 1. Let environment be this's relevant settings object.
        // 2. Let storageKey be the result of running obtain a storage key given environment.
        //    If failure is returned, then return a promise rejected with a "SecurityError" DOMException
        let Some(storage_key) = obtain_a_storage_key(realm) else {
            let cx = CallContext::new(vm, realm);
            let error = Exception::Dom(
                "SecurityError",
                Utf16String::from_utf8("Failed to obtain a storage key"),
            )
            .throw(&cx);
            reject_promise(vm, promise, error.value());
            return Ok(promise);
        };

        // 3. Let p be a new promise.
        // NB: p is the promise created above.

        // 4. Run these steps in parallel:
        run_in_parallel(
            vm,
            create_function(
                vm,
                (realm, NoCells(storage_key), promise),
                |vm, &(realm, ref storage_key, promise), ()| {
                    crate::bindings::interface::with_temporary_execution_context_and_callbacks(realm, || {
                        // 1. Let databases be the set of databases in storageKey.
                        //    If this cannot be determined for any reason, then queue a database task to reject p with an appropriate error (e.g. an "UnknownError" DOMException) and terminate these steps.
                        let databases = Database::for_key(vm, &storage_key.0);

                        // 2. Let result be a new list.
                        // 3. For each db of databases:
                        //     1. If db's version is 0, then continue.
                        //     2. Let info be a new IDBDatabaseInfo dictionary.
                        //     3. Set info's name dictionary member to db's name.
                        //     4. Set info's version dictionary member to db's version.
                        //     5. Append info to result.
                        // 4. Queue a database task to resolve p with result.
                        // AD-HOC: p is resolved directly in a deferred invocation, not in a database task.
                        resolve_database_info_list_promise(vm, realm, promise, &databases.values());
                    });
                },
            ),
        );

        // 5. Return p.
        Ok(promise)
    }
}

// NB: The generated conversion of IDBDatabaseInfo is only built with the bindings, so the dictionaries are built here.
fn resolve_database_info_list_promise(vm: &Vm, realm: Gc<Realm>, promise: WebIdlPromise, databases: &[DatabaseRef]) {
    let result = Array::create(vm, realm, 0, None).must();

    let mut result_index: u64 = 0;
    for db in databases {
        // INTEROP: A database's version is not reported until its upgrade transaction commits, so that databases()
        //          does not pick up changes that have not been committed.
        let mut version = db.version();
        if let Some(upgrade_transaction) = db.upgrade_transaction() {
            version = upgrade_transaction.original_version();
        }

        if version == 0 {
            continue;
        }

        let info = Object::create(vm, realm, Some(realm.intrinsics().object_prototype(vm)));
        info.create_data_property(
            vm,
            &PropertyKey::from_utf8("name"),
            Value::from_string(PrimitiveString::create(vm, db.name())),
            None,
            None,
        )
        .must();
        #[allow(
            clippy::cast_precision_loss,
            reason = "the C++ converted the version to a double as well"
        )]
        info.create_data_property(
            vm,
            &PropertyKey::from_utf8("version"),
            Value::from_f64(version as f64),
            None,
            None,
        )
        .must();
        result
            .create_data_property_or_throw(vm, &PropertyKey::from_number(result_index), Value::from_object(info))
            .must();
        result_index += 1;
    }

    resolve_promise(vm, promise, Value::from_object(result));
}
