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
use std::sync::atomic::{AtomicU64, Ordering};

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
    /// The layout arena, with the host tables and the layout scratch beside it.
    arena: Box<ArenaHandle>,
}

impl RenderState {
    fn new() -> Self {
        Self {
            arena: Box::new(ArenaHandle::new()),
        }
    }

    /// Drops the state, which must hold no layout node any more.
    fn retire(self) {
        let arena = self.arena.arena();
        arena.assert_owner_thread();
        assert_eq!(
            self.arena.host_tables().shells.borrow().len(),
            0,
            "layout node arena destroyed with layout nodes"
        );
        assert_eq!(
            arena.live_slot_count(),
            0,
            "layout node arena destroyed with live slots"
        );
    }
}

/// A message the host sends a document's render state.
pub(crate) enum RenderMessage {
    /// Makes the render state of a new document.
    Create { document: DocumentId },
    /// Drops the render state of a document the host has let go of.
    Destroy { document: DocumentId },
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
        RenderMessage::Create { document } => STATES.with_borrow_mut(|states| {
            let previous = states.insert(document, RenderState::new());
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
    }
}

/// Sends `message` to the render side, which handles it right here.
pub(crate) fn send(message: RenderMessage) {
    handle(message);
}

/// What the host holds of a document it created: the document's name, and the arena of its render state that the
/// host's entries still reach directly.
#[repr(C)]
pub struct FfiRenderDocument {
    pub document: DocumentId,
    pub arena: *mut c_void,
}

/// Creates the render state of a new document, and answers with its name and the arena the host's entries reach.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_create_document() -> FfiRenderDocument {
    let document = DocumentId::mint();
    send(RenderMessage::Create { document });
    // The state was made in place, on this thread, so the host may reach its arena directly until its entries go
    // through messages.
    let arena = STATES.with_borrow_mut(|states| {
        let state = states
            .get_mut(&document)
            .expect("the render state of a new document is made in place");
        std::ptr::from_mut::<ArenaHandle>(&mut state.arena).cast::<c_void>()
    });
    FfiRenderDocument { document, arena }
}

/// Drops the render state of `document`, once the host reaches its arena no more.
#[unsafe(no_mangle)]
pub extern "C" fn render_state_destroy_document(document: DocumentId) {
    send(RenderMessage::Destroy { document });
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
    fn a_created_document_has_a_render_state_until_it_is_destroyed() {
        let created = render_state_create_document();
        assert!(!created.arena.is_null());
        assert_eq!(state_count(), 1);
        render_state_destroy_document(created.document);
        assert_eq!(state_count(), 0);
    }

    #[cfg(debug_assertions)]
    #[test]
    #[should_panic(expected = "destroyed twice")]
    fn destroying_a_document_twice_is_a_senders_bug() {
        let document = DocumentId::mint();
        send(RenderMessage::Create { document });
        send(RenderMessage::Destroy { document });
        send(RenderMessage::Destroy { document });
    }
}
