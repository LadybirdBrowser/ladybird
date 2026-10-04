/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Each document's render state, which the StyleLayout thread owns.
//!
//! A document's [`RenderState`] is what its style, layout and paint preparation compute over: its layout arena and
//! what lives beside it. It lives on the StyleLayout thread, the render owner, which makes it the first time a job of
//! the document reaches it and drops it there (see [`owner`]). The [`DocumentHost`] names it, and reaches it only with
//! a job it hands the owner: a [`RenderMessage`] it [`send`]s and waits for, a question it asks and waits for, the
//! writes it queued, which go first or are posted ahead, or a frame that flies beside the host until the host takes
//! it in. Nothing on the host's thread can name the state, so nothing there reaches it.

use crate::css::style::StyleEngineHandle;
use crate::css::style::bridge::create_document_style_engine;
use crate::layout::ArenaHandle;
use std::cell::{Cell, RefCell};

mod clock;
mod devtools;
mod document_host;
mod owner;
mod questions;
mod wait;

pub(crate) use clock::ClockPlan;
pub use document_host::DocumentHost;
pub(crate) use document_host::OwedWorkPayment;
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

/// One document's render state, on the render owner.
pub(crate) struct RenderState {
    /// The layout arena. It links the style engine below, which outlives it.
    arena: Box<ArenaHandle>,
    /// The document's style engine, which the state owns through the handle the arena links. Every borrow of the
    /// engine comes from this one pointer, and a borrow of the state borrows it mutably only where it reaches the
    /// engine alone.
    engine: StyleEngineHandle,
    /// What the writes the state applied since the host's last job ended owe the host, which the host pays once it has
    /// the job back.
    owed: Vec<crate::layout::tree_mutation::HostWorkDue>,
}

impl RenderState {
    /// Makes the state `seed` describes.
    fn new(seed: owner::StateSeed) -> Self {
        let owner::StateSeed { device_class, shared } = seed;
        let mut arena = Box::new(ArenaHandle::new());
        let mut engine = create_document_style_engine(device_class);
        engine.share_element_random_base_values_exist(shared.element_random_base_values_exist);
        engine.share_declaration_block_versions(shared.declaration_block_versions);
        engine.share_container_effects_held(shared.container_effects_held);
        let engine = StyleEngineHandle::create(engine);
        arena
            .arena_mut()
            .share_svg_paint_resources_enrolled(shared.svg_paint_resources_enrolled);
        arena.arena_mut().set_style_engine(engine);
        Self {
            arena,
            engine,
            owed: Vec::new(),
        }
    }

    /// Runs `job` on the state with `marks`, the layout tree update marks the document's host lends it, if it lends
    /// them, and answers them back.
    fn with_marks<R>(
        &mut self,
        marks: Option<crate::layout::tree_update_marks::LayoutTreeUpdateMarks>,
        job: impl FnOnce(&mut Self) -> R,
    ) -> (R, Option<crate::layout::tree_update_marks::LayoutTreeUpdateMarks>) {
        let Some(marks) = marks else {
            return (job(self), None);
        };
        *self.arena.arena().layout_tree_update_marks().borrow_mut() = marks;
        let answer = job(self);
        (answer, Some(self.arena.arena().layout_tree_update_marks().take()))
    }

    /// Drops the state, which must hold no layout node any more.
    fn retire(self) {
        let Self { arena, engine, owed } = self;
        assert!(owed.is_empty(), "every job's host pays what its writes owe it");
        assert_eq!(
            arena.arena().live_slot_count(),
            0,
            "layout node arena destroyed with live slots"
        );
        drop(arena);
        // SAFETY: The state made the handle, and the arena that linked it is gone.
        unsafe { engine.destroy() }.end_recording();
    }

    /// How far the arena's rows have been written, which the host keeps to know whether the rows it holds still read
    /// as the arena.
    fn rows_version(&self) -> crate::layout::RowsVersion {
        self.arena.arena().rows_version()
    }

