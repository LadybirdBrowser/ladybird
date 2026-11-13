/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechRecognitionEvent, https://wicg.github.io/speech-api/#speechrecognitionevent

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use super::speech_recognition_result_list::SpeechRecognitionResultList;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

pub(crate) struct SpeechRecognitionEventInit {
    pub(crate) parent: EventInit,
    pub(crate) result_index: u32,
    pub(crate) results: Ref<SpeechRecognitionResultList>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechRecognitionEvent {
    event: CppEvent,
    result_index: u32,
    results: Ref<SpeechRecognitionResultList>,
}

impl Wrappable for SpeechRecognitionEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechRecognitionEvent {
    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: SpeechRecognitionEventInit,
    ) -> WebIdlResult<Ref<Self>> {
        let event = CppEvent::create_for_owner(
            cx.realm(),
            &r#type,
            event_init_dict.parent.bubbles,
            event_init_dict.parent.cancelable,
            event_init_dict.parent.composed,
        );
        let object = allocate(cx.vm(), |_| Self {
            event,
            result_index: event_init_dict.result_index,
            results: event_init_dict.results,
        });
        object.event.set_owner(object);
        Ok(object)
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionevent-resultindex
    pub(crate) fn result_index(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.result_index)
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionevent-results
    pub(crate) fn results(&self, _: &mut CallContext) -> WebIdlResult<Ref<SpeechRecognitionResultList>> {
        Ok(self.results)
    }
}
