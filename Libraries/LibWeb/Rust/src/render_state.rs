/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Each document's render state, which its host owns.
//!
//! A document's [`RenderState`] is what its style, layout and paint preparation compute over: its layout arena and
//! what lives beside it. The [`DocumentHost`] owns it in its frame: the state is either here, where the host lends it
//! to a [`RenderMessage`] it [`send`]s to the StyleLayout thread and waits for, or flying, moved into the job of a frame
//! that runs beside the host until the host takes it in again. Code that runs beside a flying frame has no state to
//! reach.

use crate::css::style::StyleEngineHandle;
use crate::css::style::bridge::{FfiDeviceClass, create_document_style_engine};
use crate::layout::ArenaHandle;
use std::cell::{Cell, RefCell};

mod devtools;
mod document_host;
mod questions;
mod wait;

pub use document_host::DocumentHost;
#[cfg(test)]
pub(crate) use document_host::TestHost;
pub(crate) use questions::{
    ArenaAnswer, ArenaQuery, ArenaRead, CommittedRows, EngineCall, Lent, PreparationPending, ask,
};
pub use wait::BegunRead;
pub(crate) use wait::held_node_entries;
pub(crate) use wait::{
    ForcedRead, FrameJobPermit, LockstepProof, NodeRead, ReadRight, RenderJob, RenderWait, ReplyTo, ScriptForcedRead,
    SpentWait, StyleJobPermit, TaskBoundary, force_read, render_state_died, run_job, wait_for_render_state,
};

/// One document's render state.
pub(crate) struct RenderState {
    /// The layout arena. It links the style engine below, which outlives it.
    arena: Box<ArenaHandle>,
    /// The document's style engine, which the state owns through the handle the arena links. Every borrow of the
    /// engine comes from this one pointer, and a borrow of the state borrows it mutably only where it reaches the
    /// engine alone.
    engine: StyleEngineHandle,
}

impl RenderState {
    /// Makes the state of a document whose device is of class `device_class`.
    fn new(device_class: FfiDeviceClass) -> Self {
        let mut arena = Box::new(ArenaHandle::new());
        let engine = StyleEngineHandle::create(create_document_style_engine(device_class));
        arena.arena_mut().set_style_engine(engine);
        Self { arena, engine }
    }

    /// Drops the state, which must hold no layout node any more.
    fn retire(self) {
        let Self { arena, engine } = self;
        assert_eq!(
            arena.arena().live_slot_count(),
            0,
            "layout node arena destroyed with live slots"
        );
        drop(arena);
        // SAFETY: The state made the handle, and the arena that linked it is gone.
        unsafe { engine.destroy() }.end_recording();
    }

    /// Applies `changes`, writes the host queued, in the order the host made them.
    fn apply(&mut self, changes: std::vec::Drain<'_, ArenaChange>) {
        for change in changes {
            // SAFETY: The state is borrowed mutably, and so is the engine its arena links.
            unsafe { change.apply(self.arena.arena_mut(), self.engine) };
        }
    }

    /// Answers `question`, as of what the state holds now.
    fn answer<Q: questions::Question>(&mut self, question: Q) -> Q::Answer {
        // SAFETY: As for a change.
        unsafe { question.answer(self.arena.arena_mut(), self.engine) }
    }

    /// The style engine, to read.
    fn engine_ref(&self) -> &crate::css::style::StyleEngine {
        // SAFETY: The engine lives as long as the state, and is borrowed mutably only through a mutable borrow of it.
        unsafe { self.engine.get() }
    }

    /// The style engine, borrowed for as long as the state is.
    fn engine_mut(&mut self) -> &mut crate::css::style::StyleEngine {
        // SAFETY: As for a change.
        unsafe { self.engine.get_mut() }
    }

