/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The render owner: whichever thread holds the slot every document's render state lives in.
//!
//! The Render thread holds the slot for the jobs it runs, and a host's thread holds it for a job it runs itself,
//! while it waits for nothing the Render thread runs (see [`super::on_render_side`]). A thread holds the slot only
//! for the length of a job, never between them, so no thread waits for another to run script. The host names its
//! document's state by the [`DocumentId`] it minted, and reaches it only with a job. The owner makes a state the first
//! time a job reaches it, from what the host's [`StateSeed`] says, so a document that never sends its state anything
//! never makes one, and drops it with the job that retires it.

use super::RenderState;
use std::cell::{Cell, RefCell, UnsafeCell};
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{Condvar, LazyLock, Mutex, PoisonError};

/// The host's name for its document's render state, which it mints without asking the owner.
#[derive(Clone, Copy, PartialEq, Eq, Hash, Debug)]
pub(crate) struct DocumentId(u64);

impl DocumentId {
    pub(super) fn mint() -> Self {
        static NEXT: AtomicU64 = AtomicU64::new(1);
        Self(NEXT.fetch_add(1, Ordering::Relaxed))
    }
}

/// What the owner makes a document's render state from, which the host's first job hands it.
pub(crate) struct StateSeed {
    pub(super) shared: SharedWithHost,
}

/// What a document's render state shares with its host, which the host reads, or mints from, without asking the owner.
#[derive(Clone, Default)]
pub(crate) struct SharedWithHost {
    /// The flag the state's style engine raises once any element has random base values, and never lowers.
    pub(super) element_random_base_values_exist: Arc<AtomicBool>,
    /// The flag the state's arena raises while any row has enrolled an SVG paint resource.
    pub(super) svg_paint_resources_enrolled: Arc<AtomicBool>,
    /// The flag the state's arena raises once any `::first-letter` box takes a style derived from the element
    /// containing its letter, and never lowers.
    pub(super) first_letter_styles_derived: Arc<AtomicBool>,
    /// The flag the state's style engine raises while it keeps any row's container effects for the host.
    pub(super) container_effects_held: Arc<AtomicBool>,
    /// The versions the state's style engine mints declaration blocks from, which the host mints from as well.
    pub(super) declaration_block_versions: Arc<AtomicU32>,
    /// The depth of the deepest element the state's style tree ever held.
    pub(super) deepest_element: Arc<AtomicU32>,
    /// How many elements' links the state's style engine wrote since it last released a transaction, each of which may
    /// deepen the tree by a level until the tree settles them.
    pub(super) link_rows_written: Arc<AtomicU32>,
    /// The style atoms of the process, which the host interns names in beside the state's style engine.
    pub(super) style_atoms: Arc<crate::css::style::ProcessAtoms>,
}

impl SharedWithHost {
    pub(super) fn new() -> Self {
        Self {
            declaration_block_versions: Arc::new(AtomicU32::new(1)),
            style_atoms: Arc::clone(SLOT.process_style.atoms()),
            ..Self::default()
        }
    }
}

/// The turns threads take at the slot, in the order they ask for them. A thread that holds the slot over and over, as
/// the ticks of a clock that falls behind do, would otherwise take it ahead of a thread waiting to wake, for as long as
/// it goes on.
#[derive(Default)]
struct Turns {
    next: AtomicU64,
    serving: AtomicU64,
    waiting: Mutex<()>,
    turn_ended: Condvar,
}

impl Turns {
    fn take(&self) {
        // NB: These operations are sequentially consistent, or this thread could read the turn as not yet its own while
        //     the holder ending it reads no ticket past its own, and wakes nobody.
        let ticket = self.next.fetch_add(1, Ordering::SeqCst);
        if self.serving.load(Ordering::SeqCst) == ticket {
            return;
        }
        let mut waiting = self.waiting.lock().unwrap_or_else(PoisonError::into_inner);
        while self.serving.load(Ordering::SeqCst) != ticket {
            waiting = self.turn_ended.wait(waiting).unwrap_or_else(PoisonError::into_inner);
        }
    }

