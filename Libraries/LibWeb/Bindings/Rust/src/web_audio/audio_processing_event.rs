/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
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

pub(crate) struct AudioProcessingEventInit {
    pub(crate) parent: EventInit,
    pub(crate) input_buffer: Ref<AudioBuffer>,
    pub(crate) output_buffer: Ref<AudioBuffer>,
    pub(crate) playback_time: f64,
}

// https://webaudio.github.io/web-audio-api/#AudioProcessingEvent
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct AudioProcessingEvent {
    pub(crate) event: CppEvent,
    playback_time: f64,
    input_buffer: Ref<AudioBuffer>,
    output_buffer: Ref<AudioBuffer>,
}

impl Wrappable for AudioProcessingEvent {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event.as_cpp_parent()
    }
}

impl AudioProcessingEvent {
    pub(crate) fn create(
        vm: &Vm,
        event: CppEvent,
        playback_time: f64,
        input_buffer: Ref<AudioBuffer>,
        output_buffer: Ref<AudioBuffer>,
    ) -> Ref<Self> {
        let object = allocate(vm, |_| Self {
            event,
            playback_time,
            input_buffer,
            output_buffer,
        });
        object.event.set_owner(object);
        object
    }

    pub(crate) fn construct(
        cx: &mut CallContext,
        r#type: Utf16String,
        event_init_dict: AudioProcessingEventInit,
    ) -> WebIdlResult<Ref<Self>> {
        let event = CppEvent::create_for_owner(
            cx.realm(),
            &r#type,
            event_init_dict.parent.bubbles,
            event_init_dict.parent.cancelable,
            event_init_dict.parent.composed,
        );
        Ok(Self::create(
            cx.vm(),
            event,
            event_init_dict.playback_time,
            event_init_dict.input_buffer,
            event_init_dict.output_buffer,
        ))
    }

    pub(crate) fn playback_time(&self, _: &mut CallContext) -> WebIdlResult<f64> {
        Ok(self.playback_time)
    }

    pub(crate) fn input_buffer(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioBuffer>> {
        Ok(self.input_buffer)
    }

    pub(crate) fn output_buffer(&self, _: &mut CallContext) -> WebIdlResult<Ref<AudioBuffer>> {
        Ok(self.output_buffer)
    }
}
