/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechGrammar, https://wicg.github.io/speech-api/#speechgrammar

use core::cell::{Cell, RefCell};

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechGrammar {
    #[gc(untraced)]
    src: RefCell<Utf16String>,
    weight: Cell<f32>,
}

impl Wrappable for SpeechGrammar {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechGrammar {
    pub(crate) fn construct(_: &mut CallContext) -> WebIdlResult<Self> {
        Ok(Self {
            src: RefCell::new(Utf16String::default()),
            weight: Cell::new(1.0),
        })
    }

    // https://wicg.github.io/speech-api/#dom-speechgrammar-src
    pub(crate) fn src(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.src.borrow().clone())
    }

    pub(crate) fn set_src(&self, _: &mut CallContext, src: Utf16String) -> WebIdlResult<()> {
        *self.src.borrow_mut() = src;
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechgrammar-weight
    pub(crate) fn weight(&self, _: &mut CallContext) -> WebIdlResult<f32> {
        Ok(self.weight.get())
    }

    pub(crate) fn set_weight(&self, _: &mut CallContext, weight: f32) -> WebIdlResult<()> {
        self.weight.set(weight);
        Ok(())
    }
}
