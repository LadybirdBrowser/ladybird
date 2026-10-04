/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Each document's render state, named by a [`DocumentId`] and reached only through messages.
//!
//! A document's [`RenderState`] is what its style, layout and paint preparation compute over: its layout arena and
//! what lives beside it. The host holds the document's id and reaches the state by sending a [`RenderMessage`]
//! through [`send`] to the StyleLayout thread, which the states live on. Only [`handle`] mints the [`RenderingSide`]
//! that the states are reached with, so code that is not handed one cannot reach a document's render state.

use crate::css::style::StyleEngineHandle;
use crate::css::style::bridge::{FfiDeviceClass, create_document_style_engine};
use crate::fast_hash::FastMap as HashMap;
use crate::layout::ArenaHandle;
use std::cell::{Cell, RefCell};
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

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
pub(crate) use wait::{
    ForcedRead, FrameJobPermit, LockstepProof, RenderJob, RenderWait, ReplyTo, ScriptForcedRead, SpentWait,
    StyleJobPermit, TaskBoundary, force_read, force_read_flown_style, render_state_died, run_job,
    wait_for_render_state,
};

/// The host's name for one document's render state. The host mints it, so naming a new document needs no answer from
/// the render side.
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug, Default)]
pub struct DocumentId(pub u64);

impl DocumentId {
    fn mint() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self(NEXT.fetch_add(1, Ordering::Relaxed))
    }
}

/// Proof that the caller handles a render message, and so may reach the render states.
///
/// Only [`handle`] mints one, for as long as it handles a message. The raw pointer marker makes the proof neither
/// [`Send`] nor [`Sync`], so it cannot leave the thread that handles the message.
pub(crate) struct RenderingSide {
    not_send_or_sync: PhantomData<*const ()>,
}

/// One document's render state.
pub(crate) struct RenderState {
    /// The layout arena. It links the style engine below, which outlives it.
    arena: Box<ArenaHandle>,
    /// The document's style engine, which the state owns through the handle the arena links. Every borrow of the
    /// engine comes from this one pointer, and a message borrows it mutably only where it reaches the engine alone.
    engine: StyleEngineHandle,
}

/// What the host keeps of its document's new render state.
pub(crate) struct CreatedState {
    /// Where the state keeps its arena until it is destroyed, for the changes and questions the host applies and
    /// answers where it is.
    pub(crate) arena: NonNull<ArenaHandle>,
    /// Where the state keeps its style engine until it is destroyed, for the same.
    pub(crate) engine: StyleEngineHandle,
    /// The flag the state raises once any element has random base values, and never lowers.
    pub(crate) element_random_base_values_exist: Arc<AtomicBool>,
}

// SAFETY: The host reaches the arena and the engine only through its changes and its questions, which run while nothing
// on the render side reaches them.
unsafe impl Send for CreatedState {}

impl RenderState {
    /// Makes the state of a document, and answers what the host keeps of it.
    fn new(device_class: FfiDeviceClass) -> (Self, CreatedState) {
        let mut arena = Box::new(ArenaHandle::new());
        let engine = create_document_style_engine(device_class);
        let element_random_base_values_exist = engine.element_random_base_values_exist();
        let engine = StyleEngineHandle::create(engine);
        arena.arena().set_style_engine(engine);
        let created = CreatedState {
            arena: NonNull::from(&mut *arena),
            engine,
            element_random_base_values_exist,
        };
        (Self { arena, engine }, created)
    }

