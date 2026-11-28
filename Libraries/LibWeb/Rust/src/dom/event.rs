/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The core of an event: the flags, targets and path that dispatching an event reads and writes, whose layout is
//! defined here, and which C++ embeds in DOM::Event and reads and writes in place.
//! https://dom.spec.whatwg.org/#interface-event

use core::alloc::Layout;
use core::cell::Cell;
use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::mem::offset_of;
use core::ptr::NonNull;

use super::event_target::EventTargetMember;
use super::event_target::EventTargetRef;
use crate::stage::MainThread;

/// Where DOM::Event embeds its core, in bytes from the start of the event's cell, which Event.cpp asserts.
pub const EVENT_CORE_OFFSET: usize = 32;

/// The values of the eventPhase attribute, which C++ names DOM::Event::Phase.
/// https://dom.spec.whatwg.org/#dom-event-eventphase
pub const EVENT_PHASE_NONE: u8 = 0;
pub const EVENT_PHASE_CAPTURING: u8 = 1;
pub const EVENT_PHASE_AT_TARGET: u8 = 2;
pub const EVENT_PHASE_BUBBLING: u8 = 3;

/// A DOM::Event as C++ allocates it: the start of the event's cell, with the event's core EVENT_CORE_OFFSET bytes into
/// it. FFI entry points take events as references to it, which C++ sees as DOM::Event pointers.
#[repr(C)]
pub struct EventCell {
    // NB: The cell's state changes behind shared references to it, see NodeCell.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which Rust never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

/// An item of an event's path, laid out as C++ reads it, which only Rust writes.
/// https://dom.spec.whatwg.org/#event-path-item
#[repr(C)]
pub struct EventPathItem {
    /// https://dom.spec.whatwg.org/#event-path-item-invocation-target
    invocation_target: EventTargetMember,
    /// https://dom.spec.whatwg.org/#event-path-item-shadow-adjusted-target
    shadow_adjusted_target: EventTargetMember,
    /// https://dom.spec.whatwg.org/#event-path-item-relatedtarget
    related_target: EventTargetMember,
    /// https://dom.spec.whatwg.org/#event-path-item-invocation-target-in-shadow-tree
    invocation_target_in_shadow_tree: Cell<bool>,
    /// https://dom.spec.whatwg.org/#event-path-item-root-of-closed-tree
    root_of_closed_tree: Cell<bool>,
    /// https://dom.spec.whatwg.org/#event-path-item-slot-in-closed-tree
    slot_in_closed_tree: Cell<bool>,
}

impl EventPathItem {
    #[inline(always)]
    pub(super) fn new(
        invocation_target: EventTargetRef,
        shadow_adjusted_target: Option<EventTargetRef>,
        related_target: Option<EventTargetRef>,
        invocation_target_in_shadow_tree: bool,
        root_of_closed_tree: bool,
        slot_in_closed_tree: bool,
    ) -> Self {
        Self {
            invocation_target: EventTargetMember::new(Some(invocation_target)),
            shadow_adjusted_target: EventTargetMember::new(shadow_adjusted_target),
            related_target: EventTargetMember::new(related_target),
            invocation_target_in_shadow_tree: Cell::new(invocation_target_in_shadow_tree),
            root_of_closed_tree: Cell::new(root_of_closed_tree),
            slot_in_closed_tree: Cell::new(slot_in_closed_tree),
        }
    }

    /// # Safety
    ///
    /// The path that holds the item must keep the targets it names alive, which it does for as long as its event does.
    #[inline(always)]
    pub(super) unsafe fn invocation_target(&self) -> EventTargetRef {
        // SAFETY: Guaranteed by the caller, and an item always has an invocation target.
        unsafe { EventTargetRef::from_member(&self.invocation_target) }.expect("an event path item has a target")
    }

    /// # Safety
    ///
    /// See [`EventPathItem::invocation_target`].
    #[inline(always)]
    pub(super) unsafe fn shadow_adjusted_target(&self) -> Option<EventTargetRef> {
        // SAFETY: Guaranteed by the caller.
        unsafe { EventTargetRef::from_member(&self.shadow_adjusted_target) }
    }

