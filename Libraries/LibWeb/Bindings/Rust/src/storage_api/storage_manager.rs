/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! StorageManager, https://storage.spec.whatwg.org/#storagemanager

use libjs_rust::api::{Gc, Object, PropertyKey, Realm, Trace, Value, Vm};

use crate::bindings::conversions::ToJs;
use crate::bindings::environment_settings_object::relevant_settings_object;
use crate::bindings::global_object::relevant_global_object;
use crate::bindings::interface::{
    Ref, Wrappable, allocate, create_dictionary_object, set_dictionary_member,
    with_temporary_execution_context_and_callbacks,
};
use crate::bindings::promise::{
    TaskSource, WebIdlPromise, create_promise, in_parallel, promise_realm, queue_a_global_task,
    reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::storage_api::storage_shelf::obtain_a_local_storage_shelf;

/// The dictionary StorageEstimate of StorageAPI/StorageManager.idl.
pub(crate) struct StorageEstimate {
    pub(crate) usage: Option<u64>,
    pub(crate) quota: Option<u64>,
}

// https://storage.spec.whatwg.org/#storagemanager
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StorageManager {
    realm: Gc<Realm>,
}

impl Wrappable for StorageManager {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

/// https://storage.spec.whatwg.org/#queue-a-storage-task
fn queue_a_storage_task<C: Trace + Copy + 'static>(vm: &Vm, global: Gc<Object>, captures: C, steps: fn(&Vm, C)) {
    // To queue a storage task given a global object global and a series of steps steps, queue a global task on the
    // storage task source with global and steps.
    queue_a_global_task(vm, global, TaskSource::Storage, captures, steps);
}

impl StorageManager {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<StorageManager> {
        allocate(vm, |_| StorageManager { realm })
    }

    // https://storage.spec.whatwg.org/#dom-storagemanager-estimate
    pub(crate) fn estimate(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // 1. Let promise be a new promise.
        let promise = create_promise(vm, self.realm);

        // 2. Let global be this's relevant global object.
        let global = relevant_global_object(self);

        // 3. Let shelf be the result of running obtain a local storage shelf with this's relevant settings object.
        let shelf = obtain_a_local_storage_shelf(vm, &relevant_settings_object(global));

        // 4. If shelf is failure, then reject promise with a TypeError.
        let Some(shelf) = shelf else {
            reject_promise_with_exception(
                vm,
                promise,
                Exception::type_error("Failed to obtain local storage shelf."),
            );
            return Ok(promise);
        };

        // 5. Otherwise, run these steps in parallel:
        in_parallel(
            vm,
            self.realm,
            (promise, global, shelf),
            |vm, (promise, global, shelf)| {
                // 1. Let usage be storage usage for shelf.
                let usage = shelf.storage_usage();

                // 2. Let quota be storage quota for shelf.
                let quota = shelf.storage_quota();

                // 3. Let dictionary be a new StorageEstimate dictionary whose usage member is usage and quota member is quota.
                // 4. If there was an internal error while obtaining usage and quota, then queue a storage task with global to
                //    reject promise with a TypeError.
                // 5. Otherwise, queue a storage task with global to resolve promise with dictionary.
                queue_a_storage_task(vm, global, (promise, usage, quota), |vm, (promise, usage, quota)| {
                    let realm = promise_realm(promise);
                    with_temporary_execution_context_and_callbacks(realm, || {
                        let dictionary = StorageEstimate {
                            usage: Some(usage),
                            quota: Some(quota),
                        };
                        resolve_promise(
                            vm,
                            promise,
                            storage_estimate_to_js(&CallContext::new(vm, realm), dictionary),
                        );
                    });
                });
            },
        );

        // 6. Return promise.
        Ok(promise)
    }
}

// NB: The generated conversion of StorageEstimate is only built with the bindings, so the dictionary is converted here.
fn storage_estimate_to_js(cx: &CallContext, dictionary: StorageEstimate) -> Value {
    let object = create_dictionary_object(cx);
    let members = [("usage", dictionary.usage), ("quota", dictionary.quota)];
    for (name, value) in members {
        if let Some(value) = value {
            let value = value.to_js(cx).expect("converting a number does not throw");
            set_dictionary_member(cx, object, &PropertyKey::from_utf8(name), value)
                .expect("defining a member of a new object does not throw");
        }
    }
    Value::from_object(object)
}