    /// Handles `message`, after `changes`, the writes its host queued before it.
    fn handle(&mut self, changes: std::vec::Drain<'_, ArenaChange>, message: RenderMessage<'_>) {
        self.apply(changes);
        match message {
            RenderMessage::Style { job, reply, .. } => reply.answer(|| job.run(self.engine_mut())),
            // The host keeps what the job's inputs name until it has the answer.
            RenderMessage::LayoutRound { job, reply, .. } => reply.answer(|| job.run(&mut self.arena)),
            RenderMessage::Paint { pass, reply } => reply.answer(|| pass.run(self.arena.arena_mut())),
            RenderMessage::PanicForTesting { reply } => reply.answer(|| panic!("the render state panicked for a test")),
        }
    }
}

/// What a frame that flew brings its host back: the render state it took, what its job answered, and the emptied buffer
/// of the writes it took, which the host's queue keeps.
pub(crate) struct Landing {
    state: RenderState,
    style: crate::css::style::style_job::StyleJobAnswer,
    changes: Vec<ArenaChange>,
}

/// A write the host makes to a document's render state, as owned data the state applies in the order the host made
/// it, before anything that reads what it changes.
pub(crate) enum ArenaChange {
    /// A write to the document's layout marks or layout facts.
    Layout(crate::layout::layout_changes::LayoutChange),
    /// A write to the document's paint state.
    Paint(crate::painting::paint_changes::PaintChange),
    /// A write to the document's style engine.
    Style(crate::css::style::bridge::StyleChange),
    /// A hand-written write to the document's style engine.
    Engine(crate::css::style::engine_calls::EngineWrite),
}

impl ArenaChange {
    /// # Safety
    ///
    /// `engine` must name the live style engine `arena` links, which nothing else borrows meanwhile.
    unsafe fn apply(self, arena: &mut crate::layout::LayoutNodeArena, engine: StyleEngineHandle) {
        match self {
            Self::Layout(change) => change.apply(arena),
            Self::Paint(change) => change.apply(arena),
            // SAFETY: Guaranteed by the caller. A style change reaches the engine only through this borrow.
            Self::Style(change) => change.apply(unsafe { engine.get_mut() }),
            // SAFETY: As above.
            Self::Engine(write) => write.apply(unsafe { engine.get_mut() }),
        }
    }

    /// Whether the change writes what the document's style engine computes from. A mint of style node identities does
    /// not: it makes them live, in the order the host minted them.
    fn writes_style(&self) -> bool {
        match self {
            Self::Style(_) => true,
            Self::Engine(write) => !matches!(write, crate::css::style::engine_calls::EngineWrite::MintStyleNodes(_)),
            Self::Layout(_) | Self::Paint(_) => false,
        }
    }
}

/// Proof that the host's document has no frame in flight: the host has taken it in, or let none fly. Only
/// DocumentHost::take_frame_in() and DocumentHost::layout_waits_for_no_frame() mint it.
pub(crate) struct NoFrameInFlight(());

/// The writes a host queued for its document's render state, in the order the host made them, in two buffers the queue
/// keeps: the writes are applied out of one as the host queues more into the other, so queueing a write allocates only
/// where a buffer grows past what it held before.
#[derive(Default)]
struct ChangeQueue {
    queued: RefCell<Vec<ArenaChange>>,
    /// The empty buffer the host queues into while the writes queued before are applied.
    spare: Cell<Vec<ArenaChange>>,
    /// Whether nothing is queued and no frame of the document flies, so the render state reads as of every write the
    /// host made. It is all a question the host answers where it is tests.
    settled: Cell<bool>,
}

impl ChangeQueue {
    fn push(&self, change: ArenaChange) {
        self.queued.borrow_mut().push(change);
        self.settled.set(false);
    }

    fn is_settled(&self) -> bool {
        self.settled.get()
    }

