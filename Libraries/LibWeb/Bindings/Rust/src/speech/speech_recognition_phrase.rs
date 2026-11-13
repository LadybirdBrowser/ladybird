/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechRecognitionPhrase, https://wicg.github.io/speech-api/#speechrecognitionphrase

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechRecognitionPhrase {
    phrase: Utf16String,
    boost: f32,
}

impl Wrappable for SpeechRecognitionPhrase {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechRecognitionPhrase {
    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-speechrecognitionphrase
    pub(crate) fn construct(_: &mut CallContext, phrase: Utf16String, boost: f32) -> WebIdlResult<Self> {
        // 1. If boost is smaller than 0.0 or greater than 10.0, throw a SyntaxError and abort these steps.
        if !(0.0..=10.0).contains(&boost) {
            return Err(Exception::Dom(
                "SyntaxError",
                Utf16String::from_ascii(b"SpeechRecognitionPhrase boost must be between 0 and 10"),
            ));
        }

        // 2. Let phr be a new object of type SpeechRecognitionPhrase.
        // 3. Set phr.[[phrase]] to be the value of phrase.
        // 4. Set phr.[[boost]] to be the value of boost.
        // 5. Return phr.
        Ok(Self { phrase, boost })
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-phrase
    pub(crate) fn phrase(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.phrase.clone())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognitionphrase-boost
    pub(crate) fn boost(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.boost)
    }
}
