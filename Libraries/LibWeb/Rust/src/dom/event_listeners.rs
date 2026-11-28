/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The event listener list of an event target, and what adding, removing and invoking event listeners does with it.
//! https://dom.spec.whatwg.org/#eventtarget-event-listener-list
//!
//! An event listener is a DOM::DOMEventListener, a cell C++ allocates and fills, whose fields Rust reads in place
//! through [`EventListenerFields`], which mirrors their layout. A target that ever had a listener keeps its list in an
//! [`EventListenerList`]: the listeners as compressed pointers in a buffer Rust allocates, grows and frees, which is the
//! first field of the target's DOM::EventTarget::Data, which C++ allocates the first time the target needs it. Rust
//! visits every listener of the list as the target's edges are visited, see EventTargetDataField.
//!
//! The listeners a dispatch runs for a target are a clone of the list, taken before any of them runs. The clone lives
//! on the stack, in chunks of up to CLONED_LISTENERS_CHUNK references to the start of each listener's cell, which the
//! conservative stack scan recognizes, so the listeners it holds stay alive and in place while their callbacks run
//! script, also where script removes them from the list.

use core::cell::Cell;
use core::cell::UnsafeCell;
use core::marker::PhantomData;
use core::marker::PhantomPinned;
use core::mem::offset_of;
use core::num::NonZeroU32;
use core::ops::ControlFlow;
use core::ptr::NonNull;
use std::alloc::Layout;

use libgc_api::member::write_barrier;

use super::event_target::EventTargetCell;
use super::event_target::EventTargetRef;
use super::node::NodeCell;
use super::node::compress;
use super::node::decompress;
use super::node_core::NodePointer;

crate::stage::main_thread_ffi_entries!(without_host);

/// Where DOM::IDLEventListener keeps its GC::Ref<WebIDL::CallbackType>, in bytes from the start of its cell, which
/// IDLEventListener.cpp asserts.
pub const IDL_EVENT_LISTENER_CALLBACK_OFFSET: usize = 16;

/// Where WebIDL::CallbackType keeps the GC::NonnullMember<JS::Object> of its callback, in bytes from the start of its
/// cell, which IDLEventListener.cpp asserts.
pub const CALLBACK_TYPE_CALLBACK_OFFSET: usize = 16;