    /// Lends the queued writes to `apply`, which applies them, and keeps their emptied buffer as the spare. A write the
    /// host queues meanwhile, as the writes are applied or the render side works, waits for the next application, as do
    /// the style writes where `hold_style`.
    fn drain<R>(
        &self,
        _landed: NoFrameInFlight,
        hold_style: bool,
        apply: impl FnOnce(std::vec::Drain<'_, ArenaChange>) -> R,
    ) -> R {
        let mut queued = self.queued.replace(self.spare.take());
        if hold_style {
            self.queue_style_writes_of(&mut queued);
        }
        let answer = apply(queued.drain(..));
        self.spare.set(queued);
        self.settled.set(self.queued.borrow().is_empty());
        answer
    }

    /// Moves the style writes of `changes` back into the queue, where they wait for the next application.
    #[cold]
    fn queue_style_writes_of(&self, changes: &mut Vec<ArenaChange>) {
        self.queued
            .borrow_mut()
            .extend(changes.extract_if(.., |change| change.writes_style()));
    }

    /// Takes the queued writes, for a style transaction that flies with them. Their buffer comes back with give_back().
    /// The queue is not settled until the host has taken the transaction in.
    fn take(&self) -> Vec<ArenaChange> {
        self.settled.set(false);
        self.queued.replace(self.spare.take())
    }

    /// Keeps `buffer`, emptied by the render side, as the spare.
    fn give_back(&self, buffer: Vec<ArenaChange>) {
        debug_assert!(buffer.is_empty(), "the render side gives back an emptied buffer");
        self.spare.set(buffer);
    }

    /// Takes the queued style writes out of the queue, in the spare, until requeue() puts them back.
    fn hold_style_writes(&self) -> Vec<ArenaChange> {
        let mut held = self.spare.take();
        held.extend(self.queued.borrow_mut().extract_if(.., |change| change.writes_style()));
        held
    }

    /// Queues the writes hold_style_writes() held behind the writes queued meanwhile.
    fn requeue(&self, mut held: Vec<ArenaChange>) {
        self.queued.borrow_mut().append(&mut held);
        self.spare.set(held);
        self.settled.set(false);
    }
}

/// A message the host sends its document's render state, which it lends the message to while it waits.
#[expect(
    clippy::large_enum_variant,
    reason = "a message is made once, in the frame of the host that waits for it"
)]
pub(crate) enum RenderMessage<'a> {
    /// A style transaction of the document.
    Style {
        job: crate::css::style::style_job::StyleJob,
        reply: ReplyTo<'a, crate::css::style::style_job::StyleJobAnswer>,
        /// What shows that a forced read or a job's permit sent the transaction.
        _spent: SpentWait,
    },
    /// A layout round of the document: its tree build and layout stages.
    LayoutRound {
        job: crate::layout::LayoutRoundJob,
        reply: ReplyTo<'a, crate::layout::LayoutRoundAnswer>,
        /// What shows that a forced read or a job's permit sent the round.
        _spent: SpentWait,
    },
    /// A step of paint preparation.
    Paint {
        pass: crate::painting::paint_passes::PaintPass,
        reply: ReplyTo<'a, crate::painting::paint_passes::PaintPassAnswer>,
    },
    /// Panics answering, for a test that the host waiting for the answer crashes.
    PanicForTesting { reply: ReplyTo<'a, ()> },
}

/// Runs `job` on the render side, the StyleLayout thread, and waits for it, so it may borrow from the calling frame. A
/// job handed from that thread itself, as a child document's while it handles another, runs right there, and a unit
/// test's jobs run on the test's own thread.
fn on_render_side<R: Send>(job: impl FnOnce() -> R + Send) -> R {
    if cfg!(test) {
        return job();
    }
    crate::stage_thread::style_layout_thread().run(job)
}

/// Sends `message` to the render state of `host`'s document, after the writes the host queued, and waits until it is
/// handled. The host lends the state to the message, taking a frame in flight in first with `read`. A message writes the
/// render state, unless it is a paint pass that leaves the paint and hit testing properties current.
pub(crate) fn send(host: &DocumentHost, read: ReadRight, message: RenderMessage<'_>) {
    if !matches!(&message, RenderMessage::Paint { pass, .. } if pass.leaves_paint_preparation_current()) {
        host.note_render_state_write();
    }
    host.drain_queued_changes(read, |changes| {
        host.with_state(|state| on_render_side(move || state.handle(changes, message)));
    });
}

