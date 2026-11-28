/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of an event target: what dispatching an event asks of every object in an event's path, whose layout is
//! defined here, and which C++ embeds in DOM::EventTarget, and event_listeners.rs keeps up to date in place.
//! https://dom.spec.whatwg.org/#interface-eventtarget

use core::cell::Cell;
use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::num::NonZeroU32;
use core::ptr::NonNull;

use libgc_api::Trace;
use libgc_api::member::write_barrier;

use super::event_listeners::EventListenerList;
use super::node::NodeCell;
use super::node::compress;
use super::node::decompress;
use super::node_class::NodeClassId;
use super::node_core::NodePointer;
use super::node_ref::NodeRef;
use super::prefix::EventTargetPrefix;
use crate::stage::MainThread;

/// A field that holds an event target or null, laid out as C++'s GC::Member<DOM::EventTarget>, which is what C++ sees it
/// as: a compressed cell pointer in 32 bits, with 0 for null.
#[repr(transparent)]
pub struct EventTargetMember(Cell<u32>);

/// A field that holds the pointer to a DOM::EventTarget::Data, laid out as C++'s OwnPtr of it, which C++ allocates and
/// frees. The data starts with the target's event listener list, which is all Rust reads of it.
#[repr(transparent)]
pub struct EventTargetDataField(UnsafeCell<*const EventListenerList>);

super::trace::member_fields!(EventTargetMember);

// SAFETY: Traces the target's event listener list. C++ visits the rest of the data.
unsafe impl Trace for EventTargetDataField {
    #[inline(always)]
    fn trace<'c>(&'c self, visitor: &mut libgc_api::Visitor<'c>) {
        // SAFETY: The field is that of a live event target, and C++ does not change it while its edges are visited.
        if let Some(list) = unsafe { self.listener_list() } {
            list.trace(visitor);
        }
    }
}

impl EventTargetDataField {
    /// The target's event listener list, if it ever needed one.
    ///
    /// # Safety
    ///
    /// The field must be that of a live event target, which C++ does not give new data while the list is borrowed.
    #[inline(always)]
    pub(super) unsafe fn listener_list(&self) -> Option<&EventListenerList> {
        // SAFETY: Guaranteed by the caller. The data starts with the list.
        unsafe { (*self.0.get()).as_ref() }
    }
}

// The flags of an event target's core, a bit each in its flags byte, which C++ sets as it constructs the target, from
// which virtual functions its class overrides, and never changes.

/// Whether the target is no node, and its class overrides get_parent(), which only then can answer anything but null.
/// https://dom.spec.whatwg.org/#get-the-parent
pub const EVENT_TARGET_HAS_GET_THE_PARENT: u8 = 1 << 0;

/// The state of an event target that dispatch reads, which C++ embeds in DOM::EventTarget as its only storage of it.
///
/// The listener type summaries say which event types the target may have listeners for: a type whose bit is clear has
/// none. The bit of a type is one of 16, picked from the type's interned identity (see DOM::event_type_summary_bit()).
/// Adding a listener sets the bit of its type, and removing one works the summaries out again from the listeners that
/// are left, see event_listeners.rs. Capturing listeners and the others are summarized apart, as only the ones of one
/// kind run in each phase.
#[derive(Trace)]
#[repr(C)]
pub struct EventTargetCore {
    /// The EVENT_TARGET_* flags.
    flags: Cell<u8>,
    /// The types of the target's listeners whose capture is true.
    capture_listener_types: Cell<u16>,
    /// The types of the target's listeners whose capture is false.
    bubble_listener_types: Cell<u16>,
}

impl EventTargetCore {
    /// The EVENT_TARGET_* flags.
    #[inline(always)]
    pub fn flags(&self) -> u8 {
        self.flags.get()
    }

    /// Whether the target may have a listener whose type has one of the bits of `types`, and whose capture is true if
    /// `capture`, and false otherwise.
    #[inline(always)]
    pub fn may_have_listeners(&self, types: u16, capture: bool) -> bool {
        let summary = match capture {
            true => self.capture_listener_types.get(),
            false => self.bubble_listener_types.get(),
        };
        summary & types != 0
    }

