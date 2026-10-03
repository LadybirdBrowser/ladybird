/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's one way to wait for a document's render state, and the rights it spends to do so.
//!
//! The host waits only with a [`ScriptForcedRead`], which the host entries script APIs call mint for a current answer,
//! or a [`LockstepProof`], whose reasons are the waits internal code makes. Each right is minted only by the module
//! that owns its marker, so code elsewhere has no way to wait. What runs beside the host is taken in without a wait
//! only at a [`TaskBoundary`], which the entries the event loop calls between two tasks mint.
//!
//! A style or layout job is sent only by [`force_read`], the first job of a read the host waits for, or by
//! [`run_job`] with a permit that says why the job is another one, as only this module makes the [`SpentWait`] the
//! job's message carries.

use super::{DocumentHost, DocumentId, RenderMessage, send};
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

/// The top of the host's event loop, between two tasks, where the host takes in what has finished beside it and waits
/// for nothing. It stays on the host's thread.
pub(crate) struct TaskBoundary {
    not_send_or_sync: PhantomData<*const ()>,
}

mod private {
    pub trait ScriptEntry {}
    pub trait LockstepReason {}
    pub trait EventLoopEntry {}
    pub trait RenderWait {}
}

/// A marker that only a host entry script APIs call can construct, which mints a [`ScriptForcedRead`].
pub(crate) trait ScriptEntry: private::ScriptEntry {}

/// A marker that only the module waiting for its reason can construct, which mints a [`LockstepProof`].
pub(crate) trait LockstepReason: private::LockstepReason {}

/// A marker that only a host entry the event loop calls between two tasks can construct, which mints a
/// [`TaskBoundary`].
pub(crate) trait EventLoopEntry: private::EventLoopEntry {}

/// What a wait for a document's render state spends: a script's forced read, or a [`LockstepProof`].
pub(crate) trait RenderWait: private::RenderWait {}

impl private::RenderWait for ScriptForcedRead {}
impl RenderWait for ScriptForcedRead {}
impl private::RenderWait for LockstepProof {}
impl RenderWait for LockstepProof {}
impl private::RenderWait for StyleJobPermit {}
impl RenderWait for StyleJobPermit {}
impl private::RenderWait for FrameJobPermit {}
impl RenderWait for FrameJobPermit {}

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