/// Submits `job`, a style transaction of `host`'s document, to the render side, the StyleLayout thread, with the
/// document's render state and the writes the host queued, and goes on: the frame flies with the state until the host
/// takes it in, and the document drains the transaction's reactions with `drain`. Only a transaction that `_license`
/// lets fly is submitted.
pub(crate) fn fly(
    host: &DocumentHost,
    job: crate::css::style::style_job::StyleJob,
    drain: crate::css::style::style_job::FfiFlownStyleDrain,
    _license: &crate::painting::recording_slot::FlightLicense,
) {
    let mut changes = host.take_queued_changes_for_flight();
    host.let_frame_fly(drain, |mut state| {
        let run = move || {
            state.apply(changes.drain(..));
            let style = job.run(state.engine_mut());
            Landing { state, style, changes }
        };
        #[cfg(test)]
        return crate::stage_thread::InFlight::landed(run());
        #[cfg(not(test))]
        crate::stage_thread::style_layout_thread().submit(run)
    });
}

// A render state runs on a thread of its own, where nothing of the host may follow it: the shells and the callbacks
// into the host's DOM are main-thread objects because of what they hold and do, and stay with the host. The state,
// what a frame brings back, and every message and write the host sends it may cross, which the compiler checks here.
const _: () = {
    const fn assert_send<T: Send + ?Sized>() {}
    assert_send::<RenderState>();
    assert_send::<Landing>();
    assert_send::<RenderMessage>();
    assert_send::<ArenaChange>();
};

// Only a forced read waits for a frame in flight.
impl crate::stage_thread::Flown for Landing {
    type JoinRight = ForcedRead;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr::NonNull;
    use std::rc::Rc;

    #[test]
    fn a_forced_read_is_begun_by_its_outermost_scope_and_spent_once() {
        let host = DocumentHost::for_test();
        host.begin_forced_read(true);
        host.begin_forced_read(false);
        assert!(matches!(host.take_unstyled_read(), Some(ForcedRead::Script(_))));
        assert!(host.take_forced_read().is_none(), "a read is spent once");
        host.end_forced_read();
        host.end_forced_read();
        host.begin_forced_read(false);
        assert!(matches!(host.take_forced_read(), Some(ForcedRead::Host(_))));
        host.end_forced_read();
        assert!(host.take_forced_read().is_none(), "a read ends with its scope");
    }

    #[test]
    fn applying_queued_changes_keeps_the_queues_buffers() {
        use crate::layout::layout_changes::LayoutChange;
        let queue = ChangeQueue::default();
        let write = || ArenaChange::Layout(LayoutChange::RecordPartialRelayoutEscape);
        let mut buffers = [std::ptr::null(); 4];
        for buffer in &mut buffers {
            for _ in 0..8 {
                queue.push(write());
            }
            *buffer = queue.queued.borrow().as_ptr();
            queue.drain(NoFrameInFlight(()), false, |changes| assert_eq!(changes.count(), 8));
        }
        assert_eq!(buffers[0], buffers[2]);
        assert_eq!(buffers[1], buffers[3]);
        queue.push(write());
        queue.drain(NoFrameInFlight(()), false, |changes| {
            assert_eq!(changes.count(), 1);
            queue.push(write());
        });
        assert!(
            !queue.is_settled(),
            "a write queued meanwhile waits for the next application"
        );
    }

