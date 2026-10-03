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

use crate::css::style::StyleEngineHandle;
use crate::css::style::bridge::{FfiDeviceClass, create_document_style_engine};
use crate::fast_hash::FastMap as HashMap;
use crate::layout::ArenaHandle;
use std::cell::RefCell;
use std::ffi::c_void;
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::atomic::{AtomicU64, Ordering};

mod devtools;
mod document_host;
mod questions;
mod wait;

pub use document_host::DocumentHost;
pub(crate) use questions::{Answer, ArenaAnswer, ArenaQuery, LentSlice, Query, ask};
pub(crate) use wait::{LockstepProof, RenderWait, ReplyTo, ScriptForcedRead, wait_for_render_state, wait_from_entry};

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
    /// The layout arena, which names the document's host for the entries that still reach it directly. It links the
    /// style engine below, which outlives it.
    arena: Box<ArenaHandle>,
    /// The document's style engine, which the state owns through the handle the arena links. Every borrow of the
    /// engine comes from this one pointer, and a message borrows it mutably only where it reaches the engine alone.
    engine: StyleEngineHandle,
}

impl RenderState {
    fn new(host: NonNull<DocumentHost>, device_class: FfiDeviceClass) -> Self {
        let arena = Box::new(ArenaHandle::new(host));
        // SAFETY: The host outlives its document's render state.
        let host = unsafe { host.as_ref() };
        host.watch_rows_of(NonNull::from(arena.arena()));
        let engine = create_document_style_engine(device_class);
        host.watch_element_random_base_values(engine.element_random_base_values_exist());
        let engine = StyleEngineHandle::create(engine);
        arena.arena().set_style_engine(engine);
        Self { arena, engine }
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
    /// Whether applying the change can alter what the rows the render state publishes answer the host (see
    /// [`crate::layout::row_reads`]), so that a read the host makes after queuing it waits for rows that reflect it.
    /// What the next layout or paint reads, and the style engine's state, alter none.
    fn alters_published_rows(&self) -> bool {
        match self {
            Self::Layout(change) => change.alters_published_rows(),
            Self::Paint(change) => change.alters_published_rows(),
            Self::Style(_) | Self::Engine(_) => false,
        }
    }

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
}

/// A message the host sends a document's render state.
pub(crate) enum RenderMessage {
    /// Makes the render state of a new document, whose arena names the document's host.
    Create {
        document: DocumentId,
        host: NonNull<DocumentHost>,
        device_class: FfiDeviceClass,
    },
    /// Drops the render state of a document the host has let go of.
    Destroy { document: DocumentId },
    /// A write to a document's render state.
    Change { document: DocumentId, change: ArenaChange },
    /// A style transaction of the document the host waits for.
    Style {
        document: DocumentId,
        job: crate::css::style::style_job::StyleJob,
        reply: ReplyTo<crate::css::style::style_job::StyleJobAnswer>,
    },
    /// A layout stage of the document the host waits for.
    Layout {
        document: DocumentId,
        job: crate::layout::LayoutStageJob,
        reply: ReplyTo<crate::layout::LayoutStageOutput>,
    },
    /// A step of paint preparation the host waits for.
    Paint {
        document: DocumentId,
        pass: crate::painting::paint_passes::PaintPass,
        reply: ReplyTo<crate::painting::paint_passes::PaintPassAnswer>,
    },
    /// A question about a document's render state the host waits for the answer to.
    Ask {
        document: DocumentId,
        query: Query,
        reply: ReplyTo<Answer>,
    },
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
        RenderMessage::Create {
            document,
            host,
            device_class,
        } => STATES.with_borrow_mut(|states| {
            let previous = states.insert(document, RenderState::new(host, device_class));
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
            if let Some((arena, engine)) = state_parts(document) {
                // SAFETY: The state keeps the arena and the engine where they are while the message is handled, and
                // nothing else reaches them meanwhile.
                unsafe { change.apply((*arena).arena_mut(), engine) };
            }
        }
        RenderMessage::Style { document, job, reply } => reply.answer(|| {
            let (_, engine) = state_parts(document).expect("a document the host styles has a render state");
            // SAFETY: As for a change. The host lends what the job's inputs name until it has the answer.
            unsafe { job.run(engine.get_mut()) }
        }),
        RenderMessage::Layout { document, job, reply } => reply.answer(|| {
            let (arena, _) = state_parts(document).expect("a document the host lays out has a render state");
            // SAFETY: As for a change. The host keeps what the job's inputs name until it has the answer.
            job.run(unsafe { &*arena })
        }),
        RenderMessage::Paint { document, pass, reply } => reply.answer(|| {
            let (arena, _) = state_parts(document).expect("a document the host paints has a render state");
            // SAFETY: As for a change.
            pass.run(unsafe { &mut *arena }.arena_mut())
        }),
        RenderMessage::Ask { document, query, reply } => reply.answer(|| {
            let (arena, engine) = state_parts(document).expect("a document the host asks about has a render state");
            // SAFETY: As for a change.
            unsafe { query.answer((*arena).arena_mut(), engine) }
        }),
        RenderMessage::PanicForTesting { reply } => reply.answer(|| panic!("the render state panicked for a test")),
    }
}

/// The style engine of `document`'s render state, for the host's entries that still reach it directly, as
/// [`arena_for_unconverted_entry`] answers its arena.
pub(crate) fn style_engine_for_unconverted_entry(document: DocumentId) -> StyleEngineHandle {
    STATES.with_borrow(|states| {
        states
            .get(&document)
            .expect("the render state of a live document is on this thread")
            .engine
    })
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

// A render state is to move to the thread that renders, where nothing of the host may follow it: the shells and the
// callbacks into the host's DOM are main-thread objects because of what they hold and do, and stay with the host. What
// the arena shares between its rows and its caches may follow it; the arena itself still links its engine and the
// host's box presence callback, which go with the last entries that reach it directly.
const _: () = {
    const fn assert_send_and_sync<T: Send + Sync + ?Sized>() {}
    assert_send_and_sync::<std::sync::Arc<crate::css::counter_representation::CounterStyle>>();
    assert_send_and_sync::<std::sync::Arc<crate::layout::rendered_text::CachedTextChunks>>();
    assert_send_and_sync::<std::sync::Arc<[crate::layout::svg_formatting_context::FfiFloatPoint]>>();
    assert_send_and_sync::<crate::css::computed_value_types::RetainedComputedResolvedTransformList>();
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
    fn document_ids_are_minted_by_the_host() {
        let first = DocumentId::mint();
        let second = DocumentId::mint();
        assert!(first != DocumentId::default() && second != DocumentId::default());
        assert_ne!(first, second);
    }

    #[test]
    fn a_document_host_holds_a_render_state_until_it_is_destroyed() {
        let host = document_host::document_host_create(0);
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
            device_class: FfiDeviceClass::ForegroundDesktop,
        });
        send(RenderMessage::Destroy { document });
        send(RenderMessage::Destroy { document });
    }

    #[test]
    fn a_queued_change_that_alters_the_rows_makes_the_host_read_them_again() {
        use crate::layout::layout_changes::LayoutChange;
        use crate::layout::node_data::NodeSlotId;
        let pointer = document_host::document_host_create(0);
        // SAFETY: The host lives until it is destroyed below.
        let host = unsafe { &*pointer };
        assert!(host.rows().is_none());
        host.fresh_rows(ScriptForcedRead::for_test());
        assert!(host.rows().is_some());
        host.queue_change(ArenaChange::Layout(LayoutChange::SetNeedsFullLayoutTreeUpdate(true)));
        assert!(host.rows().is_some(), "a layout mark leaves the rows as they are");
        host.queue_change(ArenaChange::Layout(LayoutChange::InvalidateTextContent {
            node: NodeSlotId::INVALID,
        }));
        assert!(host.rows().is_none());
        host.fresh_rows(ScriptForcedRead::for_test());
        assert!(host.rows().is_some());
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
        let arena =
            unsafe { &mut *arena_for_unconverted_entry(host.document()).cast::<crate::layout::LayoutNodeArena>() };
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
}