    /// Applies `changes`, writes the host queued, in the order the host made them.
    fn apply(&mut self, changes: impl IntoIterator<Item = ArenaChange>) {
        for change in changes {
            // SAFETY: The state is borrowed mutably, and so is the engine its arena links.
            unsafe { change.apply(self.arena.arena_mut(), self.engine, &mut self.owed) };
        }
    }

    /// Takes what the writes the state applied owe the host, for the host to pay once it has the job back, what the
    /// host learns of the engine's deferred element style inputs, where they moved, and the facts of the state as the
    /// job leaves it.
    fn take_owed(&mut self) -> Owed {
        let engine = self.engine_ref();
        let facts = StateFacts {
            has_pending_style_transaction: engine.has_pending_transaction(),
            has_deferred_element_style_inputs: engine.has_deferred_element_style_inputs(),
            has_size_containers_needing_evaluation_after_layout: engine
                .has_size_containers_needing_evaluation_after_layout(),
            layout_is_up_to_date_unless_built: self.arena.arena().layout_is_up_to_date(false),
            rendering_preparation_pending: crate::painting::paint_passes::rendering_preparation_pending(
                self.arena.arena(),
            )
            .is_some(),
            owes_image_resources: self.arena.arena().owes_image_resources_to_host(),
            selector_attribute_value_text_requirements_version: engine
                .selector_attribute_value_text_requirements_version(),
        };
        let selector_value_text_names = std::sync::Arc::clone(engine.selector_attribute_value_text_names());
        Owed {
            work: std::mem::take(&mut self.owed),
            deferred_inputs: self.engine_mut().take_moved_deferred_element_style_inputs(),
            facts,
            selector_value_text_names,
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

    /// Runs `round` after `style`, the frame's style transaction, where the frame applies the transaction's rows to the
    /// boxes itself: the round lays out the boxes as the host's install of the rows leaves them. Answers the rows it
    /// applied, by element, and what the round owes the host with the rows it laid out, which the frame publishes.
    fn fly_round(
        &mut self,
        style: &crate::css::style::style_job::StyleJobAnswer,
        round: crate::layout::SealedRound,
    ) -> (
        Vec<crate::css::style::flight_style_rows::FlightStyleRow>,
        Option<(crate::layout::FlownRound, crate::layout::row_reads::RowSnapshot)>,
    ) {
        use crate::layout::tree_mutation::{HostCalls, OwedHostWork};
        let Ok(rows) = self.engine_ref().rows_the_flight_applies(style.rows()) else {
            return (Vec::new(), None);
        };
        // What applying the rows owes the host, the host's install of the rows makes good again.
        let work = OwedHostWork::default();
        let Ok(applied) = self.arena.arena().apply_flight_style_rows(HostCalls(&work), rows) else {
            return (Vec::new(), None);
        };
        let round = round
            .run(&mut self.arena, work)
            .map(|round| (round, self.arena.arena_mut().publish_row_snapshot(false)));
        (applied, round)
    }

    /// Handles `message`.
    fn handle(&mut self, message: RenderMessage<'_>) {
        match message {
            RenderMessage::Style { job, reply, .. } => reply.answer(|| job.run(self.engine_mut())),
            // The host keeps what the job's inputs name until it has the answer.
            RenderMessage::LayoutRound { job, reply, .. } => reply.answer(|| job.run(&mut self.arena)),
            RenderMessage::Paint { pass, reply } => reply.answer(|| pass.run(self.arena.arena_mut())),
            RenderMessage::PanicForTesting { reply } => reply.answer(|| panic!("the render state panicked for a test")),
        }
    }
}

/// What a frame that flew brings its host back: what its style transaction answered, the rows of the transaction it
/// applied to the boxes itself, by element, what its first layout round owes with the rows it published after it, where
/// it ran one, and the emptied buffer of the writes it took, which the host's queue keeps.
pub(crate) struct Landing {
    style: crate::css::style::style_job::StyleJobAnswer,
    applied: Vec<crate::css::style::flight_style_rows::FlightStyleRow>,
    round: Option<(crate::layout::FlownRound, crate::layout::row_reads::RowSnapshot)>,
    changes: Vec<ArenaChange>,
    /// The layout tree update marks the host lent the frame.
    marks: Option<crate::layout::tree_update_marks::LayoutTreeUpdateMarks>,
    /// What the writes the frame applied owe the host.
    owed: Owed,
}

/// What a job or a frame leaves its host to take back: what the writes it applied owe the host, and the element style
/// inputs the engine defers as it ended, where they moved.
pub(crate) struct Owed {
    work: Vec<crate::layout::tree_mutation::HostWorkDue>,
    deferred_inputs: Option<Vec<crate::css::style::engine_calls::DeferredInput>>,
    facts: StateFacts,
    /// The attribute names whose value text the engine's selectors read, as of `facts`.
    selector_value_text_names: crate::css::style::SelectorValueTextNames,
}

/// What a document's render state answers of itself as a job or a frame leaves it, which the host reads in place for as
/// long as it writes nothing: only the host's jobs and frames write the state.
#[derive(Clone, Copy)]
pub(crate) struct StateFacts {
    pub(crate) has_pending_style_transaction: bool,
    pub(crate) has_deferred_element_style_inputs: bool,
    pub(crate) has_size_containers_needing_evaluation_after_layout: bool,
    /// Whether the layout is up to date, unless the document's layout tree update marks ask for a build of it.
    pub(crate) layout_is_up_to_date_unless_built: bool,
    /// Whether preparing the document for rendering has something to do.
    pub(crate) rendering_preparation_pending: bool,
    /// Whether the layout tree builds owe the host image resources, which no write moves (see
    /// [`DocumentHost::known_owed_image_resources`]).
    pub(crate) owes_image_resources: bool,
    /// Where the engine's selectors' requirements of attribute value text are.
    pub(crate) selector_attribute_value_text_requirements_version: u64,
}

// Every write the host makes is moved through its queue and into the render state, so a variant that carries a large
// payload inline instead of behind a pointer makes every write slower.
const _: () = assert!(std::mem::size_of::<ArenaChange>() <= 72);

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
    /// What is left of the boxes of the nodes the identities name is detached as the nodes leave the document (see
    /// [`crate::layout::tree_builder::detach_remaining_rows_for_removal`]), which owes the host what it pays once the
    /// job that applies it is done.
    DetachForRemoval(Box<[u32]>),
    /// The anonymous rows below the row inherit its style again (see
    /// [`crate::layout::LayoutNodeArena::reinherit_anonymous_descendants`]), which owes their layout
    /// nodes the records they take, paid once the job that applies it is done.
    ReinheritAnonymousDescendants(crate::layout::node_data::NodeSlotId),
    /// A removed node's box leaves its parent's box in place, or the parent is marked for a rebuild, as the render state
    /// finds the boxes (see [`crate::layout::box_removal`]), which owes the host what it pays once the job that applies
    /// it is done.
    RemoveBox(crate::layout::box_removal::BoxRemoval),
    /// A write to the document's style sheets.
    Rule(crate::css::style::rule_writes::RuleWrite),
}

impl ArenaChange {
    /// # Safety
    ///
    /// `engine` must name the live style engine `arena` links, which nothing else borrows meanwhile.
    unsafe fn apply(
        self,
        arena: &mut crate::layout::LayoutNodeArena,
        engine: StyleEngineHandle,
        owed: &mut Vec<crate::layout::tree_mutation::HostWorkDue>,
    ) {
        match self {
            Self::Layout(change) => change.apply(arena),
            Self::DetachForRemoval(nodes) => owed.push(
                crate::layout::layout_changes::LayoutWrite::DetachRemainingRowsForRemoval(&nodes)
                    .apply(arena)
                    .host_work,
            ),
            // A job or frame the host has not had back yet may have freed the row since.
            Self::ReinheritAnonymousDescendants(node) => {
                if arena.slot_is_live(node) {
                    owed.push(
                        crate::layout::layout_changes::LayoutWrite::ReinheritAnonymousDescendants { node }
                            .apply(arena)
                            .host_work,
                    );
                }
            }
            Self::RemoveBox(removal) => removal.apply(arena, owed),
            Self::Paint(change) => change.apply(arena),
            // SAFETY: Guaranteed by the caller. A style change reaches the engine only through this borrow.
            Self::Style(change) => change.apply(unsafe { engine.get_mut() }),
            // SAFETY: As above.
            Self::Engine(write) => write.apply(unsafe { engine.get_mut() }),
            // SAFETY: As above.
            Self::Rule(write) => write.apply(unsafe { engine.get_mut() }),
        }
    }