    #[inline(always)]
    pub(super) fn has_shadow_adjusted_target(&self) -> bool {
        self.shadow_adjusted_target.is_some()
    }

    /// # Safety
    ///
    /// See [`EventPathItem::invocation_target`].
    #[inline(always)]
    pub(super) unsafe fn related_target(&self) -> Option<EventTargetRef> {
        // SAFETY: Guaranteed by the caller.
        unsafe { EventTargetRef::from_member(&self.related_target) }
    }

    #[inline(always)]
    pub(super) fn invocation_target_in_shadow_tree(&self) -> bool {
        self.invocation_target_in_shadow_tree.get()
    }
}

/// The path of an event, as its core holds it: its items, and the touch target list of each, in buffers that Rust
/// allocates as dispatch appends to the path and frees as it empties it, and that C++ visits in place. Each item has
/// the same number of touch targets, as many as the event's touch target list had as dispatch began, and the touch
/// targets of item `i` are `touch_target_count` after one another, from `i * touch_target_count` on.
/// https://dom.spec.whatwg.org/#event-path
#[repr(C)]
pub struct EventPath {
    items: Cell<*mut EventPathItem>,
    size: Cell<u32>,
    capacity: Cell<u32>,
    touch_targets: Cell<*mut EventTargetMember>,
    touch_target_count: Cell<u32>,
}

impl EventPath {
    /// The items, as they are when asked, for as long as the path is borrowed, during which nothing may call into C++.
    #[inline(always)]
    pub(super) fn items(&self) -> &[EventPathItem] {
        // SAFETY: The buffer holds `size` items, which only Rust writes, and frees only as it empties the path.
        unsafe { ak::slice_from_raw_parts(self.items.get(), self.size.get() as usize) }
    }

    /// The item at `index`, if the path still has that many, which an event dispatched again from one of its own
    /// listeners by C++ may not.
    #[inline(always)]
    pub(super) fn item(&self, index: usize) -> Option<&EventPathItem> {
        self.items().get(index)
    }

    #[inline(always)]
    pub(super) fn touch_target_count(&self) -> usize {
        self.touch_target_count.get() as usize
    }

    /// The touch target list of the item at `index`.
    pub(super) fn touch_targets(&self, index: usize) -> &[EventTargetMember] {
        let count = self.touch_target_count();
        if count == 0 || index >= self.items().len() {
            return &[];
        }
        // SAFETY: The buffer holds `count` touch targets for each of the `size` items.
        unsafe { ak::slice_from_raw_parts(self.touch_targets.get().add(index * count), count) }
    }

    /// Starts a path whose items each have `touch_target_count` touch targets.
    pub(super) fn begin(&self, touch_target_count: usize) {
        assert!(self.size.get() == 0, "an event's path is empty as its dispatch begins");
        self.touch_target_count
            .set(u32::try_from(touch_target_count).expect("a touch target list is never that long"));
    }

    /// Appends an item, and its touch target list, whose length is the path's touch target count.
    pub(super) fn append(
        &self,
        item: EventPathItem,
        touch_targets: impl ExactSizeIterator<Item = Option<EventTargetRef>>,
    ) {
        let size = self.size.get() as usize;
        if size == self.capacity.get() as usize {
            self.grow();
        }
        let count = self.touch_target_count();
        assert_eq!(touch_targets.len(), count);
        // SAFETY: The buffers have room for the item and its touch targets past the `size` items it holds.
        unsafe {
            self.items.get().add(size).write(item);
            for (index, touch_target) in touch_targets.enumerate() {
                self.touch_targets
                    .get()
                    .add(size * count + index)
                    .write(EventTargetMember::new(touch_target));
            }
        }
        // NB: The item counts, and so C++ visits it, only once it is written.
        self.size.set(size as u32 + 1);
    }

