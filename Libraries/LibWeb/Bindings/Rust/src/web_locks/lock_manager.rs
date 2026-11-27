/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;

use ak::{Utf16String, Utf16StringUnits};
use libjs_rust::api::{Gc, NativeFunction, PropertyKey, Realm, Trace, Value, Vm};

use crate::bindings::callback::PromiseCallbackFunction;
#[cfg(web_idl_bindings)]
use crate::bindings::conversions::ToJs;
use crate::bindings::environment_settings_object::{CppEnvironmentSettingsObject, relevant_settings_object};
use crate::bindings::global_object::window_from_global_object;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi, wrap};
use crate::bindings::promise::{
    WebIdlPromise, create_promise, create_rejected_promise, create_rejected_promise_from_exception, promise_realm,
    react_to_promise, resolve_promise,
};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::dom::abort_signal::{AbortAlgorithm, AbortSignal};
use crate::indexed_db::internal::function::NoCells;
use crate::web_locks::abstract_operations::{obtain_lock_manager, queue_web_locks_callback_task, queue_web_locks_task};
use crate::web_locks::agent::{act_on_notices, agent_id, keep_lock, keep_request, next_entry_id, take_request};
use crate::web_locks::lock::{LOCK_DATA_CLASS, Lock, LockData};
use crate::web_locks::lock_request::{LOCK_REQUEST_CLASS, LockRequest, LockRequestRef};
use crate::web_locks::lock_table::{self, AgentId, LockManagerKey, RequestOutcome};

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
    pub(crate) signal: Option<Ref<AbortSignal>>,
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