    /// Whether the change writes what the document's style engine computes from. A mint of style node identities does
    /// not: it makes them live, in the order the host minted them.
    fn writes_style(&self) -> bool {
        match self {
            Self::Style(change) => !change.notes_attribute_name(),
            Self::Engine(write) => !matches!(write, crate::css::style::engine_calls::EngineWrite::MintStyleNodes(_)),
            Self::Rule(_) => true,
            Self::Layout(_)
            | Self::Paint(_)
            | Self::DetachForRemoval(_)
            | Self::ReinheritAnonymousDescendants(_)
            | Self::RemoveBox(_) => false,
        }
    }

    /// Whether the change may move a fact the host knows of the render state (see [`StateFacts`]): a write to the paint
    /// state never does, nor do some style and layout writes.
    fn may_move_facts(&self) -> bool {
        match self {
            Self::Paint(_) => false,
            Self::Style(change) => change.may_move_facts(),
            Self::Layout(change) => change.may_move_facts(),
            Self::Engine(_)
            | Self::DetachForRemoval(_)
            | Self::ReinheritAnonymousDescendants(_)
            | Self::RemoveBox(_)
            | Self::Rule(_) => true,
        }
    }

    /// Whether the change may write the arena, and so the rows: a write to the style engine never does.
    fn may_write_rows(&self) -> bool {
        self.row_write() != RowWrite::None
    }