    fn end(&self) {
        let serving = self.serving.fetch_add(1, Ordering::SeqCst) + 1;
        if self.next.load(Ordering::SeqCst) != serving {
            // NB: A waiter holds the lock from reading the turn until it sleeps, so this reaches it.
            drop(self.waiting.lock().unwrap_or_else(PoisonError::into_inner));
            self.turn_ended.notify_all();
        }
    }
}

/// The slot every document's render state lives in, which one thread holds at a time.
struct Slot {
    held: Turns,
    /// Each document's render state and clock lanes, which only the thread that holds the slot reaches. An entry is
    /// boxed, so a job reaching it keeps reaching it while the job makes another document's state.
    states: RefCell<HashMap<DocumentId, Box<DocumentEntry>>>,
    /// The style data every document's style engine shares with the others.
    process_style: Arc<crate::css::style::ProcessStyle>,
    /// What names the slot to ThreadSanitizer, which sees the ordering holding it gives only through it.
    tsan_key: AtomicBool,
    /// The threads the style and layout passes of every document run on besides the owner's.
    style_layout_pool: Arc<crate::style_layout_pool::StyleLayoutPool>,
}

// SAFETY: Only the thread that holds the slot reaches its states, which may cross threads, and holding it orders what
// one holder wrote before what the next one reads.
unsafe impl Sync for Slot {}

// What the slot holds moves from one thread to another with it, which the compiler checks here.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<RenderState>();
    assert_send::<super::clock::LaneSlot>();
};

static SLOT: LazyLock<Slot> = LazyLock::new(|| Slot {
    held: Turns::default(),
    states: RefCell::new(HashMap::new()),
    process_style: Arc::default(),
    tsan_key: AtomicBool::new(false),
    style_layout_pool: Arc::new(crate::style_layout_pool::StyleLayoutPool::of_process()),
});

/// Has the style and layout threads of the process give back the memory they freed, which they keep until they
/// allocate again. Each does so between its jobs, and this does not wait for them. The Paint and Render threads both
/// ask for this when they go idle, as either can stay busy for long while the workers have nothing to do.
#[cfg(feature = "allocator")]
pub(crate) fn release_unused_memory_of_style_layout_threads() {
    if let Some(pool) = SLOT.style_layout_pool.started() {
        pool.spawn_broadcast(|_| ak::release_unused_allocator_memory());
    }
}

/// Standalone Rust tests do not link the C++ allocator, so there is nothing to release.
#[cfg(not(feature = "allocator"))]
pub(crate) fn release_unused_memory_of_style_layout_threads() {}

/// The style data every document's style engine shares with the others, which any thread reaches without the slot: only
/// what it guards itself is written once it is made.
pub(crate) fn process_style() -> &'static Arc<crate::css::style::ProcessStyle> {
    &SLOT.process_style
}

thread_local! {
    /// How many holds of the slot the calling thread is in, which a host callback of a job takes again.
    static HOLDS: Cell<u32> = const { Cell::new(0) };
    /// What the calling thread let go of while it held the slot, which it drops once it no longer holds it.
    static DROPPED_AFTER_HOLD: RefCell<Vec<Box<dyn std::any::Any>>> = const { RefCell::new(Vec::new()) };
}

/// Drops `value`, which the calling thread let go of while it holds the slot, once its outermost hold ends: a value
/// that takes long to drop, such as a fork of a whole render state, does not keep the threads waiting for the slot.
pub(super) fn drop_after_hold(value: impl std::any::Any) {
    debug_assert!(is_held_here(), "only a thread holding the slot defers a drop past it");
    DROPPED_AFTER_HOLD.with_borrow_mut(|values| values.push(Box::new(value)));
}

/// Whether the calling thread holds the slot: it runs a job on the render states, or a host callback of one.
pub(super) fn is_held_here() -> bool {
    HOLDS.get() > 0
}

