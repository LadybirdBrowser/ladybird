/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The host's one way to wait for a document's render state, and the rights it spends to do so.
//!
//! The host waits only with a [`ScriptForcedRead`], which the host entries script APIs call mint for a current answer,
//! or in a [`BegunRead`], a read the host began, which a host entry that reaches the render state where the host is
//! takes from its caller: code that has not begun a read has nothing to reach the state with. What runs beside the host
//! is taken in without a wait only at a [`TaskBoundary`], which the entries the event loop calls between two tasks
//! mint.
//!
//! A style or layout job is sent only by [`force_read`], the first job of a read the host waits for, or by
//! [`run_job`] with a permit that says why the job is another one, as only this module makes the [`SpentWait`] the
//! job's message carries.

use super::{DocumentHost, RenderMessage, send};
use std::marker::PhantomData;

/// The one wait of a script API call that needs a current answer: getComputedStyle, an element's geometry, hit
/// testing, innerText and the like. The host entry the API calls mints it, and a wait takes it by value, so it is
/// neither `Clone` nor `Copy`, and it stays on the document's thread.
pub(crate) struct ScriptForcedRead {
    not_send_or_sync: PhantomData<*const ()>,
}

/// The right of the host to wait for a recording in flight, for one of the reasons that have a [`LockstepReason`]
/// marker. It stays on the document's thread.
pub(crate) struct LockstepProof {
    not_send_or_sync: PhantomData<*const ()>,
}

/// A read of a document's render state that the host began and has not ended. A host entry that reaches the render
/// state where the host is takes it from its caller, as only a read takes a frame in flight in: the host's scope of a
/// read lends it to the entries it calls (see [`super::document_host::document_host_read_scope_view`]), and nothing
/// else makes one, so code that reaches the state without a begun read does not compile. It stays on the document's
/// thread.
pub struct BegunRead {
    not_send_or_sync: PhantomData<*const ()>,
}

impl BegunRead {
    /// The begun read a host keeps, which its scopes of a read lend.
    pub(super) fn of_host() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// What takes a frame in flight in for a reach of a document's render state: a forced read of the reach's own, or the
/// read the host began, which the reach spends only where the frame flies, or the proof that none flies. Only this
/// module makes one.
pub(crate) enum ReadRight {
    Forced(ForcedRead),
    Begun(SpendsBegunRead),
    /// No frame flies to take in.
    Here(super::NoFrameInFlight),
}

/// What a [`ReadRight`] holds that spends the read the host began.
pub(crate) struct SpendsBegunRead(());

/// What the reads of a layout node the host holds spend: the read that reached the node. A layout node is reached only
/// through an entry that takes a [`BegunRead`], as the ones that find the node an element or a row is bound to do, and
/// that entry took the frame in, which flies again only from a rendering update, after the task that reached the node.
/// Only the modules whose entries answer a node's reads of its row mint one, each with a [`HeldNodeEntry`] marker.
pub(crate) struct NodeRead {
    not_send_or_sync: PhantomData<*const ()>,
}

impl NodeRead {
    /// The read of a layout node the host holds, which the entry `_` marks reads.
    pub(crate) fn of_held_node(_: &impl HeldNodeEntry) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
}

/// The top of the host's event loop, between two tasks, where the host takes in what has finished beside it and waits
/// for nothing. It stays on the host's thread.
pub(crate) struct TaskBoundary {
    not_send_or_sync: PhantomData<*const ()>,
}

/// The start of a task of the host's event loop, where a clock lease begins: only there, so a lease never begins inside
/// a task. It stays on the host's thread.
pub(crate) struct TaskStart {
    not_send_or_sync: PhantomData<*const ()>,
}

mod private {
    pub trait ScriptEntry {}
    pub trait LockstepReason {}
    pub trait EventLoopEntry {}
    pub trait RenderWait {}
    pub trait HeldNodeEntry {}
}

/// A marker that only a host entry script APIs call can construct, which mints a [`ScriptForcedRead`].
pub(crate) trait ScriptEntry: private::ScriptEntry {}

/// A marker that only a module whose entries read the row of a layout node the host holds can construct, which mints a
/// [`NodeRead`].
pub(crate) trait HeldNodeEntry: private::HeldNodeEntry {}

/// A marker that only the module waiting for its reason can construct, which mints a [`LockstepProof`] to join a
/// recording in flight.
pub(crate) trait LockstepReason: private::LockstepReason {}

/// A marker that only a host entry the event loop calls between two tasks can construct, which mints a
/// [`TaskBoundary`].
pub(crate) trait EventLoopEntry: private::EventLoopEntry {}

/// What a wait for a document's render state spends: a script's forced read, the read the host began, or a job's
/// permit, which a begun read makes.
pub(crate) trait RenderWait: private::RenderWait {
    /// What takes a frame in flight in for the wait.
    fn into_read_right(self) -> ReadRight;