/// A DOM::DOMEventListener as C++ allocates it, which C++ sees as that class. FFI entry points take listeners as
/// references to it.
#[repr(C)]
pub struct EventListenerCell {
    // NB: The cell's state changes behind shared references to it, see NodeCell.
    _opaque: UnsafeCell<[u8; 0]>,
    // NB: C++ owns the cell, which Rust never moves, and which stays on the main thread.
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

/// A DOM::IDLEventListener as C++ allocates it, which C++ sees as that class.
#[repr(C)]
pub struct IdlEventListenerCell {
    _opaque: UnsafeCell<[u8; 0]>,
    _owned_by_cpp: PhantomData<(*mut u8, PhantomPinned)>,
}

/// The fields of a DOM::DOMEventListener, laid out as C++ declares them after the cell's header, which
/// DOMEventListener.cpp asserts. Rust reads them, and sets only the removed flag.
/// https://dom.spec.whatwg.org/#concept-event-listener
#[repr(C)]
pub struct EventListenerFields {
    _cell_header: [u8; 11],
    /// The EVENT_LISTENER_* flags, which are the embedder flags of the cell.
    flags: Cell<u8>,
    _embedder_type_id: [u8; 2],
    /// The GC::Member<DOM::IDLEventListener> of the listener's callback, 0 for null.
    /// https://dom.spec.whatwg.org/#event-listener-callback
    callback: Cell<u32>,
    /// https://dom.spec.whatwg.org/#event-listener-signal
    _signal: Cell<u32>,
    /// The raw word of the interned string of the listener's type.
    /// https://dom.spec.whatwg.org/#event-listener-type
    type_: UnsafeCell<usize>,
}

/// https://dom.spec.whatwg.org/#event-listener-capture
pub const EVENT_LISTENER_CAPTURE: u8 = 1 << 0;
/// https://dom.spec.whatwg.org/#event-listener-once
pub const EVENT_LISTENER_ONCE: u8 = 1 << 1;
/// https://dom.spec.whatwg.org/#event-listener-removed
pub const EVENT_LISTENER_REMOVED: u8 = 1 << 2;
/// Whether the listener's passive is a boolean rather than null, which EVENT_LISTENER_PASSIVE then is. Only C++ reads
/// these two.
/// https://dom.spec.whatwg.org/#event-listener-passive
pub const EVENT_LISTENER_PASSIVE_IS_SET: u8 = 1 << 3;
pub const EVENT_LISTENER_PASSIVE: u8 = 1 << 4;

const _: () = assert!(size_of::<EventListenerFields>() == 32 && align_of::<EventListenerFields>() == 8);
pub const EVENT_LISTENER_FLAGS_OFFSET: usize = 11;
pub const EVENT_LISTENER_CALLBACK_OFFSET: usize = 16;
pub const EVENT_LISTENER_TYPE_OFFSET: usize = 24;
const _: () = assert!(offset_of!(EventListenerFields, flags) == EVENT_LISTENER_FLAGS_OFFSET);
const _: () = assert!(offset_of!(EventListenerFields, callback) == EVENT_LISTENER_CALLBACK_OFFSET);
const _: () = assert!(offset_of!(EventListenerFields, type_) == EVENT_LISTENER_TYPE_OFFSET);

impl EventListenerFields {
    #[inline(always)]
    pub(super) fn capture(&self) -> bool {
        self.flags.get() & EVENT_LISTENER_CAPTURE != 0
    }

    #[inline(always)]
    pub(super) fn once(&self) -> bool {
        self.flags.get() & EVENT_LISTENER_ONCE != 0
    }

    #[inline(always)]
    pub(super) fn removed(&self) -> bool {
        self.flags.get() & EVENT_LISTENER_REMOVED != 0
    }

    /// https://dom.spec.whatwg.org/#event-listener-removed
    #[inline(always)]
    fn set_removed(&self) {
        self.flags.set(self.flags.get() | EVENT_LISTENER_REMOVED);
    }

    /// The raw word of the interned string of the listener's type.
    #[inline(always)]
    pub(super) fn type_raw(&self) -> usize {
        // SAFETY: C++ sets the type before it adds the listener to a list, and never changes it after that.
        unsafe { *self.type_.get() }
    }

    /// The identity of the listener's callback: the compressed pointer of the JS object of its callback, as callbacks
    /// are compared, or none if it is null.
    #[inline(always)]
    fn callback_identity(&self) -> Option<NonZeroU32> {
        let compressed = NonZeroU32::new(self.callback.get())?;
        let idl_event_listener = decompress(NodePointer::from_compressed(compressed.get()));
        // SAFETY: The listener keeps its callback alive.
        Some(unsafe { idl_event_listener_callback_identity(idl_event_listener.cast()) })
    }
}

/// The compressed pointer of the JS object of the callback of `idl_event_listener`.
///
/// # Safety
///
/// `idl_event_listener` must be a live DOM::IDLEventListener.
#[inline(always)]
unsafe fn idl_event_listener_callback_identity(idl_event_listener: NonNull<IdlEventListenerCell>) -> NonZeroU32 {
    // SAFETY: Guaranteed by the caller. The offsets are those IDLEventListener.cpp asserts, and an IDLEventListener keeps
    //         its CallbackType alive, which keeps its object.
    unsafe {
        let callback_type = *idl_event_listener
            .as_ptr()
            .byte_add(IDL_EVENT_LISTENER_CALLBACK_OFFSET)
            .cast::<*const u8>();
        let object = *callback_type.add(CALLBACK_TYPE_CALLBACK_OFFSET).cast::<u32>();
        NonZeroU32::new(object).expect("a callback has an object")
    }
}

/// A listener, as a function that crosses to C++ takes or returns it: C++ sees it as a raw pointer, which every ABI
/// passes and returns as C does, where a GC::Ref is a class that MSVC returns through a hidden pointer.
pub type RawEventListenerRef = EventListenerRef;

/// A listener held by the code that adds, removes or invokes it, valid for as long as something keeps it alive: the
/// list it is read from while nothing calls into C++, or a clone of the list on the stack. It is laid out as a
/// GC::Ref<DOM::DOMEventListener>.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct EventListenerRef {
    // NB: NonNull makes the reference neither Send nor Sync.
    cell: NonNull<EventListenerCell>,
}