    #[cold]
    fn grow(&self) {
        let capacity = self.capacity.get() as usize;
        let new_capacity = if capacity == 0 { 16 } else { capacity * 2 };
        let count = self.touch_target_count();
        // SAFETY: The buffers were allocated with the layouts of `capacity` items, and `capacity * count` touch targets,
        //         or are null where `capacity` is 0. Neither layout is zero-sized.
        unsafe {
            self.items.set(reallocate(self.items.get(), capacity, new_capacity));
            if count != 0 {
                self.touch_targets.set(reallocate(
                    self.touch_targets.get(),
                    capacity * count,
                    new_capacity * count,
                ));
            }
        }
        self.capacity
            .set(u32::try_from(new_capacity).expect("an event path is never that long"));
    }

    /// Empties the path, and frees its buffers.
    pub(super) fn clear(&self) {
        let capacity = self.capacity.get() as usize;
        let count = self.touch_target_count();
        self.size.set(0);
        self.capacity.set(0);
        self.touch_target_count.set(0);
        // SAFETY: The buffers were allocated with these layouts, and nothing reads them now that the path is empty.
        unsafe {
            if capacity != 0 {
                std::alloc::dealloc(
                    self.items.get().cast(),
                    Layout::array::<EventPathItem>(capacity).unwrap(),
                );
                if count != 0 {
                    std::alloc::dealloc(
                        self.touch_targets.get().cast(),
                        Layout::array::<EventTargetMember>(capacity * count).unwrap(),
                    );
                }
            }
        }
        self.items.set(core::ptr::null_mut());
        self.touch_targets.set(core::ptr::null_mut());
    }
}

/// Moves `old_count` values of `T` at `buffer`, allocated for that many, or null where it is 0, to a new buffer for
/// `new_count`.
///
/// # Safety
///
/// `buffer` must have been allocated by the global allocator with the layout of `old_count` values of `T`, or be null
/// where `old_count` is 0, and `new_count` must be greater than `old_count`.
unsafe fn reallocate<T>(buffer: *mut T, old_count: usize, new_count: usize) -> *mut T {
    let new_layout = Layout::array::<T>(new_count).expect("an event path buffer fits in memory");
    // SAFETY: Guaranteed by the caller.
    let new_buffer = unsafe {
        match old_count {
            0 => std::alloc::alloc(new_layout),
            _ => std::alloc::realloc(buffer.cast(), Layout::array::<T>(old_count).unwrap(), new_layout.size()),
        }
    };
    if new_buffer.is_null() {
        std::alloc::handle_alloc_error(new_layout);
    }
    new_buffer.cast()
}

/// The state of an event that dispatch reads and writes, which C++ embeds in DOM::Event as its only storage of it.
/// https://dom.spec.whatwg.org/#concept-event
#[repr(C)]
pub struct EventCore {
    /// https://dom.spec.whatwg.org/#event-target
    target: EventTargetMember,
    /// https://dom.spec.whatwg.org/#event-relatedtarget
    related_target: EventTargetMember,
    /// https://dom.spec.whatwg.org/#dom-event-currenttarget
    current_target: EventTargetMember,
    /// One of the EVENT_PHASE_* values.
    /// https://dom.spec.whatwg.org/#dom-event-eventphase
    event_phase: Cell<u8>,
    /// https://dom.spec.whatwg.org/#dom-event-bubbles
    bubbles: Cell<bool>,
    /// https://dom.spec.whatwg.org/#dom-event-cancelable
    cancelable: Cell<bool>,
    /// https://dom.spec.whatwg.org/#stop-propagation-flag
    stop_propagation: Cell<bool>,
    /// https://dom.spec.whatwg.org/#stop-immediate-propagation-flag
    stop_immediate_propagation: Cell<bool>,
    /// https://dom.spec.whatwg.org/#canceled-flag
    canceled: Cell<bool>,
    /// https://dom.spec.whatwg.org/#in-passive-listener-flag
    in_passive_listener: Cell<bool>,
    /// https://dom.spec.whatwg.org/#composed-flag
    composed: Cell<bool>,
    /// https://dom.spec.whatwg.org/#initialized-flag
    initialized: Cell<bool>,
    /// https://dom.spec.whatwg.org/#dispatch-flag
    dispatch: Cell<bool>,
    /// https://dom.spec.whatwg.org/#dom-event-istrusted
    is_trusted: Cell<bool>,
    /// https://dom.spec.whatwg.org/#event-path
    path: EventPath,
}

impl EventCore {
    /// # Safety
    ///
    /// The event's cell must be alive, which keeps its targets alive.
    #[inline(always)]
    pub(super) unsafe fn target(&self) -> Option<EventTargetRef> {
        // SAFETY: Guaranteed by the caller.
        unsafe { EventTargetRef::from_member(&self.target) }
    }