impl TaskBoundary {
    /// A task boundary for a unit test.
    #[cfg(test)]
    pub(crate) fn for_test() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The task boundary at which the event loop calls the entry `_` marks.
    pub(crate) fn at_event_loop_entry(_: &impl EventLoopEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// What a read of a document's render state the host waits for spends on its first style or layout job, through
/// [`force_read`]: the read of the script API call it is made for, or the host's own (an event's dispatch, a child
/// document's style update, an inspection, a rendering update). The scope that brackets the read begins it (see
/// [`DocumentHost::begin_forced_read`]), and the read's first job takes it.
pub(crate) enum ForcedRead {
    Script(ScriptForcedRead),
    Host(LockstepProof),
    /// The read's first job was its style transaction, which spent the read: the read's first layout round is its
    /// second wait, which the type shows.
    AfterStyle(StyledFirst),
}

/// What a forced read whose first job was its style transaction leaves its first layout round. Only [`force_read`]
/// makes one.
pub(crate) struct StyledFirst {
    not_send_or_sync: PhantomData<*const ()>,
}

/// The right to send a document's render state a style transaction that is not a forced read's first job, and wait for
/// it. Only the constructor below mints one.
pub(crate) struct StyleJobPermit {
    not_send_or_sync: PhantomData<*const ()>,
}

impl StyleJobPermit {
    /// A style transaction no read waits for, as a rendering update's, or a style wave after a read's first.
    pub(crate) fn of_style_update() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// The right to send a document's render state a layout round that is not a forced read's first job, and wait for it.
/// Only the constructors below mint one, so every extra round of a read says why it is one.
pub(crate) struct FrameJobPermit {
    not_send_or_sync: PhantomData<*const ()>,
}

impl FrameJobPermit {
    /// A round after one that left the update another round: it built a tree to lay out after the host was paid for
    /// it, or style or layout work came back from what the host did after it.
    pub(crate) fn for_next_round() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The first round of a layout update in a read whose first job was spent already: another pass for an image that
    /// arrived or a scroll-state snapshot, or a second update the read's call runs.
    pub(crate) fn read_lays_out_again() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// What the message of a style or layout job carries to show that [`force_read`] or [`run_job`] sent it. Only this
/// module makes one, so nothing else sends such a job.
pub(crate) struct SpentWait(());

/// A style or layout job of a document's render state, which only [`force_read`] and [`run_job`] send.
pub(crate) trait RenderJob {
    /// What the render state answers the job with.
    type Answer;

    /// The permit that sends the job where it is not a forced read's first.
    type Permit: RenderWait;

    /// Whether the job is a style transaction, which leaves a forced read's layout to a job of its own.
    const IS_STYLE: bool;

    /// The message that sends the job for `document`, which answers through `reply`.
    fn message(self, document: DocumentId, reply: ReplyTo<'_, Self::Answer>, spent: SpentWait) -> RenderMessage<'_>;
}

/// The first job of a read of `host`'s document's render state the host waits for: spends `read` on `job`, and waits
/// for its answer. A read whose first job is its style transaction leaves its first layout round a [`StyledFirst`].
pub(crate) fn force_read<J: RenderJob>(_read: ForcedRead, host: &DocumentHost, job: J) -> J::Answer {
    let answer = send_job(host, job);
    if J::IS_STYLE {
        host.leave_forced_read(ForcedRead::AfterStyle(StyledFirst {
            not_send_or_sync: PhantomData,
        }));
    }
    answer
}

/// Runs `job`, a style or layout job of `host`'s document that is not a forced read's first, spending `_permit`, and
/// waits for its answer.
pub(crate) fn run_job<J: RenderJob>(_permit: J::Permit, host: &DocumentHost, job: J) -> J::Answer {
    send_job(host, job)
}

fn send_job<J: RenderJob>(host: &DocumentHost, job: J) -> J::Answer {
    let document = host.document();
    send_and_wait(host, |reply| job.message(document, reply, SpentWait(())))
}

macro_rules! script_entry {
    ($marker:path) => {
        impl private::ScriptEntry for $marker {}
        impl ScriptEntry for $marker {}
    };
}

// The host entries script APIs call, which may each spend one forced read.
script_entry!(super::devtools::DevtoolsEntry);
script_entry!(super::document_host::ForcedReadScope);
script_entry!(crate::layout::script_entries::ScriptEntry);

macro_rules! lockstep_reason {
    ($marker:path) => {
        impl private::LockstepReason for $marker {}
        impl LockstepReason for $marker {}
    };
}

// The waits internal code makes, each minted only by the module its marker belongs to.
lockstep_reason!(crate::painting::recording_slot::RecordingNeedsItsRecorder);
lockstep_reason!(crate::layout::layout_changes::HostPaysTheWrite);
lockstep_reason!(crate::layout::shell_reads::HostReadsItsOwnWrite);
lockstep_reason!(crate::layout::LayoutUpdateReads);
lockstep_reason!(crate::layout::tree_update_marks::HostMarksLayoutTree);
lockstep_reason!(crate::painting::ffi::HostReadsPaintState);
lockstep_reason!(crate::painting::ffi::InputReadsBoxes);
lockstep_reason!(crate::painting::ffi::ScrollSnaps);
lockstep_reason!(crate::painting::paint_passes::HostPaintStep);
lockstep_reason!(crate::layout::text_queries::InputSelectsByWord);
lockstep_reason!(crate::css::style::engine_calls::EngineDoor);
lockstep_reason!(super::document_host::HostReadsLayout);
lockstep_reason!(super::document_host::NewDocument);

// The host entries the event loop calls between two tasks.
impl private::EventLoopEntry for crate::painting::ffi::TakesFinishedRecordingIn {}
impl EventLoopEntry for crate::painting::ffi::TakesFinishedRecordingIn {}

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
    host: &DocumentHost,
    message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>,
) -> R {
    send_and_wait(host, message)
}

fn send_and_wait<R>(host: &DocumentHost, message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>) -> R {
    let mut answered = None;
    send(host, message(ReplyTo(&mut answered)));
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
    fn a_task_boundary_is_not_send() {
        <TaskBoundary as AmbiguousIfSend<_>>::marker();
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