    /// How far the change may write the rows.
    fn row_write(&self) -> RowWrite {
        match self {
            Self::Layout(change) => change.row_write(),
            Self::Paint(_) => RowWrite::Rows,
            Self::DetachForRemoval(_) | Self::RemoveBox(_) => RowWrite::Identities,
            Self::ReinheritAnonymousDescendants(_) => RowWrite::Styles,
            Self::Style(_) | Self::Engine(_) | Self::Rule(_) => RowWrite::None,
        }
    }
}

/// How far a change may write a document's rows.
#[derive(Clone, Copy, Default, PartialEq, Eq, PartialOrd, Ord, Debug)]
pub(crate) enum RowWrite {
    #[default]
    None,
    /// What a row holds, but neither its style record, what it is, nor the row a node is bound to (see
    /// [`crate::layout::LayoutNodeArena::rows_identity_version`]).
    Rows,
    /// A row's style record too.
    Styles,
    /// What a row is, or the row a node is bound to, too.
    Identities,
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
    /// How far the writes queued may write the rows (see [`ArenaChange::row_write`]).
    row_write: Cell<RowWrite>,
    /// What the writes queued may move of what the host knows of the render state.
    moves: Cell<QueuedMoves>,
}

/// What writes may move of what the host knows of the render state.
#[derive(Clone, Copy, Default)]
struct QueuedMoves {
    /// A fact (see [`ArenaChange::may_move_facts`]).
    facts: bool,
    /// The engine's selectors, which only a rule write compiles.
    selectors: bool,
}

impl QueuedMoves {
    fn of<'a>(changes: impl IntoIterator<Item = &'a ArenaChange>) -> Self {
        changes.into_iter().fold(Self::default(), |moves, change| Self {
            facts: moves.facts || change.may_move_facts(),
            selectors: moves.selectors || matches!(change, ArenaChange::Rule(_)),
        })
    }

    fn with(self, other: Self) -> Self {
        Self {
            facts: self.facts || other.facts,
            selectors: self.selectors || other.selectors,
        }
    }
}

impl ChangeQueue {
    fn is_empty(&self) -> bool {
        self.queued.borrow().is_empty()
    }