    /// Whether the target may have a listener whose type has one of the bits of `types`.
    #[inline(always)]
    pub fn may_have_any_listeners(&self, types: u16) -> bool {
        (self.capture_listener_types.get() | self.bubble_listener_types.get()) & types != 0
    }

    #[inline(always)]
    pub(super) fn capture_listener_types(&self) -> u16 {
        self.capture_listener_types.get()
    }

    #[inline(always)]
    pub(super) fn bubble_listener_types(&self) -> u16 {
        self.bubble_listener_types.get()
    }

    /// Sets the listener type summaries, which event_listeners.rs keeps up to date as the target's listeners change.
    #[inline(always)]
    pub(super) fn set_listener_types(&self, capture_listener_types: u16, bubble_listener_types: u16) {
        self.capture_listener_types.set(capture_listener_types);
        self.bubble_listener_types.set(bubble_listener_types);
    }
}

/// A DOM::EventTarget: the start of the target's cell, which starts with an EventTargetPrefix. FFI entry points take
/// event targets as references to it, which C++ sees as DOM::EventTarget pointers.
#[repr(C)]
pub struct EventTargetCell {
    // NB: The cell's state changes behind shared references to it, see NodeCell.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which Rust never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

/// An event target held by the code that dispatches an event, valid across allocation and script for as long as it is
/// held on the stack, as a NodeRef is: it holds the start of the target's cell, which the conservative stack scan
/// recognizes, and reads the target's core anew on every access. It is laid out as a GC::Ref<DOM::EventTarget>.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct EventTargetRef {
    // NB: NonNull makes the reference neither Send nor Sync.
    cell: NonNull<EventTargetCell>,
}

impl From<NodeRef> for EventTargetRef {
    /// The event target a node is, which starts with its event target.
    #[inline(always)]
    fn from(node: NodeRef) -> Self {
        Self {
            cell: NonNull::from(node.cell()).cast(),
        }
    }
}

impl EventTargetRef {
    /// The event target `cell` is, as an FFI entry point that holds a main thread token is handed it.
    ///
    /// # Safety
    ///
    /// `cell` must be a live event target, which the caller keeps alive for as long as the reference is held.
    #[inline(always)]
    pub(crate) unsafe fn from_cell(cell: &EventTargetCell, _main_thread: &MainThread<'_>) -> Self {
        Self {
            cell: NonNull::from(cell),
        }
    }

    /// The event target a field holds, or none if it is null.
    ///
    /// # Safety
    ///
    /// `member` must be a field of a live cell that keeps the target it names alive.
    #[inline(always)]
    pub(super) unsafe fn from_member(member: &EventTargetMember) -> Option<Self> {
        let compressed = NonZeroU32::new(member.0.get())?;
        Some(Self {
            cell: decompress(NodePointer::from_compressed(compressed.get())).cast(),
        })
    }

    /// The start of the cell, as an opaque value the compiler has to keep the held address live for.
    #[inline(always)]
    fn pinned_cell(&self) -> *const EventTargetCell {
        let mut address = self.cell.as_ptr() as usize;
        // SAFETY: The assembly is empty, as in NodeRef::pinned_cell().
        unsafe {
            core::arch::asm!(
                "/* {address} */",
                address = inout(reg) address,
                options(nomem, nostack, preserves_flags),
            );
        }
        core::ptr::with_exposed_provenance(address)
    }

    /// The class of the node the target is, or NodeClassId::NotANode if it is no node.
    #[inline(always)]
    pub(super) fn class_id(&self) -> NodeClassId {
        // SAFETY: The target is alive for as long as the reference is held.
        unsafe { NodeClassId::of_cell(self.pinned_cell()) }
    }

    /// The start of the target, read for as long as the reference is borrowed, during which nothing may call into C++.
    #[inline(always)]
    pub(super) fn prefix(&self) -> &EventTargetPrefix {
        // SAFETY: Every event target starts with an EventTargetPrefix, which EventTarget.cpp asserts, and the target is
        //         alive for as long as the reference is held.
        unsafe { &*self.pinned_cell().cast::<EventTargetPrefix>() }
    }

