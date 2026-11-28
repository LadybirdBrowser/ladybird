/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://w3c.github.io/requestidlecallback/#the-idledeadline-interface

use core::ffi::c_void;

use libjs_rust::api::{Trace, Vm};

use crate::bindings::interface::{Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_high_resolution_time_current_high_resolution_time(global_object: *mut c_void) -> f64;
    fn web_request_idle_callback_idle_deadline_deadline(global_object: *mut c_void) -> f64;
}

#[derive(Trace)]
pub(crate) struct IdleDeadline {
    did_timeout: bool,
}

impl Wrappable for IdleDeadline {}

impl IdleDeadline {
    // https://w3c.github.io/requestidlecallback/#dom-idledeadline-timeremaining
    pub(crate) fn time_remaining(&self, cx: &mut CallContext) -> WebIdlResult<f64> {
        if self.did_timeout {
            return Ok(0.0);
        }

        // 1. Let now be a DOMHighResTimeStamp representing current high resolution time in milliseconds.
        let global = cx.realm().global_object();
        // SAFETY: The global object is live.
        let now = unsafe { web_high_resolution_time_current_high_resolution_time(global.as_ptr().cast()) };

        // 2. Let deadline be the result of calling IdleDeadline's get deadline time algorithm.
        // SAFETY: The global object is live.
        let deadline = unsafe { web_request_idle_callback_idle_deadline_deadline(global.as_ptr().cast()) };

        // 3. Let timeRemaining be deadline - now.
        let mut time_remaining = deadline - now;

        // 4. If timeRemaining is negative, set it to 0.
        if time_remaining < 0.0 {
            time_remaining = 0.0;
        }

        // 5. Return timeRemaining.
        // NB: Coarsened to milliseconds.
        Ok(time_remaining.ceil())
    }

    // https://w3c.github.io/requestidlecallback/#dom-idledeadline-didtimeout
    pub(crate) fn did_timeout(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.did_timeout)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_rust_idle_deadline_create(vm: &Vm, did_timeout: bool) -> *mut c_void {
    cell_into_abi(allocate(vm, |_| IdleDeadline { did_timeout }))
}
