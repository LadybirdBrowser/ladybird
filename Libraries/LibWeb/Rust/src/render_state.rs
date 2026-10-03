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
use crate::layout::{ArenaHandle, HostOfEntries};
use std::cell::RefCell;
use std::marker::PhantomData;
use std::ptr::NonNull;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};

mod devtools;
mod document_host;
mod questions;
mod wait;

pub use document_host::DocumentHost;
pub(crate) use questions::{ArenaAnswer, ArenaQuery, CommittedRows, Lent, PreparationPending, ask};
pub(crate) use wait::{
    LockstepProof, RenderWait, ReplyTo, ScriptForcedRead, render_state_died, wait_for_render_state, wait_from_entry,
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
    /// The layout arena, which names the document's host for the entries that still reach it directly. It links the
    /// style engine below, which outlives it.
    arena: Box<ArenaHandle>,
    /// The document's style engine, which the state owns through the handle the arena links. Every borrow of the
    /// engine comes from this one pointer, and a message borrows it mutably only where it reaches the engine alone.
    engine: StyleEngineHandle,
}

/// What the host keeps of its document's new render state.
pub(crate) struct CreatedState {
    /// Where the state keeps its arena until it is destroyed, for the host's entries that still reach it directly.
    pub(crate) arena: NonNull<ArenaHandle>,
    /// Where the state keeps its style engine until it is destroyed, for the same entries.
    pub(crate) engine: StyleEngineHandle,
    /// The flag the state raises once any element has random base values, and never lowers.
    pub(crate) element_random_base_values_exist: Arc<AtomicBool>,
}

// SAFETY: The host reaches the arena and the engine only through its unconverted entries, which run while nothing on
// the render side reaches them.
unsafe impl Send for CreatedState {}

impl RenderState {
    /// Makes the state of the document whose host is `host`, and answers what the host keeps of it.
    fn new(host: HostOfEntries, device_class: FfiDeviceClass) -> (Self, CreatedState) {
        let mut arena = Box::new(ArenaHandle::new(host));
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
}

/// A message the host sends a document's render state.
pub(crate) enum RenderMessage<'a> {
    /// Makes the render state of a new document, whose arena names the document's host.
    Create {
        document: DocumentId,
        host: HostOfEntries,
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
    },
    /// A layout round of the document the host waits for: its tree build and layout stages.
    LayoutRound {
        document: DocumentId,
        job: crate::layout::LayoutRoundJob,
        reply: ReplyTo<'a, crate::layout::LayoutRoundAnswer>,
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

/// Handles `message` on the render side.
pub(crate) fn handle(message: RenderMessage<'_>) {
    handle_message(
        &RenderingSide {
            not_send_or_sync: PhantomData,
        },
        message,
    );
}

fn handle_message(_: &RenderingSide, message: RenderMessage<'_>) {
    match message {
        RenderMessage::Create {
            document,
            host,
            device_class,
            reply,
        } => reply.answer(|| {
            let (state, created) = RenderState::new(host, device_class);
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
        RenderMessage::Style { document, job, reply } => reply.answer(|| {
            let (_, engine) = state_parts(document).expect("a document the host styles has a render state");
            // SAFETY: The state keeps the arena and the engine where they are while the message is handled, and nothing
            // else reaches them meanwhile. The host lends the job its inputs, and what they name, until it has the
            // answer.
            unsafe { job.run(engine.get_mut()) }
        }),
        RenderMessage::LayoutRound { document, job, reply } => reply.answer(|| {
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

/// Sends `message` to the render side, the StyleLayout thread, and waits until it is handled, so the message may
/// borrow from the calling frame. A message sent while the thread handles another (a child document's) is handled
/// right there, and a unit test's render states stay on the test's own thread.
pub(crate) fn send(message: RenderMessage<'_>) {
    if cfg!(test) {
        return handle(message);
    }
    crate::stage_thread::style_layout_thread().run(|| handle(message));
}

// A render state lives on a thread of its own, where nothing of the host may follow it: the shells and the callbacks
// into the host's DOM are main-thread objects because of what they hold and do, and stay with the host. The state and
// every message the host sends it may cross, which the compiler checks here.
const _: () = {
    const fn assert_send<T: Send + ?Sized>() {}
    assert_send::<RenderState>();
    assert_send::<RenderMessage>();
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
        wait_for_render_state(ScriptForcedRead::for_test(), &host, |reply| RenderMessage::Create {
            document,
            host: HostOfEntries::new(NonNull::from(&host)),
            device_class: FfiDeviceClass::ForegroundDesktop,
            reply,
        });
        send(RenderMessage::Destroy { document });
        send(RenderMessage::Destroy { document });
    }

    #[test]
    fn only_a_change_that_writes_the_rows_makes_the_host_read_them_again() {
        use crate::layout::layout_changes::LayoutChange;
        use crate::layout::node_data::{NodeFlag, NodeSlotId};
        let pointer = document_host::document_host_create(0);
        // SAFETY: The host lives until it is destroyed below.
        let host = unsafe { &*pointer };
        // SAFETY: The arena lives as long as the host's render state, and nothing else reaches it meanwhile.
        let arena = unsafe {
            &mut *host
                .arena_for_unconverted_entry()
                .cast::<crate::layout::LayoutNodeArena>()
        };
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
        let arena = unsafe {
            &mut *host
                .arena_for_unconverted_entry()
                .cast::<crate::layout::LayoutNodeArena>()
        };
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
        let arena = unsafe {
            &mut *host
                .arena_for_unconverted_entry()
                .cast::<crate::layout::LayoutNodeArena>()
        };
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
