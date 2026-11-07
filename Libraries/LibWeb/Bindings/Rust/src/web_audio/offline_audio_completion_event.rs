/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use super::audio_buffer::AudioBuffer;
use crate::bindings::event_target::CppEvent;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use crate::dom::event::EventInit;

pub(crate) struct OfflineAudioCompletionEventInit {
    pub(crate) parent: EventInit,
    pub(crate) rendered_buffer: Ref<AudioBuffer>,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct OfflineAudioCompletionEvent {
    pub(crate) event: CppEvent,
    rendered_buffer: Option<Ref<AudioBuffer>>,
}

impl Wrappable for OfflineAudioCompletionEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl OfflineAudioCompletionEvent {
    pub(crate) fn create(vm: &Vm, event: CppEvent, rendered_buffer: Ref<AudioBuffer>) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            event,
            rendered_buffer: Some(rendered_buffer),
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: OfflineAudioCompletionEventInit,
    ) -> WebIdlResult<Ref<Self>> {
        let event = CppEvent::create_for_owner(
            cx.realm(),
            &r#type,
            event_init_dict.parent.bubbles,
            event_init_dict.parent.cancelable,
            event_init_dict.parent.composed,
        );
        Ok(Self::create(cx.vm(), event, event_init_dict.rendered_buffer))
    }

    pub(crate) fn rendered_buffer(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioBuffer>> {
        Ok(self
            .rendered_buffer
            .expect("the event initializer supplies its required buffer"))
    }
}