    fn push(&self, change: ArenaChange) {
        use crate::css::style::bridge::StyleChange;
        self.row_write.set(self.row_write.get().max(change.row_write()));
        self.moves.set(self.moves.get().with(QueuedMoves::of([&change])));
        let mut queued = self.queued.borrow_mut();
        // An epoch of style record views that ends before anything else is queued or applied in it views nothing.
        if matches!(change, ArenaChange::Style(StyleChange::EndStyleRecordViewEpoch {}))
            && matches!(
                queued.last(),
                Some(ArenaChange::Style(StyleChange::BeginStyleRecordViewEpoch {}))
            )
        {
            queued.pop();
            return;
        }
        queued.push(change);
    }

    fn may_move_facts(&self) -> bool {
        self.moves.get().facts
    }

    fn may_compile_selectors(&self) -> bool {
        self.moves.get().selectors
    }

    /// Queues `change` ahead of every write queued before it.
    fn push_front(&self, change: ArenaChange) {
        self.row_write.set(self.row_write.get().max(change.row_write()));
        self.queued.borrow_mut().insert(0, change);
    }

    fn may_write_rows(&self) -> bool {
        self.row_write.get() != RowWrite::None
    }

    fn may_write_row_identities(&self) -> bool {
        self.row_write.get() == RowWrite::Identities
    }

    fn may_write_row_styles(&self) -> bool {
        self.row_write.get() >= RowWrite::Styles
    }

