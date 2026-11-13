/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechSynthesisUtterance, https://wicg.github.io/speech-api/#speechsynthesisutterance

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use super::speech_synthesis_voice::SpeechSynthesisVoice;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechSynthesisUtterance {
    event_target: CppEventTarget,
    #[gc(untraced)]
    text: RefCell<Utf16String>,
    #[gc(untraced)]
    lang: RefCell<Utf16String>,
    voice: Cell<Option<Ref<SpeechSynthesisVoice>>>,
    volume: Cell<f32>,
    rate: Cell<f32>,
    pitch: Cell<f32>,
}

impl Wrappable for SpeechSynthesisUtterance {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for SpeechSynthesisUtterance {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechSynthesisUtterance {
    pub(crate) fn construct(cx: &mut CallContext, text: Utf16String) -> WebIdlResult<Ref<Self>> {
        let utterance = allocate(cx.vm(), |_| Self {
            event_target: CppEventTarget::new(),
            text: RefCell::new(text),
            lang: RefCell::new(Utf16String::default()),
            voice: Cell::new(None),
            volume: Cell::new(1.0),
            rate: Cell::new(1.0),
            pitch: Cell::new(1.0),
        });
        utterance.event_target.set_owner(utterance);
        Ok(utterance)
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-text
    pub(crate) fn text(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.text.borrow().clone())
    }

    pub(crate) fn set_text(&self, _: &mut CallContext, text: Utf16String) -> WebIdlResult<()> {
        *self.text.borrow_mut() = text;
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-lang
    pub(crate) fn lang(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.lang.borrow().clone())
    }

    pub(crate) fn set_lang(&self, _: &mut CallContext, lang: Utf16String) -> WebIdlResult<()> {
        *self.lang.borrow_mut() = lang;
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-voice
    pub(crate) fn voice(&self, _: &mut CallContext) -> WebIdlResult<Option<Ref<SpeechSynthesisVoice>>> {
        Ok(self.voice.get())
    }

    pub(crate) fn set_voice(&self, _: &mut CallContext, voice: Option<Ref<SpeechSynthesisVoice>>) -> WebIdlResult<()> {
        self.voice.set(voice);
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-volume
    pub(crate) fn volume(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.volume.get())
    }

    pub(crate) fn set_volume(&self, _: &mut CallContext, volume: f32) -> WebIdlResult<()> {
        self.volume.set(volume);
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-rate
    pub(crate) fn rate(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.rate.get())
    }

    pub(crate) fn set_rate(&self, _: &mut CallContext, rate: f32) -> WebIdlResult<()> {
        self.rate.set(rate);
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisutterance-pitch
    pub(crate) fn pitch(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.pitch.get())
    }

    pub(crate) fn set_pitch(&self, _: &mut CallContext, pitch: f32) -> WebIdlResult<()> {
        self.pitch.set(pitch);
        Ok(())
    }
}