    /// The target's core, read for as long as the reference is borrowed, during which nothing may call into C++.
    #[inline(always)]
    pub(super) fn core(&self) -> &EventTargetCore {
        &self.prefix().core
    }

    /// The node the target is, or none if it is no node.
    #[inline(always)]
    pub(super) fn as_node(self) -> Option<NodeRef> {
        if self.class_id() == NodeClassId::NotANode {
            return None;
        }
        // SAFETY: A node starts with its event target, and the target is alive for as long as the reference is held.
        Some(unsafe { NodeRef::from_cell_unchecked(self.cell.cast::<NodeCell>()) })
    }

    /// The start of the target's cell, as C++ takes it.
    #[inline(always)]
    pub(super) fn as_ptr(self) -> *const EventTargetCell {
        self.cell.as_ptr()
    }

    /// The compressed pointer that names the target, as GC::Member fields hold it.
    #[inline(always)]
    fn compressed(self) -> u32 {
        // SAFETY: The target is alive for as long as the reference is held.
        compress(unsafe { self.cell.cast::<NodeCell>().as_ref() }).compressed()
    }
}

impl EventTargetMember {
    /// A field that holds `target`, or null.
    #[inline(always)]
    pub(super) fn new(target: Option<EventTargetRef>) -> Self {
        Self(Cell::new(target.map_or(0, EventTargetRef::compressed)))
    }

    /// Stores `target`, or null, as GC::Member's assignment does, through the write barrier hook.
    #[inline(always)]
    pub(super) fn set(&self, target: Option<EventTargetRef>) {
        let compressed = target.map_or(0, EventTargetRef::compressed);
        write_barrier(self.0.as_ptr(), compressed);
        self.0.set(compressed);
    }

    #[inline(always)]
    pub(super) fn is_some(&self) -> bool {
        self.0.get() != 0
    }
}

#[cfg(test)]
impl EventTargetMember {
    pub(crate) fn null_for_test() -> Self {
        Self(Cell::new(0))
    }
}

#[cfg(test)]
impl EventTargetCore {
    pub(crate) fn null_for_test() -> Self {
        Self {
            flags: Cell::new(0),
            capture_listener_types: Cell::new(0),
            bubble_listener_types: Cell::new(0),
        }
    }
}

#[cfg(test)]
impl EventTargetDataField {
    pub(crate) fn null_for_test() -> Self {
        Self(UnsafeCell::new(core::ptr::null()))
    }
}

/// An event target or null, as C++ hands one over: laid out as a GC::Ptr<DOM::EventTarget>.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct NullableEventTargetRef(Option<EventTargetRef>);

/// An event target, as a function that crosses to C++ takes or returns it: C++ sees it as a raw pointer, which every
/// ABI passes and returns as C does, where a GC::Ref is a class that MSVC returns through a hidden pointer.
pub type RawEventTargetRef = EventTargetRef;

/// An event target or null, as a function that crosses to C++ takes or returns it: C++ sees it as a raw pointer, which
/// every ABI passes and returns as C does, where a GC::Ptr is a class that MSVC returns through a hidden pointer.
pub type RawNullableEventTargetRef = NullableEventTargetRef;

impl From<Option<EventTargetRef>> for NullableEventTargetRef {
    #[inline(always)]
    fn from(target: Option<EventTargetRef>) -> Self {
        Self(target)
    }
}

impl From<NullableEventTargetRef> for Option<EventTargetRef> {
    #[inline(always)]
    fn from(target: NullableEventTargetRef) -> Self {
        target.0
    }
}

const _: () = assert!(size_of::<EventTargetMember>() == 4 && align_of::<EventTargetMember>() == 4);
// NB: EventTarget.cpp asserts the same layout of the struct it includes.
const _: () = assert!(size_of::<EventTargetDataField>() == 8 && align_of::<EventTargetDataField>() == 8);
