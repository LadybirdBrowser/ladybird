/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::callback::PromiseCallbackFunction;
use crate::bindings::environment_settings_object::CppEnvironmentSettingsObject;
use crate::bindings::promise::{pending_steps, run_pending_steps};
use crate::storage_api::storage_key::obtain_a_storage_key;
use crate::web_locks::agent;
use crate::web_locks::lock_table::{self, LockManagerKey};

unsafe extern "C" {
    fn web_locks_queue_task(
        realm: *mut c_void,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
    fn web_locks_queue_callback_task(
        callback: *mut c_void,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
}

// https://w3c.github.io/web-locks/#obtain-a-lock-manager
pub(crate) fn obtain_lock_manager(environment: &CppEnvironmentSettingsObject) -> Option<LockManagerKey> {
    // 1. Let map be the result of obtaining a local storage bottle map given environment and "web-locks".
    // 2. If map is failure, then return failure.
    // NB: Obtaining the map fails when obtaining a storage key for environment fails.
    let storage_key = obtain_a_storage_key(environment)?;

    // 3. Let bottle be map’s associated storage bottle.
    // 4. Return bottle’s associated lock manager.
    // NB: The lock table has the lock manager of each storage key, which all agents of this process share.
    // FIXME: Agents of the same storage key in other WebContent processes, such as those of other tabs or a shared
    //        worker that another process hosts, have lock managers of their own.
    Some(LockManagerKey::new(storage_key))
}

pub(crate) fn queue_web_locks_task<C: Trace + Clone + 'static>(
    vm: &Vm,
    realm: Gc<Realm>,
    captures: C,
    steps: fn(&Vm, C),
) {
    let cell = pending_steps(vm, captures, steps);
    // SAFETY: LibWeb retains the realm and the traced captures until the task runs.
    unsafe { web_locks_queue_task(realm.as_ptr().cast(), cell, run_pending_steps) };
}

pub(crate) fn queue_web_locks_callback_task<C: Trace + Clone + 'static>(
    vm: &Vm,
    callback: &PromiseCallbackFunction,
    captures: C,
    steps: fn(&Vm, C),
) {
    let cell = pending_steps(vm, captures, steps);
    // SAFETY: LibWeb retains the callback and the traced captures until the task runs.
    unsafe { web_locks_queue_callback_task(callback.as_ptr(), cell, run_pending_steps) };
}

/// The id under which the lock table knows the agent of this thread.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_web_locks_agent_id() -> u64 {
    agent::agent_id()
}

/// Terminates the remaining locks and requests of the agent, from any thread.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_web_locks_terminate_remaining_locks_and_requests_of_agent(agent_id: u64) {
    lock_table::terminate_remaining_locks_and_requests_of_agent(agent_id);
}

/// Terminates the remaining locks and requests of the client, an environment of the agent of this thread.
///
/// # Safety
///
/// `environment` must be a live environment settings object.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_web_locks_terminate_remaining_locks_and_requests_of_client(
    vm: &Vm,
    environment: *mut c_void,
) {
    // NB: An agent that never used Web Locks has nothing in the lock table.
    if !agent::has_agent_id() {
        return;
    }
    // SAFETY: The caller passes a live environment settings object.
    let environment = unsafe { CppEnvironmentSettingsObject::from_raw(environment) };
    let client_id = Utf16String::from_utf8(&environment.id());
    queue_web_locks_task(vm, environment.realm(), client_id, |vm, client_id| {
        agent::terminate_remaining_locks_and_requests_of_client(vm, &client_id);
    });
}
