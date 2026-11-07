/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use super::node_message::SharedCppObject;
use crate::web_audio::control_message::FfiSharedCppObject;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct PipeCallbacks {
    pub service: FfiSharedCppObject,
    pub post_task: unsafe extern "C" fn(*mut c_void, *mut c_void, unsafe extern "C" fn(*mut c_void)),
    pub on_wakeup: unsafe extern "C" fn(*mut c_void),
}

pub(crate) struct CallbackService {
    pub owner: SharedCppObject,
    pub callbacks: PipeCallbacks,
}

// SAFETY: Callback services are atomically reference-counted and invoked on their control-thread event loop.
unsafe impl Send for CallbackService {}
unsafe impl Sync for CallbackService {}
impl CallbackService {
    pub(crate) unsafe fn new(callbacks: PipeCallbacks) -> Self {
        Self {
            owner: unsafe { callbacks.service.acquire() }.unwrap(),
            callbacks,
        }
    }

    pub(crate) unsafe fn post(&self, task: *mut c_void, invoke: unsafe extern "C" fn(*mut c_void)) {
        unsafe {
            (self.callbacks.post_task)(self.owner.pointer, task, invoke);
        }
    }
}
