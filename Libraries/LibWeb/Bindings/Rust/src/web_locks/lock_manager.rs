/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;

use ak::{Utf16String, Utf16StringUnits};
use libjs_rust::api::{Gc, GcRefCell, NativeFunction, PropertyKey, Realm, Trace, Value, Vm};

use crate::bindings::callback::PromiseCallbackFunction;
use crate::bindings::conversions::CppInterface;
#[cfg(web_idl_bindings)]
use crate::bindings::conversions::ToJs;
use crate::bindings::environment_settings_object::{CppEnvironmentSettingsObject, relevant_settings_object};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi, realm_arguments, wrap};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise, create_rejected_promise_from_exception, react_to_promise,
    reject_promise_with_exception, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::abort_signal::AbortSignal;
use crate::web_locks::abstract_operations::{
    LockRequestQueueMap, LockRequestQueueRef, obtain_lock_manager, queue_web_locks_callback_task, queue_web_locks_task,
};
use crate::web_locks::lock::{LOCK_DATA_CLASS, Lock, LockData, LockDataRef};
use crate::web_locks::lock_request::{LOCK_REQUEST_CLASS, LockRequest, LockRequestRef};

#[derive(Clone, Copy, Default, PartialEq, Eq)]
pub(crate) enum LockMode {
    Shared,
    #[default]
    Exclusive,
}

#[derive(Default)]
pub(crate) struct LockOptions {
    pub(crate) mode: LockMode,
    pub(crate) if_available: bool,
    pub(crate) steal: bool,
    pub(crate) signal: Option<CppInterface>,
}

pub(crate) struct LockManagerSnapshot {
    pub(crate) held: Option<Vec<LockInfo>>,
    pub(crate) pending: Option<Vec<LockInfo>>,
}

impl LockManagerSnapshot {
    fn into_js(self, cx: &CallContext) -> Value {
        // NB: The generated bindings convert the dictionary, which a build of the crate on its own leaves out.
        #[cfg(web_idl_bindings)]
        return ToJs::to_js(self, cx).expect("a lock manager snapshot converts to a JS value");
        #[cfg(not(web_idl_bindings))]
        {
            let _ = (self, cx);
            unreachable!("only the generated bindings resolve a promise with the dictionary")
        }
    }
}

pub(crate) struct LockInfo {
    pub(crate) name: Option<Utf16String>,
    pub(crate) mode: Option<LockMode>,
    pub(crate) client_id: Option<Utf16String>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct LockManager {
    pub(crate) held_lock_set: GcRefCell<Vec<LockDataRef>>,
    pub(crate) lock_request_queue_map: GcRefCell<LockRequestQueueMap>,
    environment: CppEnvironmentSettingsObject,
    pub(crate) realm: Gc<Realm>,
}

impl Wrappable for LockManager {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }
}

