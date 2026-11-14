/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! WindowOrWorkerGlobalScope, https://html.spec.whatwg.org/multipage/webappapis.html#windoworworkerglobalscope

use core::ffi::c_void;

use libjs_rust::api::{Gc, Object, Trace, Value, Vm};

use crate::bindings::interface::{Ref, cell_from_abi};
use crate::bindings::promise::{pending_steps, run_pending_steps};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::transfer_record::TransferRecord;
use crate::html::structured_serialize::{structured_deserialize_with_transfer, structured_serialize_with_transfer};
use crate::trusted_types::trusted_type_policy_factory::TrustedTypePolicyFactory;

unsafe extern "C" {
    fn web_html_window_or_worker_global_scope_run_steps_after_a_timeout(
        global_object: *mut c_void,
        milliseconds: u64,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void),
    );
    fn web_html_window_or_worker_global_scope_trusted_types(global_object: *mut c_void) -> *mut c_void;
}

// https://html.spec.whatwg.org/multipage/structured-data.html#dom-structuredclone
pub(crate) fn structured_clone(cx: &CallContext, value: Value, transfer: &[Gc<Object>]) -> WebIdlResult<Value> {
    // 1. Let serialized be ? StructuredSerializeWithTransfer(value, options["transfer"]).
    let serialized = structured_serialize_with_transfer(cx, value, transfer)?;

    // 2. Let deserializeRecord be ? StructuredDeserializeWithTransfer(serialized, this's relevant realm).
    let deserialize_record = structured_deserialize_with_transfer(cx, serialized);
    TransferRecord::discard(serialized);

    // 3. Return deserializeRecord.[[Deserialized]].
    Ok(deserialize_record?.deserialized)
}

// https://w3c.github.io/trusted-types/dist/spec/#dom-windoworworkerglobalscope-trustedtypes
pub(crate) fn trusted_types(global_object: Gc<Object>) -> Ref<TrustedTypePolicyFactory> {
    // SAFETY: The global object is a live Window or WorkerGlobalScope, whose live factory LibWeb returns.
    unsafe {
        cell_from_abi(web_html_window_or_worker_global_scope_trusted_types(
            global_object.as_ptr().cast(),
        ))
    }
}

/// https://html.spec.whatwg.org/multipage/webappapis.html#windoworworkerglobalscope
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppWindowOrWorkerGlobalScope(pub(crate) Gc<Object>);

impl CppWindowOrWorkerGlobalScope {
    // https://html.spec.whatwg.org/multipage/timers-and-user-prompts.html#run-steps-after-a-timeout
    pub(crate) fn run_steps_after_a_timeout<C: Trace + Clone + 'static>(
        &self,
        vm: &Vm,
        milliseconds: u64,
        captures: C,
        steps: fn(&Vm, C),
    ) {
        let cell = pending_steps(vm, captures, steps);
        // SAFETY: The global object is a live Window or WorkerGlobalScope, and LibWeb keeps the steps alive until the
        // timeout completes.
        unsafe {
            web_html_window_or_worker_global_scope_run_steps_after_a_timeout(
                self.0.as_ptr().cast(),
                milliseconds,
                cell,
                run_pending_steps,
            );
        }
    }
}
