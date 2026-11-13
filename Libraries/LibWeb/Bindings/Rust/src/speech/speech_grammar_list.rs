/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechGrammarList, https://wicg.github.io/speech-api/#speechgrammarlist

use libjs_rust::api::{DeferGc, Trace};

use super::speech_grammar::SpeechGrammar;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechGrammarList {
    grammars: Vec<Ref<SpeechGrammar>>,
}

impl Wrappable for SpeechGrammarList {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechGrammarList {
    pub(crate) fn new(_: &DeferGc<'_>) -> Self {
        Self { grammars: Vec::new() }
    }

    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        Ok(allocate(cx.vm(), Self::new))
    }

    // https://wicg.github.io/speech-api/#dom-speechgrammarlist-length
    pub(crate) fn length(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(u32::try_from(self.grammars.len()).expect("a list has fewer than 2^32 grammars"))
    }

    // https://wicg.github.io/speech-api/#dom-speechgrammarlist-item
    pub(crate) fn item(&self, _: &mut CallContext, index: u32) -> WebIdlResult<Option<Ref<SpeechGrammar>>> {
        Ok(self.grammars.get(index as usize).copied())
    }
}
