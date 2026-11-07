/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Realm, Trace};

use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::message_port::MessagePort;

unsafe extern "C" {
    fn web_audio_processor_global_scope(realm: *mut c_void) -> *mut c_void;
    fn web_audio_processor_take_pending_data(scope: *mut c_void, port: *mut *mut c_void) -> bool;
    fn web_audio_processor_scope_realm(scope: *mut c_void) -> *mut c_void;
}

// https://webaudio.github.io/web-audio-api/#AudioWorkletProcessor
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioWorkletProcessor {
    global_scope: CppCell,
    port: Ref<MessagePort>,
}

impl Wrappable for AudioWorkletProcessor {
    // Ensures wrappers for this processor are created in the worklet realm, not the page realm.
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        // Pin wrapper creation to the worklet realm.
        let realm = unsafe { web_audio_processor_scope_realm(self.global_scope.as_ptr()) };
        Some(unsafe { Gc::from_non_null(NonNull::new(realm.cast::<Realm>()).unwrap()) })
    }
}

impl AudioWorkletProcessor {
    // https://webaudio.github.io/web-audio-api/#dom-audioworkletprocessor-audioworkletprocessor
    // Runs when `super()` executes in a registered processor subclass during node instantiation.
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        // 1. Let nodeName be the value of the current pending processor construction data's node name... (steps
        //    collapsed): When the constructor is invoked outside of AudioWorkletNode instantiation, there is no
        //    pending processor construction data, and a TypeError must be thrown.
        let global_scope = unsafe { web_audio_processor_global_scope(cx.realm().as_ptr().cast()) };
        if global_scope.is_null() {
            return Err(Exception::type_error(
                "AudioWorkletProcessor can only be constructed in an AudioWorkletGlobalScope",
            ));
        }
        let mut port = core::ptr::null_mut();
        if !unsafe { web_audio_processor_take_pending_data(global_scope, &raw mut port) } {
            return Err(Exception::type_error(
                "AudioWorkletProcessor cannot be constructed directly; it is instantiated by AudioWorkletNode",
            ));
        }
        let deserialized_port = unsafe { cell_from_abi::<MessagePort>(port) };

        // 2. Set this's port to the pending construction data's transferred port.
        let processor = allocate(cx.vm(), |_| Self {
            global_scope: unsafe { CppCell::from_raw(global_scope) },
            port: deserialized_port,
        });
        Ok(processor)
    }

    pub(crate) fn port(&self, _: &mut CallContext) -> WebIdlResult<Ref<MessagePort>> {
        Ok(self.port)
    }
}
