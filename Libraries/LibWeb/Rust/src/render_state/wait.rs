/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's one way to wait for a document's render state, and the rights it spends to do so.
//!
//! The host waits only with a [`ScriptForcedRead`], which the host entries script APIs call mint for a current answer,
//! or a [`LockstepProof`], whose reasons are the waits internal code makes. Each right is minted only by the module
//! that owns its marker, so code elsewhere has no way to wait.

use super::{DocumentHost, RenderMessage, send};
use std::marker::PhantomData;

/// The one wait of a script API call that needs a current answer: getComputedStyle, an element's geometry, hit
/// testing, innerText and the like. The host entry the API calls mints it, and a wait takes it by value, so it is
/// neither `Clone` nor `Copy`, and it stays on the document's thread.
pub(crate) struct ScriptForcedRead {
    not_send_or_sync: PhantomData<*const ()>,
}

/// The right of the host to wait for a document's render state outside a script's forced read, for one of the reasons
/// that have a [`LockstepReason`] marker. It stays on the document's thread.
pub(crate) struct LockstepProof {
    not_send_or_sync: PhantomData<*const ()>,
}

mod private {
    pub trait ScriptEntry {}
    pub trait LockstepReason {}
    pub trait RenderWait {}
}

/// A marker that only a host entry script APIs call can construct, which mints a [`ScriptForcedRead`].
pub(crate) trait ScriptEntry: private::ScriptEntry {}

/// A marker that only the module waiting for its reason can construct, which mints a [`LockstepProof`].
pub(crate) trait LockstepReason: private::LockstepReason {}

/// What a wait for a document's render state spends: a script's forced read, or a [`LockstepProof`].
pub(crate) trait RenderWait: private::RenderWait {}

impl private::RenderWait for ScriptForcedRead {}
impl RenderWait for ScriptForcedRead {}
impl private::RenderWait for LockstepProof {}
impl RenderWait for LockstepProof {}

impl ScriptForcedRead {
    /// A forced read for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The forced read of the script API call that entered the host through the entry `_` marks.
    pub(crate) fn at_script_entry(_: &impl ScriptEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

impl LockstepProof {
    /// The right to wait for the reason `_` marks.
    pub(crate) fn for_reason(_: &impl LockstepReason) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

macro_rules! script_entry {
    ($marker:path) => {
        impl private::ScriptEntry for $marker {}
        impl ScriptEntry for $marker {}
    };
}

// The host entries script APIs call, which may each spend one forced read.
script_entry!(super::devtools::DevtoolsEntry);
script_entry!(crate::layout::script_entries::ScriptEntry);

macro_rules! lockstep_reason {
    ($marker:path) => {
        impl private::LockstepReason for $marker {}
        impl LockstepReason for $marker {}
    };
}

// The waits internal code makes, each minted only by the module its marker belongs to.
lockstep_reason!(crate::layout::layout_changes::HostPaysTheWrite);
lockstep_reason!(crate::layout::shell_reads::HostReadsItsOwnWrite);
lockstep_reason!(crate::painting::ffi::InputReadsBoxes);
lockstep_reason!(crate::painting::ffi::ScrollSnaps);
lockstep_reason!(crate::painting::paint_passes::HostPaintStep);
lockstep_reason!(crate::layout::text_queries::InputSelectsByWord);
lockstep_reason!(crate::css::style::engine_calls::EngineDoor);
lockstep_reason!(crate::layout::LayoutUpdate);
lockstep_reason!(super::document_host::NewDocument);

/// Where the render side answers a host that waits for it: the slot in the waiting host's frame that the answer moves
/// into, which the message borrows for as long as the host waits. Only an answer goes through it: a slot left empty
/// is a render state that died.
pub(crate) struct ReplyTo<'a, R>(&'a mut Option<R>);

impl<R> ReplyTo<'_, R> {
    /// Answers with what `job` answers. A panic in `job` leaves the slot empty and goes on to end the message.
    pub(crate) fn answer(self, job: impl FnOnce() -> R) {
        *self.0 = Some(job());
    }
}

/// Sends the render state of `host`'s document the message `message` makes of where it answers, and waits for the
/// answer, spending `_wait`. The host is only ever shared, so what an outer call holds of it stays live across the wait.
pub(crate) fn wait_for_render_state<R>(
    _wait: impl RenderWait,
    _host: &DocumentHost,
    message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>,
) -> R {
    send_and_wait(message)
}

/// Sends the render state of the document `main_thread` was minted for the message `message` makes of where it
/// answers, and waits for the answer, spending `_wait`. An entry that runs a step of the host sends its jobs this way:
/// it holds the host only through its token, so it borrows nothing of what a wait could change.
pub(crate) fn wait_from_entry<R>(
    _wait: impl RenderWait,
    _main_thread: &crate::stage::MainThread,
    message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>,
) -> R {
    send_and_wait(message)
}

fn send_and_wait<R>(message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>) -> R {
    let mut answered = None;
    send(message(ReplyTo(&mut answered)));
    answered.unwrap_or_else(|| render_state_died())
}

/// Ends the process, on a host whose wait for a render state found no answer: the message panicked, and may have left
/// the document's render state half changed, so nothing can go on over it.
#[cold]
pub(crate) fn render_state_died() -> ! {
    std::process::abort()
}

#[cfg(test)]
mod tests {
    use super::*;

    trait AmbiguousIfSend<A> {
        fn marker() {}
    }

    impl<T: ?Sized> AmbiguousIfSend<()> for T {}
    impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

    // These fail to compile, as the calls are ambiguous, if a right to wait ever becomes Send.
    #[test]
    fn a_script_forced_read_is_not_send() {
        <ScriptForcedRead as AmbiguousIfSend<_>>::marker();
    }

    #[test]
    fn a_lockstep_proof_is_not_send() {
        <LockstepProof as AmbiguousIfSend<_>>::marker();
    }

    #[test]
    fn a_job_answers_through_its_reply() {
        let mut answered = None;
        ReplyTo(&mut answered).answer(|| 7);
        assert_eq!(answered, Some(7));
    }

    #[test]
    fn a_panic_in_a_job_for_a_waiting_caller_leaves_its_reply_empty() {
        let mut answered = None::<u32>;
        let reply = ReplyTo(&mut answered);
        let panicked = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            reply.answer(|| panic!("the job panicked"));
        }));
        assert!(panicked.is_err());
        assert_eq!(answered, None);
    }
}