impl EventListenerRef {
    /// The listener `compressed` names.
    #[inline(always)]
    fn from_compressed(compressed: NonZeroU32) -> Self {
        Self {
            cell: decompress(NodePointer::from_compressed(compressed.get())).cast(),
        }
    }

    /// The listener `cell` is, as an FFI entry point is handed it.
    #[inline(always)]
    fn from_cell(cell: &EventListenerCell) -> Self {
        Self {
            cell: NonNull::from(cell),
        }
    }

    /// The compressed pointer that names the listener, as GC::Member fields hold it.
    #[inline(always)]
    fn compressed(self) -> NonZeroU32 {
        // SAFETY: The listener is alive for as long as the reference is held.
        let pointer = compress(unsafe { self.cell.cast::<NodeCell>().as_ref() });
        NonZeroU32::new(pointer.compressed()).expect("a cell pointer is never null")
    }

    /// The listener's fields, read anew on every access.
    #[inline(always)]
    pub(super) fn fields(&self) -> &EventListenerFields {
        // SAFETY: The listener is alive for as long as the reference is held, and starts with the fields' layout.
        unsafe { self.cell.cast::<EventListenerFields>().as_ref() }
    }
}

/// The event listener list of an event target, laid out as C++ reads it: the listeners as
/// GC::Member<DOM::DOMEventListener>s in a buffer Rust allocates, as many as the size, with room for the capacity. Only
/// Rust writes it. C++ visits every listener of it as it visits the edges of the target, and has Rust free the buffer
/// as it destroys the target's data.
#[repr(C)]
pub struct EventListenerList {
    items: Cell<*mut EventListenerMember>,
    size: Cell<u32>,
    capacity: Cell<u32>,
}

/// A field that holds a listener, laid out as C++'s GC::Member<DOM::DOMEventListener>.
#[repr(transparent)]
pub struct EventListenerMember(Cell<u32>);

const _: () = assert!(size_of::<EventListenerMember>() == 4 && align_of::<EventListenerMember>() == 4);
const _: () = assert!(size_of::<EventListenerList>() == 16 && align_of::<EventListenerList>() == 8);
const _: () = assert!(offset_of!(EventListenerList, items) == 0);
const _: () = assert!(offset_of!(EventListenerList, size) == 8);
const _: () = assert!(offset_of!(EventListenerList, capacity) == 12);

impl EventListenerList {
    #[inline(always)]
    fn len(&self) -> usize {
        self.size.get() as usize
    }

    /// The compressed pointers of the listeners, read for as long as the list is borrowed, during which nothing may
    /// call into C++.
    #[inline(always)]
    fn pointers(&self) -> &[EventListenerMember] {
        // SAFETY: The buffer holds `size` listeners, which only Rust writes, or is null where the list never had any.
        unsafe { ak::slice_from_raw_parts(self.items.get(), self.len()) }
    }

    #[inline(always)]
    fn get(&self, index: usize) -> EventListenerRef {
        let compressed = NonZeroU32::new(self.pointers()[index].0.get()).expect("a listener list holds no null");
        EventListenerRef::from_compressed(compressed)
    }