/// Runs `job` holding the slot, which the calling thread takes first where it does not hold it yet, waiting for
/// whichever thread holds it to let go.
pub(super) fn hold<R>(job: impl FnOnce() -> R) -> R {
    struct Holding {
        took_the_slot: bool,
    }
    impl Drop for Holding {
        fn drop(&mut self) {
            HOLDS.set(HOLDS.get() - 1);
            if !self.took_the_slot {
                return;
            }
            crate::stage_thread::tsan::release(&SLOT.tsan_key);
            SLOT.held.end();
            // NB: Taken out before dropping them, since a drop may hold the slot again.
            let dropped = DROPPED_AFTER_HOLD.take();
            // A lane's drop resumes the panic of the recording it waits for, which a thread that is already unwinding
            // cannot survive: such a thread leaks what it let go of instead.
            if std::thread::panicking() {
                std::mem::forget(dropped);
            } else {
                drop(dropped);
            }
        }
    }
    let took_the_slot = !is_held_here();
    if took_the_slot {
        SLOT.held.take();
        crate::stage_thread::tsan::acquire(&SLOT.tsan_key);
    }
    HOLDS.set(HOLDS.get() + 1);
    let _holding = Holding { took_the_slot };
    job()
}

/// Runs `job` on the render states, holding the slot, without keeping them borrowed past it.
fn with_states<R>(job: impl FnOnce(&mut HashMap<DocumentId, Box<DocumentEntry>>) -> R) -> R {
    debug_assert!(
        is_held_here(),
        "only the thread that holds the slot reaches the render states"
    );
    job(&mut SLOT.states.borrow_mut())
}

/// What the owner holds of a document.
struct DocumentEntry {
    state: UnsafeCell<RenderState>,
    /// The lanes of the document's presented frames (see [`super::clock`]), which a job on them takes out meanwhile.
    lanes: Cell<Option<super::clock::LaneSlot>>,
    /// Whether a job runs on the state, which hands the job the only reference to it.
    job_runs_on_state: Cell<bool>,
}

/// On the owner: runs `job` on the render state of `document`, made from `seed` where no job has reached it yet.
///
/// The job holds the only reference to the state while it runs: nothing the job calls reaches the state of its own
/// document again, though a job may run one on another document's state, as a dump of a nested document does.
pub(super) fn with_state<R>(
    document: DocumentId,
    seed: Option<StateSeed>,
    job: impl FnOnce(&mut RenderState) -> R,
) -> R {
    hold(|| {
        let state = with_states(|states| {
            let entry = match seed {
                Some(seed) => states.entry(document).or_insert_with(|| {
                    Box::new(DocumentEntry {
                        state: UnsafeCell::new(RenderState::new(seed, &SLOT.process_style, &SLOT.style_layout_pool)),
                        lanes: Cell::default(),
                        job_runs_on_state: Cell::new(false),
                    })
                }),
                None => states
                    .get_mut(&document)
                    .expect("a document's first job makes its render state"),
            };
            (entry.state.get(), std::ptr::from_ref(&entry.job_runs_on_state))
        });
        // SAFETY: The entry stays boxed in the map until a job retires it, which no job of the document runs inside of.
        let job_runs_on_state = unsafe { &*state.1 };
        assert!(
            !job_runs_on_state.replace(true),
            "a job reached the render state of its own document again"
        );
        struct JobEnds<'a>(&'a Cell<bool>);
        impl Drop for JobEnds<'_> {
            fn drop(&mut self) {
                self.0.set(false);
            }
        }
        let _job_ends = JobEnds(job_runs_on_state);
        // SAFETY: As above, and the assertion makes this the only reference to the state while the job runs.
        job(unsafe { &mut *state.0 })
    })
}

/// On the owner: runs `job` on the lanes of `document`, where a job made its render state. A job on them that reaches
/// them again finds none.
pub(super) fn with_lanes<R>(
    document: DocumentId,
    job: impl FnOnce(&mut Option<super::clock::LaneSlot>) -> R,
) -> Option<R> {
    hold(|| {
        let lanes = with_states(|states| states.get(&document).map(|entry| std::ptr::from_ref(&entry.lanes)))?;
        // SAFETY: The entry stays boxed in the map until a job retires it, which no job of the document runs inside of.
        let lanes = unsafe { &*lanes };
        let mut taken = lanes.take();
        let answer = job(&mut taken);
        lanes.set(taken);
        Some(answer)
    })
}

/// On the owner: drops the render state of `document`, where a job made one, and its lanes.
pub(super) fn retire(document: DocumentId) {
    hold(|| {
        if let Some(entry) = with_states(|states| states.remove(&document)) {
            let DocumentEntry { state, lanes, .. } = *entry;
            super::clock::retire_slot(lanes.into_inner());
            state.into_inner().retire();
        }
    });
}
