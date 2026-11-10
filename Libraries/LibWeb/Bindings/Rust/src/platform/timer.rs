/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;

use libjs_rust::api::{Trace, Vm};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::promise::{pending_steps, run_pending_steps};

unsafe extern "C" {
    fn web_platform_timer_create_single_shot(
        milliseconds: u64,
        cell: *mut c_void,
        steps: unsafe extern "C" fn(*mut c_void, *mut c_void),
    ) -> *mut c_void;
    fn web_platform_timer_stop(timer: *mut c_void);
}

#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CppTimer(CppCell);

impl CppTimer {
    pub(crate) fn create_single_shot<C: Trace + Clone + 'static>(
        vm: &Vm,
        milliseconds: u64,
        captures: C,
        steps: fn(&Vm, C),
    ) -> Self {
        let cell = pending_steps(vm, captures, steps);
        // SAFETY: The cell is live, and LibWeb returns a live timer.
        Self(unsafe {
            CppCell::from_raw(web_platform_timer_create_single_shot(
                milliseconds,
                cell,
                run_pending_steps,
            ))
        })
    }

    pub(crate) fn stop(&self) {
        // SAFETY: The timer is live.
        unsafe { web_platform_timer_stop(self.0.as_ptr()) };
    }
}
