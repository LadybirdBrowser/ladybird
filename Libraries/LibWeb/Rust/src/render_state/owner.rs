/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The render owner: the StyleLayout thread holds each document's render state.
//!
//! The states live in a map that is local to the thread, so no other thread can name one: the host names its
//! document's state by the [`DocumentId`] it minted, and reaches it only with a job it hands the thread. The thread
//! makes a state the first time a job reaches it, from what the host's [`StateSeed`] says, so a document that never
//! sends its state anything never makes one, and drops it with the job that retires it.

use super::RenderState;
use std::cell::{Cell, RefCell, UnsafeCell};
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

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
    /// The flag the state's style engine raises while it keeps any row's container effects for the host.
    pub(super) container_effects_held: Arc<AtomicBool>,
    /// The versions the state's style engine mints declaration blocks from, which the host mints from as well.
    pub(super) declaration_block_versions: Arc<AtomicU32>,
}

impl SharedWithHost {
    pub(super) fn new() -> Self {
        Self {
            declaration_block_versions: Arc::new(AtomicU32::new(1)),
            ..Self::default()
        }
    }
}

thread_local! {
    // On the owner, each document's render state and clock lanes. An entry is boxed, so a job reaching it keeps reaching
    // it while the job makes another document's state.
    static STATES: RefCell<HashMap<DocumentId, Box<DocumentEntry>>> = RefCell::new(HashMap::new());
}

/// What the owner holds of a document.
struct DocumentEntry {
    state: UnsafeCell<RenderState>,
    /// The lanes of the document's presented frames (see [`super::clock`]), which a job on them takes out meanwhile.
    lanes: Cell<Option<super::clock::LaneSlot>>,
}

/// On the owner: runs `job` on the render state of `document`, made from `seed` where no job has reached it yet.
///
/// A host callback the job makes may reach the same state again, as a layout round's read of an element's style does:
/// the map is not borrowed while the job runs, and the callback reaches the state only between the job's own uses of
/// it.
pub(super) fn with_state<R>(
    document: DocumentId,
    seed: Option<StateSeed>,
    job: impl FnOnce(&mut RenderState) -> R,
) -> R {
    let state = STATES.with_borrow_mut(|states| {
        let entry = match seed {
            Some(seed) => states.entry(document).or_insert_with(|| {
                Box::new(DocumentEntry {
                    state: UnsafeCell::new(RenderState::new(seed)),
                    lanes: Cell::default(),
                })
            }),
            None => states
                .get_mut(&document)
                .expect("a document's first job makes its render state"),
        };
        entry.state.get()
    });
    // SAFETY: The state stays boxed in the map until a job retires it, which no job of the document runs inside of.
    job(unsafe { &mut *state })
}

/// On the owner: runs `job` on the lanes of `document`, where a job made its render state. A job on them that reaches
/// them again finds none.
pub(super) fn with_lanes<R>(
    document: DocumentId,
    job: impl FnOnce(&mut Option<super::clock::LaneSlot>) -> R,
) -> Option<R> {
    let lanes = STATES.with_borrow(|states| states.get(&document).map(|entry| std::ptr::from_ref(&entry.lanes)))?;
    // SAFETY: The entry stays boxed in the map until a job retires it, which no job of the document runs inside of.
    let lanes = unsafe { &*lanes };
    let mut taken = lanes.take();
    let answer = job(&mut taken);
    lanes.set(taken);
    Some(answer)
}

/// On the owner: drops the render state of `document`, where a job made one, and its lanes.
pub(super) fn retire(document: DocumentId) {
    if let Some(entry) = STATES.with_borrow_mut(|states| states.remove(&document)) {
        let DocumentEntry { state, lanes } = *entry;
        drop(lanes);
        state.into_inner().retire();
    }
}