    /// Drops the state, which must hold no layout node any more.
    fn retire(self) {
        let Self { arena, engine } = self;
        arena.arena().assert_owner_thread();
        assert_eq!(
            arena.arena().live_slot_count(),
            0,
            "layout node arena destroyed with live slots"
        );
        drop(arena);
        // SAFETY: The state made the handle, and the arena that linked it is gone.
        unsafe { engine.destroy() }.end_recording();
    }
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
    pub(crate) unsafe fn apply(self, arena: &mut crate::layout::LayoutNodeArena, engine: StyleEngineHandle) {
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
/// DocumentHost::take_frame_in() mints it.
struct NoFrameInFlight(());

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

    /// Lends the queued writes of `document` to `apply`, which applies them, and keeps their emptied buffer as the
    /// spare. A write the host queues meanwhile, as the writes are applied or the render side works, waits for the next
    /// application, as do the style writes where `hold_style`.
    fn drain(
        &self,
        _landed: NoFrameInFlight,
        document: DocumentId,
        hold_style: bool,
        apply: impl FnOnce(QueuedChanges<'_>),
    ) {
        let mut queued = self.queued.replace(self.spare.take());
        if hold_style {
            self.queue_style_writes_of(&mut queued);
        }
        apply(QueuedChanges {
            document,
            changes: queued.drain(..),
        });
        self.spare.set(queued);
        self.settled.set(self.queued.borrow().is_empty());
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

/// The writes a host queued for its document's render state since the render side last applied them, in the order the
/// host made them, lent out of the host's queue. They go to the render side ahead of the host's next message, which
/// they come before.
pub(crate) struct QueuedChanges<'a> {
    document: DocumentId,
    changes: std::vec::Drain<'a, ArenaChange>,
}

impl QueuedChanges<'_> {
    /// Applies the changes to `arena` and `engine`, the arena and style engine of their document.
    ///
    /// # Safety
    ///
    /// `engine` must name the live style engine `arena` links, which nothing else borrows meanwhile.
    unsafe fn apply(self, arena: &mut crate::layout::LayoutNodeArena, engine: StyleEngineHandle) {
        for change in self.changes {
            // SAFETY: Guaranteed by the caller.
            unsafe { change.apply(arena, engine) };
        }
    }
}

/// A message the host sends a document's render state.
#[expect(
    clippy::large_enum_variant,
    reason = "a message is made once, in the frame of the host that waits for it"
)]
pub(crate) enum RenderMessage<'a> {
    /// Makes the render state of a new document.
    Create {
        document: DocumentId,
        device_class: FfiDeviceClass,
        reply: ReplyTo<'a, CreatedState>,
    },
    /// Drops the render state of a document the host has let go of.
    Destroy { document: DocumentId },
    /// A style transaction of the document the host waits for.
    Style {
        document: DocumentId,
        job: crate::css::style::style_job::StyleJob,
        reply: ReplyTo<'a, crate::css::style::style_job::StyleJobAnswer>,
        /// What shows that a forced read or a job's permit sent the transaction.
        _spent: SpentWait,
    },
    /// A layout round of the document the host waits for: its tree build and layout stages.
    LayoutRound {
        document: DocumentId,
        job: crate::layout::LayoutRoundJob,
        reply: ReplyTo<'a, crate::layout::LayoutRoundAnswer>,
        /// What shows that a forced read or a job's permit sent the round.
        _spent: SpentWait,
    },
    /// A step of paint preparation the host waits for.
    Paint {
        document: DocumentId,
        pass: crate::painting::paint_passes::PaintPass,
        reply: ReplyTo<'a, crate::painting::paint_passes::PaintPassAnswer>,
    },
    /// Panics answering, for a test that the host waiting for the answer crashes.
    PanicForTesting { reply: ReplyTo<'a, ()> },
}

thread_local! {
    // The render state of each document, on the thread that handles render messages.
    static STATES: RefCell<HashMap<DocumentId, RenderState>> = RefCell::default();
}

/// Handles `message` on the render side, after the changes its host queued before it.
fn handle(changes: QueuedChanges<'_>, message: RenderMessage<'_>) {
    let side = RenderingSide {
        not_send_or_sync: PhantomData,
    };
    apply_changes(&side, changes);
    handle_message(&side, message);
}

fn apply_changes(_: &RenderingSide, changes: QueuedChanges<'_>) {
    if changes.changes.len() == 0 {
        return;
    }
    let (arena, engine) = state_parts(changes.document).expect("a document the host changes has a render state");
    // SAFETY: The state keeps the arena and the engine where they are while the changes are applied, and nothing else
    // reaches them meanwhile.
    unsafe { changes.apply((*arena).arena_mut(), engine) };
}

fn handle_message(_: &RenderingSide, message: RenderMessage<'_>) {
    match message {
        RenderMessage::Create {
            document,
            device_class,
            reply,
        } => reply.answer(|| {
            let (state, created) = RenderState::new(device_class);
            let previous = STATES.with_borrow_mut(|states| states.insert(document, state));
            debug_assert!(previous.is_none(), "document {document:?} created twice");
            created
        }),
        RenderMessage::Destroy { document } => {
            let state = STATES.with_borrow_mut(|states| states.remove(&document));
            // A document with no state is a bug of the sender's, which leaves nothing to drop.
            debug_assert!(state.is_some(), "document {document:?} destroyed twice");
            if let Some(state) = state {
                state.retire();
            }
        }
        RenderMessage::Style {
            document, job, reply, ..
        } => reply.answer(|| {
            let (_, engine) = state_parts(document).expect("a document the host styles has a render state");
            // SAFETY: The state keeps the engine where it is while the message is handled, and nothing else reaches it
            // meanwhile.
            job.run(unsafe { engine.get_mut() })
        }),
        RenderMessage::LayoutRound {
            document, job, reply, ..
        } => reply.answer(|| {
            let (arena, _) = state_parts(document).expect("a document the host lays out has a render state");
            // SAFETY: As for a style job. The host keeps what the job's inputs name until it has the answer.
            job.run(unsafe { &mut *arena })
        }),
        RenderMessage::Paint { document, pass, reply } => reply.answer(|| {
            let (arena, _) = state_parts(document).expect("a document the host paints has a render state");
            // SAFETY: As for a style job.
            pass.run(unsafe { &mut *arena }.arena_mut())
        }),
        RenderMessage::PanicForTesting { reply } => reply.answer(|| panic!("the render state panicked for a test")),
    }
}

/// The arena and the style engine of `document`'s render state, which stay where they are until the state is destroyed.
/// The map is not borrowed while a message reaches them, so a message handled meanwhile for another document finds its
/// own.
fn state_parts(document: DocumentId) -> Option<(*mut ArenaHandle, StyleEngineHandle)> {
    STATES.with_borrow_mut(|states| {
        let state = states.get_mut(&document);
        debug_assert!(state.is_some(), "document {document:?} has no render state");
        state.map(|state| (std::ptr::from_mut::<ArenaHandle>(&mut state.arena), state.engine))
    })
}