    /// The index of the listener whose type is `type_raw`, callback is `callback` and capture is `capture`, if any.
    fn position_of_match(&self, type_raw: usize, callback: Option<NonZeroU32>, capture: bool) -> Option<usize> {
        (0..self.len()).find(|&index| {
            let entry = self.get(index);
            let entry = entry.fields();
            entry.type_raw() == type_raw && entry.capture() == capture && entry.callback_identity() == callback
        })
    }

    fn position(&self, listener: EventListenerRef) -> Option<usize> {
        let compressed = listener.compressed().get();
        self.pointers().iter().position(|member| member.0.get() == compressed)
    }

    fn push(&self, listener: EventListenerRef) {
        let size = self.len();
        if size == self.capacity.get() as usize {
            self.grow();
        }
        let compressed = listener.compressed().get();
        // SAFETY: The buffer has room for one more listener past the `size` it holds.
        unsafe {
            let slot = self.items.get().add(size);
            slot.write(EventListenerMember(Cell::new(0)));
            write_barrier((*slot).0.as_ptr(), compressed);
            (*slot).0.set(compressed);
        }
        // NB: The listener counts, and so C++ visits it, only once it is written.
        self.size.set(size as u32 + 1);
    }

    fn remove(&self, index: usize) {
        let size = self.len();
        assert!(index < size);
        // SAFETY: The buffer holds `size` listeners, which move down by one onto `index`.
        unsafe {
            let items = self.items.get();
            core::ptr::copy(items.add(index + 1), items.add(index), size - index - 1);
        }
        self.size.set(size as u32 - 1);
    }

    #[cold]
    fn grow(&self) {
        let capacity = self.capacity.get() as usize;
        let new_capacity = (capacity * 2).max(4);
        let new_layout =
            Layout::array::<EventListenerMember>(new_capacity).expect("a listener list is never that long");
        // SAFETY: The buffer was allocated with the layout of `capacity` listeners, or is null where `capacity` is 0,
        //         and neither layout is zero-sized.
        let items = unsafe {
            match capacity {
                0 => std::alloc::alloc(new_layout),
                _ => std::alloc::realloc(
                    self.items.get().cast(),
                    Layout::array::<EventListenerMember>(capacity).unwrap(),
                    new_layout.size(),
                ),
            }
        };
        if items.is_null() {
            std::alloc::handle_alloc_error(new_layout);
        }
        self.items.set(items.cast());
        self.capacity
            .set(u32::try_from(new_capacity).expect("a listener list is never that long"));
    }

    /// Frees the buffer, which leaves the list empty.
    fn free(&self) {
        let capacity = self.capacity.get() as usize;
        if capacity != 0 {
            // SAFETY: The buffer was allocated with the layout of `capacity` listeners.
            unsafe {
                std::alloc::dealloc(
                    self.items.get().cast(),
                    Layout::array::<EventListenerMember>(capacity).unwrap(),
                );
            };
        }
        self.items.set(core::ptr::null_mut());
        self.size.set(0);
        self.capacity.set(0);
    }
}