    #[inline(always)]
    pub(super) fn set_target(&self, target: Option<EventTargetRef>) {
        self.target.set(target);
    }

    /// # Safety
    ///
    /// See [`EventCore::target`].
    #[inline(always)]
    pub(super) unsafe fn related_target(&self) -> Option<EventTargetRef> {
        // SAFETY: Guaranteed by the caller.
        unsafe { EventTargetRef::from_member(&self.related_target) }
    }

    #[inline(always)]
    pub(super) fn set_related_target(&self, related_target: Option<EventTargetRef>) {
        self.related_target.set(related_target);
    }

    #[inline(always)]
    pub(super) fn set_current_target(&self, current_target: Option<EventTargetRef>) {
        self.current_target.set(current_target);
    }

    #[inline(always)]
    pub(super) fn set_event_phase(&self, event_phase: u8) {
        self.event_phase.set(event_phase);
    }

    #[inline(always)]
    pub(super) fn bubbles(&self) -> bool {
        self.bubbles.get()
    }

    #[inline(always)]
    pub(super) fn stop_propagation(&self) -> bool {
        self.stop_propagation.get()
    }

    #[inline(always)]
    pub(super) fn canceled(&self) -> bool {
        self.canceled.get()
    }

    #[inline(always)]
    pub(super) fn composed(&self) -> bool {
        self.composed.get()
    }

    #[inline(always)]
    pub(super) fn is_trusted(&self) -> bool {
        self.is_trusted.get()
    }

    #[inline(always)]
    pub(super) fn set_dispatch(&self, dispatch: bool) {
        self.dispatch.set(dispatch);
    }

    #[inline(always)]
    pub(super) fn unset_propagation_flags(&self) {
        self.stop_propagation.set(false);
        self.stop_immediate_propagation.set(false);
    }

    #[inline(always)]
    pub(super) fn path(&self) -> &EventPath {
        &self.path
    }
}

/// An event, as a function that crosses to C++ takes or returns it: C++ sees it as a raw pointer, which every ABI
/// passes and returns as C does, where a GC::Ref is a class that MSVC returns through a hidden pointer.
pub type RawEventRef = EventRef;

/// An event that is being dispatched, held by the code that dispatches it, which keeps it alive: the caller of the
/// dispatch entry point roots it for as long as the dispatch takes. Its core is read anew on every access, so a read
/// after a call that ran script sees what the script left.
#[derive(Clone, Copy)]
#[repr(transparent)]
pub struct EventRef {
    // NB: NonNull makes the reference neither Send nor Sync.
    cell: NonNull<EventCell>,
}

impl EventRef {
    /// The event `cell` is, as an FFI entry point that holds a main thread token is handed it.
    ///
    /// # Safety
    ///
    /// `cell` must be a live event, which the caller keeps alive for as long as the reference is held.
    #[inline(always)]
    pub(crate) unsafe fn from_cell(cell: &EventCell, _main_thread: &MainThread<'_>) -> Self {
        Self {
            cell: NonNull::from(cell),
        }
    }

