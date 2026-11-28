/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::{Gc, Realm, Trace, Value, Vm};

use crate::bindings::interface::{InterfaceCell, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::abort_signal::AbortSignal;

#[derive(Trace)]
pub(crate) struct AbortController {
    signal: Ref<AbortSignal>,
}

impl Wrappable for AbortController {}

impl AbortController {
    // https://dom.spec.whatwg.org/#dom-abortcontroller-abortcontroller
    pub(crate) fn create(vm: &Vm) -> Ref<Self> {
        // 1. Let signal be a new AbortSignal object.
        let signal = AbortSignal::create(vm);

        // 2. Set this’s signal to signal.
        allocate(vm, |_| Self { signal })
    }

    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        Ok(Self::create(cx.vm()))
    }

    // https://dom.spec.whatwg.org/#dom-abortcontroller-signal
    pub(crate) fn get_signal(&self, _: &mut CallContext) -> WebIdlResult<Ref<AbortSignal>> {
        // The signal getter steps are to return this’s signal.
        Ok(self.signal)
    }

    // https://dom.spec.whatwg.org/#dom-abortcontroller-abort
    pub(crate) fn abort(&self, cx: &mut CallContext, reason: Option<Value>) -> WebIdlResult<()> {
        // The abort(reason) method steps are to signal abort on this with reason if it is given.
        self.signal_abort(cx, reason);
        Ok(())
    }

    // https://dom.spec.whatwg.org/#abortcontroller-signal-abort
    fn signal_abort(&self, cx: &CallContext, reason: Option<Value>) {
        // To signal abort on an AbortController controller with an optional reason, signal abort on controller’s signal with reason if it is given.
        self.signal.signal_abort(cx.vm(), reason, cx.realm());
    }
}

/// Creates an AbortController.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_abort_controller_create(vm: &Vm) -> *mut c_void {
    cell_into_abi(AbortController::create(vm))
}

/// Returns the controller’s signal.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_abort_controller_signal(controller: &InterfaceCell) -> *mut c_void {
    cell_into_abi(controller.object::<AbortController>().signal)
}

/// Aborts the controller with the reason in the realm.
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_abort_controller_abort(
    vm: &Vm,
    realm: Gc<Realm>,
    controller: &InterfaceCell,
    reason: Value,
) {
    let controller = controller.object::<AbortController>();
    controller.signal_abort(&CallContext::new(vm, realm), Some(reason));
}
