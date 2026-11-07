/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{Gc, Realm, Trace};

use super::audio_worklet_node::AudioWorkletNode;
use crate::bindings::cpp_cell::CppCell;
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_from_abi};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::html::message_port::MessagePort;

unsafe extern "C" {
    fn web_audio_processor_global_scope(realm: *mut c_void) -> *mut c_void;
    fn web_audio_processor_pending_node_reference(scope: *mut c_void) -> *mut c_void;
    fn web_audio_processor_pending_transferred_port(scope: *mut c_void) -> *mut c_void;
    fn web_audio_worklet_clear_pending_data(scope: *mut c_void);
    fn web_audio_processor_scope_realm(scope: *mut c_void) -> *mut c_void;
}

// https://webaudio.github.io/web-audio-api/#AudioWorkletProcessor
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioWorkletProcessor {
    global_scope: CppCell,
    pub(super) node_reference: Ref<AudioWorkletNode>,
    pub(super) callable_process: Cell<bool>,
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
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        let global_scope = unsafe { web_audio_processor_global_scope(cx.realm().as_ptr().cast()) };
        if global_scope.is_null() {
            return Err(Exception::type_error(
                "AudioWorkletProcessor can only be constructed in an AudioWorkletGlobalScope",
            ));
        }
        // 1. Let nodeReference be the result of looking up node reference on the pending processor construction data of the current AudioWorkletGlobalScope. Throw a TypeError exception if the slot is empty.
        let node_reference = unsafe { web_audio_processor_pending_node_reference(global_scope) };
        if node_reference.is_null() {
            return Err(Exception::type_error(
                "AudioWorkletProcessor cannot be constructed directly; it is instantiated by AudioWorkletNode",
            ));
        }
        let node_reference = unsafe { cell_from_abi::<AudioWorkletNode>(node_reference) };

        // 2. Let processor be the this value.
        let processor = allocate(cx.vm(), |_| Self {
            global_scope: unsafe { CppCell::from_raw(global_scope) },

            // 3. Set processor’s [[node reference]] to nodeReference.
            node_reference,

            // 4. Set processor’s [[callable process]] to true.
            callable_process: Cell::new(true),

            // 5. Let deserializedPort be the result of looking up transferred port from the pending processor construction data.
            port: {
                let deserialized_port =
                    unsafe { cell_from_abi::<MessagePort>(web_audio_processor_pending_transferred_port(global_scope)) };
                // 6. Set processor’s port to deserializedPort.
                deserialized_port
            },
        });

        // 7. Empty the pending processor construction data slot.
        unsafe { web_audio_worklet_clear_pending_data(global_scope) };
        Ok(processor)
    }

    pub(crate) fn port(&self, _: &mut CallContext) -> WebIdlResult<Ref<MessagePort>> {
        Ok(self.port)
    }
}