/// The bit of an event type, whose interned string's raw word is `type_raw`, in the listener type summaries of an
/// event target's core, which DOM::event_type_summary_bit() picks the same way.
#[inline(always)]
pub(super) fn event_type_summary_bit(type_raw: usize) -> u16 {
    1 << ((type_raw as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 60)
}

/// The event listener list of `target`, if it ever needed one.
#[inline(always)]
fn listener_list(target: &EventTargetRef) -> Option<&EventListenerList> {
    // SAFETY: The target is alive for as long as the reference is held, and C++ gives it no new data while it is
    //         borrowed.
    unsafe { target.prefix().data.listener_list() }
}

/// Works the listener type summaries of `target`'s core out again from its listeners.
fn update_listener_type_summaries(target: EventTargetRef, list: &EventListenerList) {
    let mut capture_listener_types = 0;
    let mut bubble_listener_types = 0;
    for index in 0..list.len() {
        let listener = list.get(index);
        let fields = listener.fields();
        let bit = event_type_summary_bit(fields.type_raw());
        match fields.capture() {
            true => capture_listener_types |= bit,
            false => bubble_listener_types |= bit,
        }
    }
    target
        .core()
        .set_listener_types(capture_listener_types, bubble_listener_types);
}

/// Step 5 of adding an event listener `listener` to `target`: if the target's event listener list does not contain an
/// event listener whose type is listener's type, callback is listener's callback, and capture is listener's capture,
/// appends listener to it, and answers whether it did.
/// https://dom.spec.whatwg.org/#add-an-event-listener
fn append_if_absent(target: EventTargetRef, listener: EventListenerRef) -> bool {
    let list = listener_list(&target).expect("C++ makes the data of a target it adds a listener to");
    let fields = listener.fields();
    let (type_raw, capture) = (fields.type_raw(), fields.capture());
    if list
        .position_of_match(type_raw, fields.callback_identity(), capture)
        .is_some()
    {
        return false;
    }
    list.push(listener);
    let bit = event_type_summary_bit(type_raw);
    let core = target.core();
    match capture {
        true => core.set_listener_types(core.capture_listener_types() | bit, core.bubble_listener_types()),
        false => core.set_listener_types(core.capture_listener_types(), core.bubble_listener_types() | bit),
    }
    true
}

/// Step 2 of removing an event listener `listener` from `target`: sets listener's removed to true and removes it from
/// the target's event listener list, then works the target's listener type summaries out again. Answers whether the
/// list held the listener.
/// https://dom.spec.whatwg.org/#remove-an-event-listener
pub(super) fn remove_an_event_listener(target: EventTargetRef, listener: EventListenerRef) -> bool {
    listener.fields().set_removed();
    let Some(list) = listener_list(&target) else {
        return false;
    };
    let Some(index) = list.position(listener) else {
        return false;
    };
    remove_at(target, list, index);
    true
}

/// Removes the listener at `index` from `target`'s event listener list `list`, and works the target's listener type
/// summaries out again.
fn remove_at(target: EventTargetRef, list: &EventListenerList, index: usize) {
    list.remove(index);
    update_listener_type_summaries(target, list);
}

/// How many listeners a chunk of a clone of a listener list holds.
const CLONED_LISTENERS_CHUNK: usize = 16;

/// A chunk of a clone of the listeners of a target's event listener list, which lives on the stack, see the module,
/// after the chunks before it. It holds the start of each listener's cell, which the conservative stack scan
/// recognizes.
pub(super) struct ClonedListeners<'a> {
    previous: Option<&'a ClonedListeners<'a>>,
    listeners: [Option<EventListenerRef>; CLONED_LISTENERS_CHUNK],
    len: usize,
}

impl ClonedListeners<'_> {
    /// Runs `f` with each listener of the clone, in order, until it breaks.
    pub(super) fn try_for_each(&self, f: &mut impl FnMut(EventListenerRef) -> ControlFlow<()>) -> ControlFlow<()> {
        if let Some(previous) = self.previous {
            previous.try_for_each(f)?;
        }
        for listener in self.listeners[..self.len].iter().flatten() {
            f(*listener)?;
        }
        ControlFlow::Continue(())
    }
}

