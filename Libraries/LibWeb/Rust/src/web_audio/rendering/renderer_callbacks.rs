/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::ffi::c_void;

use ak::Span;

use super::node_message::SharedCppObject;
use crate::web_audio::control_message::FfiSharedCppObject;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct RendererCallbacks {
    pub service: FfiSharedCppObject,
    pub post_task: unsafe extern "C" fn(*mut c_void, *mut c_void, unsafe extern "C" fn(*mut c_void)),
    pub on_complete: unsafe extern "C" fn(*mut c_void),
    pub on_suspended: unsafe extern "C" fn(*mut c_void, f64),
    pub on_sources_ended: unsafe extern "C" fn(*mut c_void, Span<'_, u64>),
}

pub(crate) struct CallbackService {
    pub owner: SharedCppObject,
    pub callbacks: RendererCallbacks,
}

// SAFETY: The service is atomically reference-counted, and callbacks run on its control-thread event loop.
unsafe impl Send for CallbackService {}
unsafe impl Sync for CallbackService {}
impl CallbackService {
    pub(crate) unsafe fn new(callbacks: RendererCallbacks) -> Self {
        Self {
            owner: unsafe { callbacks.service.acquire() }.unwrap(),
            callbacks,
        }
    }

    pub(crate) unsafe fn post(&self, context: *mut c_void, invoke: unsafe extern "C" fn(*mut c_void)) {
        unsafe {
            (self.callbacks.post_task)(self.owner.pointer, context, invoke);
        }
    }
}