// https://w3c.github.io/web-locks/#lockmanager
// NB: This is the LockManager object of an environment. The lock managers themselves are in the lock table.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct LockManager {
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
        let signal = options.signal;

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
        let Some(manager) = obtain_lock_manager(environment) else {
            return Ok(create_rejected_promise_from_exception(
                vm,
                realm,
                Exception::Dom(
                    "SecurityError",
                    Utf16String::from_utf8("This environment has no storage key for locks"),
                ),
            ));
        };

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
            vm,
            realm,
            promise,
            agent_id(),
            Utf16String::from_utf8(&environment.id()),
            manager,
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
        let Some(manager) = obtain_lock_manager(environment) else {
            return Ok(create_rejected_promise_from_exception(
                vm,
                self.realm,
                Exception::Dom(
                    "SecurityError",
                    Utf16String::from_utf8("This environment has no storage key for locks"),
                ),
            ));
        };

        // 4. Let promise be a new promise.
        let promise = create_promise(vm, self.realm);

        // 5. Enqueue the steps to snapshot the lock state for manager with promise to the lock task queue.
        queue_web_locks_task(vm, self.realm, (NoCells(manager), promise), |vm, (manager, promise)| {
            Self::snapshot_lock_state(vm, &manager.0, promise);
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
        vm: &Vm,
        realm: Gc<Realm>,
        promise: WebIdlPromise,
        agent: AgentId,
        client_id: Utf16String,
        manager: LockManagerKey,
        callback: PromiseCallbackFunction,
        name: Utf16String,
        mode: LockMode,
        if_available: bool,
        steal: bool,
        signal: Option<Ref<AbortSignal>>,
    ) -> LockRequestRef {
        // 1. Let request be a new lock request (agent, clientId, manager, name, mode, callback, promise, signal).
        let request = LOCK_REQUEST_CLASS.allocate(vm, |_| LockRequest {
            id: next_entry_id(),
            agent,
            client_id,
            manager_key: manager,
            realm,
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
                (request, *signal),
                |vm, (request, signal): &(LockRequestRef, Ref<AbortSignal>)| {
                    LockRequest::signal_to_abort_request(*request, vm, signal);
                    Ok(Value::UNDEFINED)
                },
                0,
                &PropertyKey::from_utf8(""),
                Some(realm),
                None,
                None,
            );
            request
                .abort_algorithm_id
                .set(signal.add_abort_algorithm(vm, AbortAlgorithm::Rust(algorithm)));
        }

        // 3. Enqueue the following steps to the lock task queue:
        queue_web_locks_task(
            vm,
            realm,
            (request, if_available, steal),
            |vm, (request, if_available, steal)| {
                // NB: The agent keeps the request's objects before its entry is in the lock table, where a grant can
                //     come from any thread.
                keep_request(vm, request.id, request);

                // 1. to 6.
                // NB: The lock table does these steps, and leaves a notice for the agent of each request that it
                //     grants and of each lock that it steals.
                match lock_table::request_lock(&request.manager_key, request.entry(), if_available, steal) {
                    RequestOutcome::Enqueued => {}
                    // 5. Otherwise, run these steps:
                    //    1. If ifAvailable is true and request is not grantable, then enqueue the following steps on
                    //       callback’s relevant settings object’s responsible event loop:
                    RequestOutcome::NotGrantable => {
                        take_request(vm, request.id);
                        queue_web_locks_callback_task(vm, &request.callback, request, |vm, request| {
                            // 1. Let r be the result of invoking callback with null as the only argument.
                            let r = request.callback.invoke(None, &[Value::NULL]);

                            // 2. Resolve promise with r and abort these steps.
                            resolve_promise(vm, request.promise, Value::from_object(r.promise()));
                        });

                        // AD-HOC: Abort the enclosing queue steps so an unavailable request is not also granted later.
                        return;
                    }
                    // NB: The agent of the request has ended, and nothing will act on a grant to it.
                    RequestOutcome::AgentEnded => {
                        take_request(vm, request.id);
                        return;
                    }
                }
                act_on_notices(vm);
            },
        );

        // 4. Return request.
        request
    }

    /// Steps 3 to 12 and 14 of processing the lock request queue, https://w3c.github.io/web-locks/#process-the-lock-request-queue,
    /// which the agent of a request that the lock table granted does on its own lock task queue.
    pub(crate) fn grant_request(vm: &Vm, request: LockRequestRef) {
        // 3. Let agent be request’s agent.
        // NB: This is the current agent.

        // 4. Let manager be request’s manager.
        let manager = request.manager_key.clone();

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
        let signal = request.signal;

        // 11. Let waiting be a new promise.
        let realm = request.realm;
        let waiting = create_promise(vm, realm);

        // 12. Let lock be a new lock with agent agent, clientId clientId, manager manager, mode mode, name name,
        //     released promise p, and waiting promise waiting.
        let lock = LOCK_DATA_CLASS.allocate(vm, |_| LockData {
            id: request.id,
            client_id,
            manager_key: manager,
            realm,
            name,
            mode,
            released_promise: p,
            waiting_promise: waiting,
        });
        keep_lock(vm, lock.id, lock);

        // 13. Append lock to manager’s held lock set.
        // NB: The lock table did this as it granted the request.

        // 14. Enqueue the following steps on callback’s relevant settings object’s responsible event loop:
        queue_web_locks_callback_task(
            vm,
            &callback,
            (request, lock, signal, callback.clone(), waiting),
            |vm, (request, lock, signal, callback, waiting)| {
                // 1. If signal is present, then run these steps:
                if let Some(signal) = &signal {
                    // 1. If signal is aborted, then run these steps:
                    if signal.aborted() {
                        // 1. Enqueue the following step to the lock task queue:
                        queue_web_locks_task(vm, lock.realm, lock, |vm, lock| {
                            // 1. Release the lock lock.
                            LockData::release_lock(lock, vm);
                        });

                        // 2. Return.
                        return;
                    }

                    // 2. Remove the algorithm signal to abort the request request from signal.
                    if let Some(id) = request.abort_algorithm_id.get() {
                        signal.remove_abort_algorithm(vm, id);
                    }
                }

                // 2. Let r be the result of invoking callback with a new Lock object associated with lock as the only
                //    argument.
                let cx = CallContext::new(vm, lock.realm);
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

    // https://w3c.github.io/web-locks/#snapshot-the-lock-state
    fn snapshot_lock_state(vm: &Vm, manager: &LockManagerKey, promise: WebIdlPromise) {
        // 1. Assert: these steps are running on the lock task queue.

        // 2. to 5.
        let (held, pending) = lock_table::snapshot_lock_state(manager);

        // 6. Resolve promise with «[ "held" → held, "pending" → pending ]».
        let snapshot = LockManagerSnapshot {
            held: Some(held),
            pending: Some(pending),
        };
        let cx = CallContext::new(vm, promise_realm(promise));
        resolve_promise(vm, promise, snapshot.into_js(&cx));
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_web_locks_create_lock_manager(vm: &Vm, realm: Gc<Realm>) -> *mut c_void {
    cell_into_abi(LockManager::create(vm, realm))
}
