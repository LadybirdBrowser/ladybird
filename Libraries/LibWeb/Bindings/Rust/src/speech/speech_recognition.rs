/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechRecognition, https://wicg.github.io/speech-api/#speechrecognition

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::Trace;

use super::speech_grammar_list::SpeechGrammarList;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechRecognition {
    event_target: CppEventTarget,
    grammars: Cell<Ref<SpeechGrammarList>>,
    #[gc(untraced)]
    lang: RefCell<Utf16String>,
    continuous: Cell<bool>,
    interim_results: Cell<bool>,
    max_alternatives: Cell<u32>,
}

impl Wrappable for SpeechRecognition {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for SpeechRecognition {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechRecognition {
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Ref<Self>> {
        let grammars = allocate(cx.vm(), SpeechGrammarList::new);
        let recognition = allocate(cx.vm(), |_| Self {
            event_target: CppEventTarget::new(),
            grammars: Cell::new(grammars),
            lang: RefCell::new(Utf16String::default()),
            continuous: Cell::new(false),
            interim_results: Cell::new(false),
            max_alternatives: Cell::new(1),
        });
        recognition.event_target.set_owner(recognition);
        Ok(recognition)
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-grammars
    pub(crate) fn grammars(&self, _: &mut CallContext) -> WebIdlResult<Ref<SpeechGrammarList>> {
        Ok(self.grammars.get())
    }

    pub(crate) fn set_grammars(&self, _: &mut CallContext, grammars: Ref<SpeechGrammarList>) -> WebIdlResult<()> {
        self.grammars.set(grammars);
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-lang
    pub(crate) fn lang(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.lang.borrow().clone())
    }

    pub(crate) fn set_lang(&self, _: &mut CallContext, lang: Utf16String) -> WebIdlResult<()> {
        *self.lang.borrow_mut() = lang;
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-continuous
    pub(crate) fn continuous(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.continuous.get())
    }

    pub(crate) fn set_continuous(&self, _: &mut CallContext, continuous: bool) -> WebIdlResult<()> {
        self.continuous.set(continuous);
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-interimresults
    pub(crate) fn interim_results(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.interim_results.get())
    }

    pub(crate) fn set_interim_results(&self, _: &mut CallContext, interim_results: bool) -> WebIdlResult<()> {
        self.interim_results.set(interim_results);
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechrecognition-maxalternatives
    pub(crate) fn max_alternatives(&self, _: &mut CallContext) -> WebIdlResult<u32> {
        Ok(self.max_alternatives.get())
    }

    pub(crate) fn set_max_alternatives(&self, _: &mut CallContext, max_alternatives: u32) -> WebIdlResult<()> {
        self.max_alternatives.set(max_alternatives);
        Ok(())
    }
}