    /// Whether the wait may reach the render state of `host`'s document: a begun read reaches only the render state of
    /// the document whose host began it.
    fn reaches(&self, _host: &DocumentHost) -> bool {
        true
    }
}

impl private::RenderWait for ScriptForcedRead {}
impl RenderWait for ScriptForcedRead {
    fn into_read_right(self) -> ReadRight {
        ReadRight::Forced(ForcedRead::Script(self))
    }
}

macro_rules! render_wait {
    ($wait:ty) => {
        impl private::RenderWait for $wait {}
        impl RenderWait for $wait {
            fn into_read_right(self) -> ReadRight {
                ReadRight::Begun(SpendsBegunRead(()))
            }
        }
    };
}

impl private::RenderWait for &BegunRead {}
impl RenderWait for &BegunRead {
    fn into_read_right(self) -> ReadRight {
        ReadRight::Begun(SpendsBegunRead(()))
    }

    fn reaches(&self, host: &DocumentHost) -> bool {
        std::ptr::eq(*self, host.begun_read())
    }
}

impl private::RenderWait for super::NoFrameInFlight {}
impl RenderWait for super::NoFrameInFlight {
    fn into_read_right(self) -> ReadRight {
        ReadRight::Here(self)
    }
}
render_wait!(NodeRead);
render_wait!(StyleJobPermit);
render_wait!(FrameJobPermit);

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

impl TaskStart {
    /// The start of the task at which the event loop calls the entry `_` marks.
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
    Host(HostRead),
    /// The read's first job was its style transaction, which spent the read: the read's first layout round is its
    /// second wait, which the type shows.
    AfterStyle(StyledFirst),
}

/// The host's own read of a document's render state, which only a scope the host begins mints.
pub(crate) struct HostRead {
    not_send_or_sync: PhantomData<*const ()>,
}

impl HostRead {
    /// The read of a scope the host began, or of the host's teardown of its document.
    pub(super) fn begun() -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }
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
    /// A style transaction of `_read` that is not its first job, as a style wave after the read's first, or a rendering
    /// update's.
    pub(crate) fn of_style_update(_read: &BegunRead) -> Self {
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
    /// A round of `_read` after one that left the update another round: it built a tree to lay out after the host
    /// was paid for it, or style or layout work came back from what the host did after it.
    pub(crate) fn for_next_round(_read: &BegunRead) -> Self {
        Self {
            not_send_or_sync: PhantomData,
        }
    }

    /// The first round of a layout update in `_read`, whose first job was spent already: another pass for an image
    /// that arrived or a scroll-state snapshot, or a second update the read's call runs.
    pub(crate) fn read_lays_out_again(_read: &BegunRead) -> Self {
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

    /// The message that sends the job, which answers through `reply`.
    fn message(self, reply: ReplyTo<'_, Self::Answer>, spent: SpentWait) -> RenderMessage<'_>;
}

/// The first job of a read of `host`'s document's render state the host waits for: spends `read` on `job`, and waits
/// for its answer. A read whose first job is its style transaction leaves its first layout round a [`StyledFirst`].
pub(crate) fn force_read<J: RenderJob>(read: ForcedRead, host: &DocumentHost, job: J) -> J::Answer {
    let answer = send_job(host, ReadRight::Forced(read), job);
    if J::IS_STYLE {
        host.leave_forced_read(ForcedRead::AfterStyle(StyledFirst {
            not_send_or_sync: PhantomData,
        }));
    }
    answer
}

/// Takes the frame of `host`'s document in, where it flies, spending `read` on the style transaction that flew with it,
/// which the read takes in as its first job: the read's first layout round is its second wait, as after a style
/// transaction it sent. This is the one way a frame in flight is waited for.
pub(crate) fn force_read_flown_style(read: ForcedRead, host: &DocumentHost) {
    host.land(read);
    host.leave_forced_read(ForcedRead::AfterStyle(StyledFirst {
        not_send_or_sync: PhantomData,
    }));
}

/// Runs `job`, a style or layout job of `host`'s document that is not a forced read's first, spending `_permit`, and
/// waits for its answer.
pub(crate) fn run_job<J: RenderJob>(permit: J::Permit, host: &DocumentHost, job: J) -> J::Answer {
    send_job(host, permit.into_read_right(), job)
}

fn send_job<J: RenderJob>(host: &DocumentHost, read: ReadRight, job: J) -> J::Answer {
    send_and_wait(host, read, |reply| job.message(reply, SpentWait(())))
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

// The waits internal code makes for a recording, each minted only by the module its marker belongs to.
lockstep_reason!(crate::painting::recording_slot::RecordingNeedsItsRecorder);

/// Defines `HeldNode`, the marker of a module whose host entries read or write the row of a layout node the host holds,
/// and `node_read()`, the [`NodeRead`] they spend. The module's marker is listed below.
macro_rules! held_node_entries {
    () => {
        /// Marks the host entries of this module, which read or write the row of a layout node the host holds.
        pub(crate) struct HeldNode {
            _private: (),
        }

        /// The read of the layout node the host holds, which this module's entries spend.
        fn node_read() -> $crate::render_state::NodeRead {
            $crate::render_state::NodeRead::of_held_node(&HeldNode { _private: () })
        }
    };
}

pub(crate) use held_node_entries;

macro_rules! held_node_entry {
    ($marker:path) => {
        impl private::HeldNodeEntry for $marker {}
        impl HeldNodeEntry for $marker {}
    };
}

// The modules whose host entries read or write the row of a layout node the host holds.
held_node_entry!(crate::layout::shell_reads::HeldNode);
held_node_entry!(crate::layout::ArenaHeldNode);
held_node_entry!(crate::layout::PartialRelayoutHeldNode);
held_node_entry!(crate::layout::rendered_text::HeldNode);
held_node_entry!(crate::layout::text_queries::HeldNode);
held_node_entry!(crate::painting::ffi::HeldNode);
held_node_entry!(crate::painting::layout_tree_dump::HeldNode);
lockstep_reason!(super::clock::PresenterNeedsItsFrame);
lockstep_reason!(super::clock::AnimationChanged);

// The host entries the event loop calls between two tasks.
impl private::EventLoopEntry for crate::painting::ffi::TakesFinishedRecordingIn {}
impl EventLoopEntry for crate::painting::ffi::TakesFinishedRecordingIn {}
impl private::EventLoopEntry for crate::css::style::style_job::TakesFinishedStyleIn {}
impl EventLoopEntry for crate::css::style::style_job::TakesFinishedStyleIn {}
impl private::EventLoopEntry for super::clock::LeasesClockForTask {}
impl EventLoopEntry for super::clock::LeasesClockForTask {}

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
/// answer, spending `wait`. The host is only ever shared, so what an outer call holds of it stays live across the wait.
pub(crate) fn wait_for_render_state<R>(
    wait: impl RenderWait,
    host: &DocumentHost,
    message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>,
) -> R {
    assert!(
        wait.reaches(host),
        "a begun read reaches only the render state of its own document"
    );
    send_and_wait(host, wait.into_read_right(), message)
}

fn send_and_wait<R>(
    host: &DocumentHost,
    read: ReadRight,
    message: impl FnOnce(ReplyTo<'_, R>) -> RenderMessage<'_>,
) -> R {
    let mut answered = None;
    send(host, read, message(ReplyTo(&mut answered)));
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
    fn a_task_start_is_not_send() {
        <TaskStart as AmbiguousIfSend<_>>::marker();
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