    /// The event's core, read for as long as the reference is borrowed, during which nothing may call into C++.
    #[inline(always)]
    pub(super) fn core(&self) -> &EventCore {
        let mut address = self.cell.as_ptr() as usize;
        // SAFETY: The assembly is empty: it keeps the start of the cell live, as NodeRef::core() does.
        unsafe {
            core::arch::asm!(
                "/* {address} */",
                address = inout(reg) address,
                options(nomem, nostack, preserves_flags),
            );
        }
        // SAFETY: Every event embeds its core EVENT_CORE_OFFSET bytes into its cell, which Event.cpp asserts, and the
        //         event is alive for as long as the reference is held.
        unsafe {
            &*core::ptr::with_exposed_provenance::<u8>(address)
                .add(EVENT_CORE_OFFSET)
                .cast::<EventCore>()
        }
    }
}

#[cfg(test)]
impl EventCore {
    pub(crate) fn for_test(bubbles: bool, composed: bool) -> Self {
        Self {
            target: EventTargetMember::new(None),
            related_target: EventTargetMember::new(None),
            current_target: EventTargetMember::new(None),
            event_phase: Cell::new(EVENT_PHASE_NONE),
            bubbles: Cell::new(bubbles),
            cancelable: Cell::new(false),
            stop_propagation: Cell::new(false),
            stop_immediate_propagation: Cell::new(false),
            canceled: Cell::new(false),
            in_passive_listener: Cell::new(false),
            composed: Cell::new(composed),
            initialized: Cell::new(true),
            dispatch: Cell::new(false),
            is_trusted: Cell::new(false),
            path: EventPath {
                items: Cell::new(core::ptr::null_mut()),
                size: Cell::new(0),
                capacity: Cell::new(0),
                touch_targets: Cell::new(core::ptr::null_mut()),
                touch_target_count: Cell::new(0),
            },
        }
    }
}

// NB: Event.cpp asserts the same layout of the structs it includes.
const _: () = assert!(size_of::<EventPathItem>() == 16 && align_of::<EventPathItem>() == 4);
const _: () = assert!(offset_of!(EventPathItem, invocation_target) == 0);
const _: () = assert!(offset_of!(EventPathItem, shadow_adjusted_target) == 4);
const _: () = assert!(offset_of!(EventPathItem, related_target) == 8);
const _: () = assert!(offset_of!(EventPathItem, invocation_target_in_shadow_tree) == 12);
const _: () = assert!(offset_of!(EventPathItem, root_of_closed_tree) == 13);
const _: () = assert!(offset_of!(EventPathItem, slot_in_closed_tree) == 14);
const _: () = assert!(size_of::<EventPath>() == 32 && align_of::<EventPath>() == 8);
const _: () = assert!(offset_of!(EventPath, items) == 0);
const _: () = assert!(offset_of!(EventPath, size) == 8);
const _: () = assert!(offset_of!(EventPath, capacity) == 12);
const _: () = assert!(offset_of!(EventPath, touch_targets) == 16);
const _: () = assert!(offset_of!(EventPath, touch_target_count) == 24);
const _: () = assert!(size_of::<EventCore>() == 56 && align_of::<EventCore>() == 8);
const _: () = assert!(offset_of!(EventCore, target) == 0);
const _: () = assert!(offset_of!(EventCore, related_target) == 4);
const _: () = assert!(offset_of!(EventCore, current_target) == 8);
const _: () = assert!(offset_of!(EventCore, event_phase) == 12);
const _: () = assert!(offset_of!(EventCore, bubbles) == 13);
const _: () = assert!(offset_of!(EventCore, cancelable) == 14);
const _: () = assert!(offset_of!(EventCore, stop_propagation) == 15);
const _: () = assert!(offset_of!(EventCore, stop_immediate_propagation) == 16);
const _: () = assert!(offset_of!(EventCore, canceled) == 17);
const _: () = assert!(offset_of!(EventCore, in_passive_listener) == 18);
const _: () = assert!(offset_of!(EventCore, composed) == 19);
const _: () = assert!(offset_of!(EventCore, initialized) == 20);
const _: () = assert!(offset_of!(EventCore, dispatch) == 21);
const _: () = assert!(offset_of!(EventCore, is_trusted) == 22);
const _: () = assert!(offset_of!(EventCore, path) == 24);