impl LockManager {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        allocate(vm, |_| Self {
            held_lock_set: GcRefCell::default(),
            lock_request_queue_map: GcRefCell::default(),
            environment: relevant_settings_object(realm.global_object()),
            realm,
        })
    }

    // https://w3c.github.io/web-locks/#dom-lockmanager-request
    pub(crate) fn request(
        &self,
        cx: &mut CallContext,
        name: Utf16String,
        options: Option<LockOptions>,
        callback: PromiseCallbackFunction,
    ) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();
        let realm = self.realm;

        // 1. If options was not passed, then let options be a new LockOptions dictionary with default members.
        let options = options.unwrap_or_default();
        let signal = options.signal.map(AbortSignal::from_wrapper);

        // 2. Let environment be this’s relevant settings object.
        let environment = &self.environment;

        // 3. If environment’s relevant global object’s associated Document is not fully active, then return a promise
        //    rejected with a "InvalidStateError" DOMException.
        if let Some(window) = window_from_global_object(environment.global_object())
            && !window.associated_document().is_fully_active()
        {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_utf8("Document is not fully active"),
                ),
            ));
        }

        // 4. Let manager be the result of obtaining a lock manager given environment. If that returned failure, then
        //    return a promise rejected with a "SecurityError" DOMException.
        let manager = obtain_lock_manager(environment);

        // 5. If name starts with U+002D HYPHEN-MINUS (-), then return a promise rejected with a "NotSupportedError"
        //    DOMException.
        let starts_with_hyphen = match name.as_units() {
            Utf16StringUnits::Ascii(units) | Utf16StringUnits::Latin1(units) => units.first() == Some(&b'-'),
            Utf16StringUnits::Utf16(units) => units.first() == Some(&u16::from(b'-')),
        };
        if starts_with_hyphen {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_utf8("Lock names starting with '-' are reserved"),
                ),
            ));
        }

        // 6. If both options["steal"] and options["ifAvailable"] are true, then return a promise rejected with a
        //    "NotSupportedError" DOMException.
        if options.steal && options.if_available {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_utf8("Lock options 'steal' and 'ifAvailable' cannot both be true"),
                ),
            ));
        }

        // 7. If options["steal"] is true and options["mode"] is not "exclusive", then return a promise rejected with a
        //    "NotSupportedError" DOMException.
        if options.steal && options.mode != LockMode::Exclusive {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_utf8("Lock option 'steal' can only be used with exclusive locks"),
                ),
            ));
        }

        // 8. If options["signal"] exists, and either of options["steal"] or options["ifAvailable"] is true, then return
        //    a promise rejected with a "NotSupportedError" DOMException.
        if signal.is_some() && (options.steal || options.if_available) {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::Dom(
                    "NotSupportedError",
                    Utf16String::from_utf8("Lock option 'signal' cannot be combined with 'steal' or 'ifAvailable'"),
                ),
            ));
        }

        // 9. If options["signal"] exists and is aborted, then return a promise rejected with options["signal"]'s abort
        //    reason.
        if let Some(signal) = &signal
            && signal.aborted()
        {
            return Ok(create_rejected_promise(vm, realm, signal.reason()));
        }

        // 10. Let promise be a new promise.
        let promise = create_promise(vm, realm);

        // 11. Request a lock with promise, the current agent, environment’s id, manager, callback, name,
        //     options["mode"], options["ifAvailable"], options["steal"], and options["signal"].
        Self::request_lock(
            manager,
            vm,
            promise,
            callback,
            name,
            options.mode,
            options.if_available,
            options.steal,
            signal,
        );

        // 12. Return promise.
        Ok(promise)
    }

    // https://w3c.github.io/web-locks/#dom-lockmanager-query
    pub(crate) fn query(&self, cx: &mut CallContext) -> WebIdlResult<WebIdlPromise> {
        let vm = cx.vm();

        // 1. Let environment be this’s relevant settings object.
        let environment = &self.environment;

        // 2. If environment’s relevant global object’s associated Document is not fully active, then return a promise
        //    rejected with a "InvalidStateError" DOMException.
        if let Some(window) = window_from_global_object(environment.global_object())
            && !window.associated_document().is_fully_active()
        {
            return Ok(create_rejected_promise_from_exception(
                vm,
                self.realm,
                Exception::Dom(
                    "InvalidStateError",
                    Utf16String::from_utf8("Document is not fully active"),
                ),
            ));
        }

        // 3. Let manager be the result of obtaining a lock manager given environment. If that returned failure, then
        //    return a promise rejected with a "SecurityError" DOMException.
        let manager = obtain_lock_manager(environment);

        // 4. Let promise be a new promise.
        let promise = create_promise(vm, self.realm);

        // 5. Enqueue the steps to snapshot the lock state for manager with promise to the lock task queue.
        queue_web_locks_task(vm, self.realm, (manager, promise), |vm, (manager, promise)| {
            manager.snapshot_lock_state(vm, promise);
        });

        // 6. Return promise.
        Ok(promise)
    }

    // https://w3c.github.io/web-locks/#request-a-lock
    #[allow(
        clippy::too_many_arguments,
        reason = "the specification gives the algorithm these parameters"
    )]
    fn request_lock(
        manager: Ref<Self>,
        vm: &Vm,
        promise: WebIdlPromise,
        callback: PromiseCallbackFunction,
        name: Utf16String,
        mode: LockMode,
        if_available: bool,
        steal: bool,
        signal: Option<AbortSignal>,
    ) -> LockRequestRef {
        // 1. Let request be a new lock request (agent, clientId, manager, name, mode, callback, promise, signal).
        let request = LOCK_REQUEST_CLASS.allocate(vm, |_| LockRequest {
            client_id: Utf16String::from_utf8(&manager.environment.id()),
            manager,
            name,
            mode,
            callback,
            promise,
            signal,
            abort_algorithm_id: Cell::new(None),
        });

        // 2. If signal is present, then add the algorithm signal to abort the request request with signal to signal.
        if let Some(signal) = &request.signal {
            let algorithm = NativeFunction::create(
                vm,
                (request, signal.clone()),
                |vm, (request, signal): &(LockRequestRef, AbortSignal)| {
                    LockRequest::signal_to_abort_request(*request, vm, signal);
                    Ok(Value::UNDEFINED)
                },
                0,
                &PropertyKey::from_utf8(""),
                Some(manager.realm),
                None,
                None,
            );
            request.abort_algorithm_id.set(signal.add_algorithm(algorithm));
        }

        // 3. Enqueue the following steps to the lock task queue:
        queue_web_locks_task(
            vm,
            manager.realm,
            (manager, request, if_available, steal),
            |vm, (manager, request, if_available, steal)| {
                // 1. Let queueMap be manager’s lock request queue map.
                let queue_map = &manager.lock_request_queue_map;

                // 2. Let queue be the result of getting the lock request queue from queueMap for name.
                let queue = queue_map.borrow_mut(vm).get_lock_request_queue(vm, &request.name);

                // 3. Let held be manager’s held lock set.
                let held = &manager.held_lock_set;

                // 4. If steal is true, then run these steps:
                if steal {
                    // 1. For each lock of held:
                    let mut i = 0;
                    while i < held.borrow().len() {
                        let lock = held.borrow()[i];

                        // 1. If lock’s name is name, then run these steps:
                        if lock.name == request.name {
                            // 1. Remove lock from held.
                            held.borrow_mut(vm).remove(i);

                            // 2. Reject lock’s released promise with an "AbortError" DOMException.
                            reject_promise_with_exception(
                                vm,
                                lock.released_promise,
                                Exception::Dom("AbortError", Utf16String::from_utf8("Lock was stolen")),
                            );
                        } else {
                            i += 1;
                        }
                    }

                    // 2. Prepend request in queue.
                    queue.0.borrow_mut(vm).push_front(request);
                }
                // 5. Otherwise, run these steps:
                else {
                    // 1. If ifAvailable is true and request is not grantable, then enqueue the following steps on
                    //    callback’s relevant settings object’s responsible event loop:
                    if if_available && !LockRequest::is_grantable(request, vm) {
                        queue_web_locks_callback_task(vm, &request.callback, request, |vm, request| {
                            // 1. Let r be the result of invoking callback with null as the only argument.
                            let r = request.callback.invoke(None, &[Value::NULL]);

                            // 2. Resolve promise with r and abort these steps.
                            resolve_promise(vm, request.promise, Value::from_object(r.promise()));
                        });

                        // AD-HOC: Abort the enclosing queue steps so an unavailable request is not also granted later.
                        return;
                    }

                    // 2. Enqueue request in queue.
                    queue.0.borrow_mut(vm).push_back(request);
                }

                // 6. Process the lock request queue queue.
                Self::process_lock_request_queue(vm, queue);
            },
        );

        // 4. Return request.
        request
    }

    // https://w3c.github.io/web-locks/#process-the-lock-request-queue
    pub(crate) fn process_lock_request_queue(vm: &Vm, queue: LockRequestQueueRef) {
        // 1. Assert: these steps are running on the lock task queue.

        // 2. For each request of queue:
        while let Some(request) = { queue.0.borrow().front().copied() } {
            // 1. If request is not grantable, then return.
            // NOTE: Only the first item in a queue is grantable. Therefore, if something is not grantable then all the
            //       following items are automatically not grantable.
            if !LockRequest::is_grantable(request, vm) {
                return;
            }

            // 2. Remove request from queue.
            queue.0.borrow_mut(vm).pop_front();

            // FIXME: 3. Let agent be request’s agent.

            // 4. Let manager be request’s manager.
            let manager = request.manager;

            // 5. Let clientId be request’s clientId.
            let client_id = request.client_id.clone();

            // 6. Let name be request’s name.
            let name = request.name.clone();

            // 7. Let mode be request’s mode.
            let mode = request.mode;

            // 8. Let callback be request’s callback.
            let callback = request.callback.clone();

            // 9. Let p be request’s promise.
            let p = request.promise;

            // 10. Let signal be request’s signal.
            let signal = request.signal.clone();

            // 11. Let waiting be a new promise.
            let waiting = create_promise(vm, manager.realm);

            // 12. Let lock be a new lock with agent agent, clientId clientId, manager manager, mode mode, name name,
            //     released promise p, and waiting promise waiting.
            let lock = LOCK_DATA_CLASS.allocate(vm, |_| LockData {
                client_id,
                manager,
                name,
                mode,
                released_promise: p,
                waiting_promise: waiting,
            });

            // 13. Append lock to manager’s held lock set.
            manager.held_lock_set.borrow_mut(vm).push(lock);

            // 14. Enqueue the following steps on callback’s relevant settings object’s responsible event loop:
            queue_web_locks_callback_task(
                vm,
                &callback,
                (manager, request, lock, signal, callback.clone(), waiting),
                |vm, (manager, request, lock, signal, callback, waiting)| {
                    // 1. If signal is present, then run these steps:
                    if let Some(signal) = &signal {
                        // 1. If signal is aborted, then run these steps:
                        if signal.aborted() {
                            // 1. Enqueue the following step to the lock task queue:
                            queue_web_locks_task(vm, manager.realm, lock, |vm, lock| {
                                // 1. Release the lock lock.
                                LockData::release_lock(lock, vm);
                            });

                            // 2. Return.
                            return;
                        }

                        // 2. Remove the algorithm signal to abort the request request from signal.
                        if let Some(id) = request.abort_algorithm_id.get() {
                            signal.remove_algorithm(id);
                        }
                    }

                    // 2. Let r be the result of invoking callback with a new Lock object associated with lock as the
                    //    only argument.
                    let cx = CallContext::new(vm, manager.realm);
                    let r = callback.invoke(None, &[Value::from_object(wrap(&cx, Lock::create(vm, lock)))]);

                    // 3. Resolve waiting with r.
                    resolve_promise(vm, waiting, Value::from_object(r.promise()));
                },
            );

            react_to_promise(
                vm,
                waiting,
                lock,
                Some(LockData::waiting_promise_settled),
                Some(LockData::waiting_promise_settled),
            );
        }
    }

    // https://w3c.github.io/web-locks/#snapshot-the-lock-state
    fn snapshot_lock_state(&self, vm: &Vm, promise: WebIdlPromise) {
        // 1. Assert: these steps are running on the lock task queue.

        // 2. Let pending be a new list.
        let mut pending = Vec::new();

        // 3. For each queue of manager’s lock request queue map’s values:
        for queue in self.lock_request_queue_map.borrow().0.values() {
            // 1. For each request of queue:
            for request in queue.0.borrow().iter() {
                // 1. Append «[ "name" → request’s name, "mode" → request’s mode, "clientId" → request’s clientId ]»
                //    to pending.
                pending.push(LockInfo {
                    name: Some(request.name.clone()),
                    mode: Some(request.mode),
                    client_id: Some(request.client_id.clone()),
                });
            }
        }

        // 4. Let held be a new list.
        let mut held = Vec::new();

        // 5. For each lock of manager’s held lock set:
        for lock in self.held_lock_set.borrow().iter() {
            // 1. Append «[ "name" → lock’s name, "mode" → lock’s mode, "clientId" → lock’s clientId ]» to held.
            held.push(LockInfo {
                name: Some(lock.name.clone()),
                mode: Some(lock.mode),
                client_id: Some(lock.client_id.clone()),
            });
        }

        // 6. Resolve promise with «[ "held" → held, "pending" → pending ]».
        let snapshot = LockManagerSnapshot {
            held: Some(held),
            pending: Some(pending),
        };
        let cx = CallContext::new(vm, self.realm);
        resolve_promise(vm, promise, snapshot.into_js(&cx));
    }
}

/// # Safety
///
/// The pointers must be the live VM and a live realm.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_web_locks_create_lock_manager(vm: *mut c_void, realm: *mut c_void) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, realm) };
    cell_into_abi(LockManager::create(vm, realm))
}
