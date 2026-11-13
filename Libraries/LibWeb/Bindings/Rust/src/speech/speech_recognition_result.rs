/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechRecognitionResult, https://wicg.github.io/speech-api/#speechrecognitionresult

use libjs_rust::api::Trace;

use super::speech_recognition_alternative::SpeechRecognitionAlternative;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechRecognitionResult {
    alternatives: Vec<Ref<SpeechRecognitionAlternative>>,
    is_final: bool,
}

impl Wrappable for SpeechRecognitionResult {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechRecognitionResult {
    // https://wicg.github.io/speech-api/#dom-speechrecognitionresult-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.alternatives.len()).expect("a result has fewer than 2^32 alternatives"))
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresult-item
    pub(crate) fn item(
        &self,
        _: &mut CallContext,
        index: u32,
    ) -> WebIdlResult<Option<Ref<SpeechRecognitionAlternative>>> {
        Ok(self.alternatives.get(index as usize).copied())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresult-isfinal
    pub(crate) fn is_final(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.is_final)
    }
}
