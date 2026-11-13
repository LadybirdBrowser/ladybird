/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechSynthesisVoice, https://wicg.github.io/speech-api/#speechsynthesisvoice

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechSynthesisVoice {
    voice_uri: Utf16String,
    name: Utf16String,
    lang: Utf16String,
    local_service: bool,
    default: bool,
}

impl Wrappable for SpeechSynthesisVoice {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechSynthesisVoice {
    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-voiceuri
    pub(crate) fn voice_uri(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.voice_uri.clone())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-name
    pub(crate) fn name(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.name.clone())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-lang
    pub(crate) fn lang(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.lang.clone())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-localservice
    pub(crate) fn local_service(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.local_service)
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesisvoice-default
    pub(crate) fn is_default(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.default)
    }
}