/// Submits `job`, a style transaction of `host`'s document, to the render side, the StyleLayout thread, behind the frame
/// in flight and the changes the host queued, and goes on: the host takes what the job answers in from the flight, and
/// the document drains its reactions with `drain`. Only a transaction that `_license` lets fly is submitted.
pub(crate) fn fly(
    host: &DocumentHost,
    job: crate::css::style::style_job::StyleJob,
    drain: crate::css::style::style_job::FfiFlownStyleDrain,
    _license: &crate::painting::recording_slot::FlightLicense,
) {
    let mut changes = host.take_queued_changes_for_flight();
    let document = host.document();
    let run = move || {
        let side = RenderingSide {
            not_send_or_sync: PhantomData,
        };
        apply_changes(
            &side,
            QueuedChanges {
                document,
                changes: changes.drain(..),
            },
        );
        let (_, engine) = state_parts(document).expect("a document whose style flies has a render state");
        // SAFETY: The state keeps the engine where it is while the job runs, and the host reaches the state again only
        // once it has taken the flight in.
        (job.run(unsafe { engine.get_mut() }), changes)
    };
    #[cfg(test)]
    host.let_style_fly(crate::stage_thread::InFlight::landed(run()), drain);
    #[cfg(not(test))]
    host.let_style_fly(crate::stage_thread::style_layout_thread().submit(run), drain);
}

/// Sends `message` about `host`'s document to the render side, the StyleLayout thread, behind the frame in flight and
/// the changes the host queued, and waits until it is handled, so the message may borrow from the calling frame. A message sent while the
/// thread handles another (a child document's) is handled right there, and a unit test's render states stay on the
/// test's own thread. A message writes the render state, unless it is a paint pass that leaves the paint and hit testing
/// properties current.
pub(crate) fn send(host: &DocumentHost, message: RenderMessage<'_>) {
    if !matches!(&message, RenderMessage::Paint { pass, .. } if pass.leaves_paint_preparation_current()) {
        host.note_render_state_write();
    }
    host.drain_queued_changes(|changes| {
        if cfg!(test) {
            return handle(changes, message);
        }
        crate::stage_thread::style_layout_thread().run(|| handle(changes, message));
    });
}

// A render state lives on a thread of its own, where nothing of the host may follow it: the shells and the callbacks
// into the host's DOM are main-thread objects because of what they hold and do, and stay with the host. The state and
// every message the host sends it may cross, which the compiler checks here.
const _: () = {
    const fn assert_send<T: Send + ?Sized>() {}
    assert_send::<RenderState>();
    assert_send::<RenderMessage>();
    assert_send::<QueuedChanges>();
};

#[cfg(test)]
mod tests {
    use super::*;
    use std::rc::Rc;

    trait AmbiguousIfSend<A> {
        fn marker() {}
    }

    impl<T: ?Sized> AmbiguousIfSend<()> for T {}
    impl<T: ?Sized + Send> AmbiguousIfSend<u8> for T {}

    // Fails to compile, as the call is ambiguous, if the proof ever becomes Send.
    #[test]
    fn the_rendering_side_is_not_send() {
        <RenderingSide as AmbiguousIfSend<_>>::marker();
    }

    fn state_count() -> usize {
        STATES.with_borrow(HashMap::len)
    }

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
    fn document_ids_are_minted_by_the_host() {
        let first = DocumentId::mint();
        let second = DocumentId::mint();
        assert!(first != DocumentId::default() && second != DocumentId::default());
        assert_ne!(first, second);
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
            queue.drain(NoFrameInFlight(()), DocumentId::default(), false, |changes| {
                assert_eq!(changes.changes.count(), 8)
            });
        }
        assert_eq!(buffers[0], buffers[2]);
        assert_eq!(buffers[1], buffers[3]);
        queue.push(write());
        queue.drain(NoFrameInFlight(()), DocumentId::default(), false, |changes| {
            assert_eq!(changes.changes.count(), 1);
            queue.push(write());
        });
        assert!(
            !queue.is_settled(),
            "a write queued meanwhile waits for the next application"
        );
    }

    #[test]
    fn a_document_host_holds_a_render_state_until_it_is_destroyed() {
        let host = document_host::document_host_create(0);
        assert_eq!(state_count(), 1);
        // SAFETY: As above.
        unsafe { document_host::document_host_destroy(host) };
        assert_eq!(state_count(), 0);
    }

    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "destroyed twice")]
    fn destroying_a_document_twice_is_a_senders_bug() {
        let host = DocumentHost::for_test();
        let document = DocumentId::mint();
        wait_for_render_state(ScriptForcedRead::for_test(), &host, |reply| RenderMessage::Create {
            document,
            device_class: FfiDeviceClass::ForegroundDesktop,
            reply,
        });
        send(&host, RenderMessage::Destroy { document });
        send(&host, RenderMessage::Destroy { document });
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