    /// Lends the queued writes to `apply`, which applies them, and keeps their emptied buffer as the spare. A write the
    /// host queues meanwhile, as the writes are applied or the render side works, waits for the next application, as do
    /// the style writes where `hold_style`.
    fn drain<R>(&self, hold_style: bool, apply: impl FnOnce(std::vec::Drain<'_, ArenaChange>) -> R) -> R {
        let mut queued = self.queued.replace(self.spare.take());
        self.row_write.set(RowWrite::None);
        self.moves.take();
        if hold_style {
            self.queue_style_writes_of(&mut queued);
        }
        let answer = apply(queued.drain(..));
        self.spare.set(queued);
        answer
    }

    /// Takes the queued writes, for a style transaction that flies with them. Their buffer comes back with
    /// give_back().
    fn take(&self) -> Vec<ArenaChange> {
        self.row_write.set(RowWrite::None);
        self.moves.take();
        self.queued.replace(self.spare.take())
    }

    /// Moves the style writes of `changes` back into the queue, where they wait for the next application.
    #[cold]
    fn queue_style_writes_of(&self, changes: &mut Vec<ArenaChange>) {
        let mut queued = self.queued.borrow_mut();
        queued.extend(changes.extract_if(.., |change| change.writes_style()));
        self.moves.set(QueuedMoves::of(queued.iter()));
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
        self.moves.set(self.moves.get().with(QueuedMoves::of(&held)));
        self.queued.borrow_mut().append(&mut held);
        self.spare.set(held);
    }
}

/// A message the host sends its document's render state, and waits for.
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

/// Runs `job` on the render owner, the StyleLayout thread, and waits for it, so it may borrow from the calling frame. A
/// job handed from that thread itself, as a host callback's while the owner runs another job, runs right there, and a
/// unit test's jobs run on the test's own thread.
fn on_render_side<R: Send>(job: impl FnOnce() -> R + Send) -> R {
    if cfg!(test) {
        return job();
    }
    crate::stage_thread::style_layout_thread().run(job)
}

/// Posts `job` to the render owner, which runs it after the jobs handed to it before, and goes on. A unit test's job
/// runs right here.
fn post_to_render_side(job: impl FnOnce() + Send + 'static) {
    if cfg!(test) {
        return job();
    }
    crate::stage_thread::style_layout_thread().post(job);
}

/// Sends `message` to the render state of `host`'s document, after the writes the host queued, and waits until it is
/// handled, taking a frame in flight in first with `read`. A message writes the render state, unless it is a paint pass
/// that leaves the paint and hit testing properties current.
pub(crate) fn send(host: &DocumentHost, read: ReadRight, message: RenderMessage<'_>) {
    if !matches!(&message, RenderMessage::Paint { pass, .. } if pass.leaves_paint_preparation_current()) {
        host.note_render_state_write();
    }
    if matches!(message, RenderMessage::LayoutRound { .. }) {
        host.forget_layout_up_to_date();
    }
    host.reach(read, move |state| state.handle(message));
}

/// Submits `job`, a style transaction of `host`'s document, to the render owner, with the writes the host queued and
/// `round`, the layout round the host sealed, and goes on: the frame flies beside the host until the host takes it in
/// and drains the transaction's reactions. Only a transaction that `_license` lets fly is submitted.
///
/// The frame applies the transaction's rows to the boxes itself before the round, where it can: otherwise the round is
/// left unrun, and the host lays out after it installs the rows.
pub(crate) fn fly(
    host: &DocumentHost,
    job: crate::css::style::style_job::StyleJob,
    round: Option<crate::layout::SealedRound>,
    _license: &crate::painting::recording_slot::FlightLicense,
) {
    let mut changes = host.take_queued_changes_for_flight();
    // The round writes the rows the host has, which it copies rather than writes in place while the host holds them.
    if round.is_some() {
        host.let_go_of_rows();
    }
    host.let_frame_fly(|document, seed, marks| {
        // A host that waits for the frame says the stop word, and the frame comes back with its style alone.
        let run = move |stop: &crate::stage_thread::StopWord| {
            owner::with_state(document, seed, |state| {
                let ((style, applied, round), marks) = state.with_marks(marks, |state| {
                    state.apply(changes.drain(..));
                    let style = job.run(state.engine_mut());
                    let (applied, round) = match round {
                        Some(round) if !stop.is_said() => state.fly_round(&style, round),
                        _ => (Vec::new(), None),
                    };
                    (style, applied, round)
                });
                let owed = state.take_owed();
                Landing {
                    style,
                    applied,
                    round,
                    changes,
                    marks,
                    owed,
                }
            })
        };
        #[cfg(test)]
        return crate::stage_thread::InFlight::landed(run(&crate::stage_thread::StopWord::default()));
        #[cfg(not(test))]
        crate::stage_thread::style_layout_thread().submit(run)
    });
}

// A render state lives on the render owner, where nothing of the host may follow it: the shells and the callbacks into
// the host's DOM are main-thread objects because of what they hold and do, and stay with the host. The state, what a
// frame brings back, and every message and write the host sends it may cross, which the compiler checks here.
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
    fn a_render_state_is_made_by_the_first_job_of_its_host() {
        let host = DocumentHost::for_test();
        host.queue_change(ArenaChange::Layout(
            crate::layout::layout_changes::LayoutChange::RecordPartialRelayoutEscape,
        ));
        assert!(!host.has_made_state(), "a queued write waits for the host's first job");
        host.fresh_rows(ScriptForcedRead::for_test());
        assert!(host.has_made_state());
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
            queue.drain(false, |changes| assert_eq!(changes.count(), 8));
        }
        assert_eq!(buffers[0], buffers[2]);
        assert_eq!(buffers[1], buffers[3]);
        queue.push(write());
        queue.drain(false, |changes| {
            assert_eq!(changes.count(), 1);
            queue.push(write());
        });
        assert_eq!(
            queue.queued.borrow().len(),
            1,
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
        host.queue_change(ArenaChange::Layout(LayoutChange::SetNeedsFullLayoutTreeUpdate));
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

        // A test that writes the arena directly tells the host so by asking for it again.
        // SAFETY: As above.
        unsafe { &mut *host.arena_for_test() }.set_node_flag(row, NodeFlag::Anonymous, true);
        assert_eq!(
            host.row_identities(ScriptForcedRead::for_test()).identity_flags(row),
            NodeFlag::Anonymous as u32
        );
        assert!(
            host.rows().is_some(),
            "a write of what a row is publishes the rows again"
        );

        // SAFETY: As above.
        unsafe { &mut *host.arena_for_test() }
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
