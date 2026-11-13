/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechRecognitionResultList, https://wicg.github.io/speech-api/#speechrecognitionresultlist

use libjs_rust::api::Trace;

use super::speech_recognition_result::SpeechRecognitionResult;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechRecognitionResultList {
    results: Vec<Ref<SpeechRecognitionResult>>,
}

impl Wrappable for SpeechRecognitionResultList {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechRecognitionResultList {
    // https://wicg.github.io/speech-api/#dom-speechrecognitionresultlist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.results.len()).expect("a list has fewer than 2^32 results"))
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionresultlist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<SpeechRecognitionResult>>> {
        Ok(self.results.get(index as usize).copied())
    }
}
