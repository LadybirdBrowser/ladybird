/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! SpeechSynthesis, https://wicg.github.io/speech-api/#speechsynthesis

use core::ffi::c_void;
use core::ptr::NonNull;

use libjs_rust::api::{MarkedVec, Trace, Vm};

use super::speech_synthesis_voice::SpeechSynthesisVoice;
use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::{CallContext, WebIdlResult};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct SpeechSynthesis {
    event_target: CppEventTarget,
    pending: bool,
    speaking: bool,
    paused: bool,
    voices: Vec<Ref<SpeechSynthesisVoice>>,
}

impl Wrappable for SpeechSynthesis {
    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for SpeechSynthesis {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl SpeechSynthesis {
    fn create(vm: &Vm) -> Ref<Self> {
        let synthesis = allocate(vm, |_| Self {
            event_target: CppEventTarget::new(),
            pending: false,
            speaking: false,
            paused: false,
            voices: Vec::new(),
        });
        synthesis.event_target.set_owner(synthesis);
        synthesis
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-pending
    pub(crate) fn pending(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.pending)
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-speaking
    pub(crate) fn speaking(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.speaking)
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-paused
    pub(crate) fn paused(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        Ok(self.paused)
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-cancel
    pub(crate) fn cancel(&self, _: &mut CallContext) -> WebIdlResult<()> {
        eprintln!("FIXME: Implement SpeechSynthesis::cancel()");
        Ok(())
    }

    // https://wicg.github.io/speech-api/#dom-speechsynthesis-getvoices
    pub(crate) fn get_voices<'vm>(
        &self,
        cx: &mut CallContext<'vm>,
    ) -> WebIdlResult<MarkedVec<'vm, Ref<SpeechSynthesisVoice>>> {
        // FIXME: Populate voices with available synthesis voices.
        let voices = MarkedVec::new(cx.vm());
        voices.extend_from_slice(&self.voices);
        Ok(voices)
    }
}

/// # Safety
///
/// The VM must be live.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_rust_speech_synthesis_create(vm: *const c_void) -> *mut c_void {
    // SAFETY: The caller passes a live VM.
    let vm = unsafe { &*vm.cast::<Vm>() };
    cell_into_abi(SpeechSynthesis::create(vm))
}