/// Clones the listeners of `target`'s event listener list whose type is `type_raw`, or `legacy_type_raw` where that is
/// not 0, from `start` on, after the chunks `previous` holds, and runs `f` with the clone.
pub(super) fn with_cloned_listeners(
    target: EventTargetRef,
    type_raw: usize,
    legacy_type_raw: usize,
    previous: Option<&ClonedListeners<'_>>,
    start: usize,
    f: &mut dyn FnMut(&ClonedListeners<'_>),
) {
    let mut chunk = ClonedListeners {
        previous,
        listeners: [None; CLONED_LISTENERS_CHUNK],
        len: 0,
    };
    let mut next_start = None;
    if let Some(list) = listener_list(&target) {
        for (index, member) in list.pointers().iter().enumerate().skip(start) {
            let Some(compressed) = NonZeroU32::new(member.0.get()) else {
                continue;
            };
            let listener = EventListenerRef::from_compressed(compressed);
            let listener_type = listener.fields().type_raw();
            if listener_type != type_raw && (legacy_type_raw == 0 || listener_type != legacy_type_raw) {
                continue;
            }
            if chunk.len == CLONED_LISTENERS_CHUNK {
                next_start = Some(index);
                break;
            }
            chunk.listeners[chunk.len] = Some(listener);
            chunk.len += 1;
        }
    }
    // NB: `f` runs script, so the list is no longer borrowed here.
    match next_start {
        Some(index) => with_cloned_listeners(target, type_raw, legacy_type_raw, Some(&chunk), index, f),
        None => f(&chunk),
    }
}

/// Step 5 of adding an event listener, see [`append_if_absent`], for DOM::EventTarget, which runs the other steps.
///
/// # Safety
///
/// The target and the listener must be live, the target must have its data, and the call must be made on the main
/// thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_append_event_listener_if_absent(
    target: &EventTargetCell,
    listener: &EventListenerCell,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The target is live.
    let target = unsafe { EventTargetRef::from_cell(target, &main_thread) };
    append_if_absent(target, EventListenerRef::from_cell(listener))
}

/// Step 2 of the removeEventListener() steps of DOM::EventTarget: if `target`'s event listener list contains an event
/// listener whose type is the interned string whose raw word is `type_raw`, whose callback is `callback`, or null, and
/// whose capture is `capture`, then removes an event listener with the target and that listener, but for the steps
/// that follow from the removal, which C++ runs for the listener this answers, if any.
/// https://dom.spec.whatwg.org/#dom-eventtarget-removeeventlistener
///
/// # Safety
///
/// The target, and the callback where it is not null, must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_matching_event_listener(
    target: &EventTargetCell,
    type_raw: usize,
    callback: Option<&IdlEventListenerCell>,
    capture: bool,
) -> *const EventListenerCell {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The target is live.
    let target = unsafe { EventTargetRef::from_cell(target, &main_thread) };
    let Some(list) = listener_list(&target) else {
        return core::ptr::null();
    };
    // SAFETY: The callback is live.
    let callback = callback.map(|callback| unsafe { idl_event_listener_callback_identity(NonNull::from(callback)) });
    let Some(index) = list.position_of_match(type_raw, callback, capture) else {
        return core::ptr::null();
    };
    let listener = list.get(index);

    // https://dom.spec.whatwg.org/#remove-an-event-listener
    // 2. Set listener's removed to true and remove listener from eventTarget's event listener list.
    listener.fields().set_removed();
    remove_at(target, list, index);
    listener.cell.as_ptr().cast_const()
}

/// Step 2 of removing an event listener, see [`remove_an_event_listener`], for DOM::EventTarget, which runs the steps
/// that follow from it where this answers that the list held the listener.
///
/// # Safety
///
/// The target and the listener must be live, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_remove_an_event_listener(
    target: &EventTargetCell,
    listener: &EventListenerCell,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let main_thread = unsafe { main_thread() };
    // SAFETY: The target is live.
    let target = unsafe { EventTargetRef::from_cell(target, &main_thread) };
    remove_an_event_listener(target, EventListenerRef::from_cell(listener))
}

/// Frees the buffer of an event listener list, for DOM::EventTarget::Data as it is destroyed.
///
/// # Safety
///
/// `list` must be the list of a target's data that is being destroyed, and the call must be made on the main thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_dom_event_listener_list_destroy(list: &EventListenerList) {
    list.free();
}
