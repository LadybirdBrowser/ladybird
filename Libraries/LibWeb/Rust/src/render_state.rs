/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Each document's render state, named by a [`DocumentId`] and reached only through messages.
//!
//! A document's [`RenderState`] is what its style, layout and paint preparation compute over: its layout arena and
//! what lives beside it. The host holds the document's id and reaches the state by sending a [`RenderMessage`]
//! through [`send`], which handles it in place on the calling thread. Only [`handle`] mints the [`RenderingSide`]
//! that the states are reached with, so code that is not handed one cannot reach a document's render state.

use crate::fast_hash::FastMap as HashMap;
use crate::layout::ArenaHandle;
use std::cell::RefCell;
use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};

mod devtools;
mod document_host;
mod wait;

pub use document_host::DocumentHost;
pub(crate) use wait::{ReplyTo, ScriptForcedRead, wait_for_render_state};

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
    /// The layout arena, which names the document's host for the entries that still reach it directly.
    arena: Box<ArenaHandle>,
}

impl RenderState {
    fn new(host: NonNull<DocumentHost>) -> Self {
        Self {
            arena: Box::new(ArenaHandle::new(host)),
        }
    }

    /// Drops the state, which must hold no layout node any more.
    fn retire(self) {
        let arena = self.arena.arena();
        arena.assert_owner_thread();
        assert_eq!(
            arena.live_slot_count(),
            0,
            "layout node arena destroyed with live slots"
        );
    }
}

/// A write the host makes to a document's render state, as owned data the state applies in the order the host made
/// it, before anything that reads what it changes.
pub(crate) enum ArenaChange {
    /// A write to the document's layout marks or layout facts.
    Layout(crate::layout::layout_changes::LayoutChange),
}

impl ArenaChange {
    fn apply(self, arena: &mut crate::layout::LayoutNodeArena) {
        match self {
            Self::Layout(change) => change.apply(arena),
        }
    }
}

/// A message the host sends a document's render state.
pub(crate) enum RenderMessage {
    /// Makes the render state of a new document, whose arena names the document's host.
    Create {
        document: DocumentId,
        host: NonNull<DocumentHost>,
    },
    /// Drops the render state of a document the host has let go of.
    Destroy { document: DocumentId },
    /// A write to a document's render state.
    Change { document: DocumentId, change: ArenaChange },
    /// Panics answering, for a test that the host waiting for the answer crashes.
    PanicForTesting { reply: ReplyTo<()> },
}

thread_local! {
    // The render state of each document, on the thread that handles render messages.
    static STATES: RefCell<HashMap<DocumentId, RenderState>> = RefCell::default();
}

/// Handles `message` on the render side.
pub(crate) fn handle(message: RenderMessage) {
    handle_message(
        &RenderingSide {
            not_send_or_sync: PhantomData,
        },
        message,
    );
}

fn handle_message(_: &RenderingSide, message: RenderMessage) {
    match message {
        RenderMessage::Create { document, host } => STATES.with_borrow_mut(|states| {
            let previous = states.insert(document, RenderState::new(host));
            debug_assert!(previous.is_none(), "document {document:?} created twice");
        }),
        RenderMessage::Destroy { document } => {
            let state = STATES.with_borrow_mut(|states| states.remove(&document));
            // A document with no state is a bug of the sender's, which leaves nothing to drop.
            debug_assert!(state.is_some(), "document {document:?} destroyed twice");
            if let Some(state) = state {
                state.retire();
            }
        }
        RenderMessage::Change { document, change } => {
            // A document with no state is a bug of the sender's, whose change has nothing to change.
            if let Some(arena) = state_arena(document) {
                // SAFETY: The state's box keeps the arena where it is while the message is handled, and nothing else
                // reaches it meanwhile.
                change.apply(unsafe { &mut *arena }.arena_mut());
            }
        }
        RenderMessage::PanicForTesting { reply } => reply.answer(|| panic!("the render state panicked for a test")),
    }
}

/// The arena of `document`'s render state, which stays where it is until the state is destroyed. The map is not
/// borrowed while a message reaches the arena, so a message handled meanwhile for another document finds its own.
fn state_arena(document: DocumentId) -> Option<*mut ArenaHandle> {
    STATES.with_borrow_mut(|states| {
        let state = states.get_mut(&document);
        debug_assert!(state.is_some(), "document {document:?} has no render state");
        state.map(|state| std::ptr::from_mut::<ArenaHandle>(&mut state.arena))
    })
}

/// Sends `message` to the render side, which handles it right here.
pub(crate) fn send(message: RenderMessage) {
    handle(message);
}

/// The arena of `document`'s render state, for the host's entries that still reach it directly.
///
/// This is the one door from the host into a render state that does not go through a message; every use of it is an
/// entry that has not been converted yet. The state is made in place, on the host's thread, so the arena stays at the
/// address answered until the document is destroyed.
pub(crate) fn arena_for_unconverted_entry(document: DocumentId) -> *mut c_void {
    STATES.with_borrow_mut(|states| {
        let state = states
            .get_mut(&document)
            .expect("the render state of a live document is on this thread");
        std::ptr::from_mut::<ArenaHandle>(&mut state.arena).cast::<c_void>()
    })
}

#[cfg(test)]
mod tests {
    use super::*;

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
    fn document_ids_are_minted_by_the_host() {
        let first = DocumentId::mint();
        let second = DocumentId::mint();
        assert!(first != DocumentId::default() && second != DocumentId::default());
        assert_ne!(first, second);
    }

    #[test]
    fn a_document_host_holds_a_render_state_until_it_is_destroyed() {
        let host = document_host::document_host_create();
        assert_eq!(state_count(), 1);
        // SAFETY: The host came from document_host_create and is destroyed once, below.
        assert!(!unsafe { document_host::render_state_arena_for_unconverted_entry(host) }.is_null());
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
        send(RenderMessage::Create {
            document,
            host: NonNull::from(&host),
        });
        send(RenderMessage::Destroy { document });
        send(RenderMessage::Destroy { document });
    }
}