    #[test]
    fn only_a_change_that_writes_the_rows_makes_the_host_read_them_again() {
        use crate::layout::layout_changes::LayoutChange;
        use crate::layout::node_data::{NodeFlag, NodeSlotId};
        let pointer = document_host::document_host_create(0);
        // SAFETY: The host lives until it is destroyed below.
        let host = unsafe { &*pointer };
        // SAFETY: The arena lives as long as the host's render state, and nothing else reaches it meanwhile.
        let arena = unsafe { &mut *host.arena_for_test() };
        let row = arena.allocate_for_test().slot;
        host.fresh_rows(ScriptForcedRead::for_test());
        host.queue_change(ArenaChange::Layout(LayoutChange::SetNeedsFullLayoutTreeUpdate(true)));
        assert!(host.rows().is_some(), "a layout mark leaves the rows as they are");
        host.queue_change(ArenaChange::Layout(LayoutChange::InvalidateTextContent {
            node: NodeSlotId::INVALID,
        }));
        assert!(host.rows().is_some(), "a change of a row that is gone writes nothing");
        host.queue_change(ArenaChange::Layout(LayoutChange::SetNodeFlag {
            node: row,
            flag: NodeFlag::IsEditingHost,
            value: true,
        }));
        assert!(host.rows().is_none());
        assert_ne!(
            host.fresh_rows(ScriptForcedRead::for_test()).flags(row) & NodeFlag::IsEditingHost as u32,
            0
        );
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        // SAFETY: The host is destroyed once, and nothing reaches it after.
        unsafe { document_host::document_host_destroy(pointer) };
    }

    #[test]
    fn a_host_that_lets_go_of_its_rows_reads_them_again() {
        let pointer = document_host::document_host_create(0);
        // SAFETY: The host lives until it is destroyed below.
        let host = unsafe { &*pointer };
        let rows = host.fresh_rows(ScriptForcedRead::for_test());
        host.let_go_of_rows();
        assert!(host.rows().is_none());
        assert!(!Rc::ptr_eq(&rows, &host.fresh_rows(ScriptForcedRead::for_test())));
        // SAFETY: The host is destroyed once, and nothing reaches it after.
        unsafe { document_host::document_host_destroy(pointer) };
    }

    #[test]
    fn a_write_through_the_arena_makes_the_host_read_the_rows_again() {
        let pointer = document_host::document_host_create(0);
        // SAFETY: The host lives until it is destroyed below.
        let host = unsafe { &*pointer };
        host.fresh_rows(ScriptForcedRead::for_test());
        assert!(host.rows().is_some());
        // SAFETY: The arena lives as long as the host's render state, and nothing else reaches it meanwhile.
        let arena = unsafe { &mut *host.arena_for_test() };
        let row = arena.allocate_for_test().slot;
        assert!(host.rows().is_none());
        assert!(host.fresh_rows(ScriptForcedRead::for_test()).node(row).is_some());
        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(host.rows().is_none());
        // SAFETY: The host is destroyed once, and nothing reaches it after.
        unsafe { document_host::document_host_destroy(pointer) };
    }

    #[test]
    fn a_style_write_leaves_what_the_rows_are_to_read_from_the_rows_the_host_has() {
        use crate::layout::node_data::{NodeFlag, NodeKind, StylePayloadsRef};
        let pointer = document_host::document_host_create(0);
        // SAFETY: The host lives until it is destroyed below.
        let host = unsafe { &*pointer };
        // SAFETY: The arena lives as long as the host's render state, and nothing else reaches it meanwhile.
        let arena = unsafe { &mut *host.arena_for_test() };
        let row = arena.allocate_for_test().slot;
        arena.write_shape(row).set_kind(NodeKind::BlockContainer);
        host.fresh_rows(ScriptForcedRead::for_test());

        let shape = arena.write_shape(row);
        shape.set_style(StylePayloadsRef::new(NonNull::dangling().as_ptr()));
        shape.mark();
        assert!(host.rows().is_none(), "a style write leaves the rows stale");
        let identities = host.row_identities(ScriptForcedRead::for_test());
        assert_eq!(identities.identity_flags(row), 0);
        assert!(host.rows().is_none(), "reading what the rows are publishes none again");

        arena.set_node_flag(row, NodeFlag::Anonymous, true);
        assert_eq!(
            host.row_identities(ScriptForcedRead::for_test()).identity_flags(row),
            NodeFlag::Anonymous as u32
        );
        assert!(
            host.rows().is_some(),
            "a write of what a row is publishes the rows again"
        );

        arena
            .free_subtree(row)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(
            host.row_identities(ScriptForcedRead::for_test())
                .shell_facts(row)
                .is_none()
        );
        // SAFETY: The host is destroyed once, and nothing reaches it after.
        unsafe { document_host::document_host_destroy(pointer) };
    }
}
