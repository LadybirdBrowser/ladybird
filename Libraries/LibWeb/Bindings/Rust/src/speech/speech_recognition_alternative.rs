/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechRecognitionAlternative, https://wicg.github.io/speech-api/#speechrecognitionalternative

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechRecognitionAlternative {
    transcript: Utf16String,
    confidence: f32,
}

impl Wrappable for SpeechRecognitionAlternative {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechRecognitionAlternative {
    // https://wicg.github.io/speech-api/#dom-speechrecognitionalternative-transcript
    pub(crate) fn transcript(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.transcript.clone())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionalternative-confidence
    pub(crate) fn confidence(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.confidence)
    }
}
