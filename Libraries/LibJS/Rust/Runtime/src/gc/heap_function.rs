/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC::Function<ThrowCompletionOr<Value>()> of Libraries/LibGC/Function.h: a closure that lives in the heap, which
//! the job queues hold.

use core::mem::MaybeUninit;

use super::class::{GcCell, define_cell};
use super::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;

struct CapturingBehaviour<C, F> {
    captures: C,
    behaviour: F,
}

/// Room for the captures of every heap function the runtime creates, the most being Promise.all's resolve element
/// job with five, so that a heap function keeps them in its own cell rather than in an allocation of their own.
#[repr(C, align(8))]
struct CaptureStorage([MaybeUninit<u8>; 48]);

#[repr(C)]
pub struct HeapFunction {
    header: CellHeader,
    /// call_with_captures() and trace_captures() for the types of the captures and the behaviour in `storage`.
    call: unsafe fn(*const CaptureStorage, &Vm) -> ThrowCompletionOr<Value>,
    trace: unsafe fn(*const CaptureStorage, &mut Visitor),
    storage: CaptureStorage,
}

define_cell!(HeapFunction, Other);

// SAFETY: The captures are all the cells the function reaches, and `trace` visits them.
unsafe impl Trace for HeapFunction {
    fn trace(&self, visitor: &mut Visitor) {
        // SAFETY: create_heap_function() stored the captures that `trace` was instantiated for.
        unsafe { (self.trace)(&raw const self.storage, visitor) };
    }
}

/// # Safety
///
/// `storage` must hold a CapturingBehaviour<C, F>.
unsafe fn call_with_captures<C, F>(storage: *const CaptureStorage, vm: &Vm) -> ThrowCompletionOr<Value>
where
    C: Copy,
    F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + Copy,
{
    // SAFETY: The caller passes storage that holds a CapturingBehaviour<C, F>, which fits and is aligned for it.
    let behaviour = unsafe { storage.cast::<CapturingBehaviour<C, F>>().read() };
    // NB: The captures are copied out of the cell, so nothing in the call points into it.
    (behaviour.behaviour)(vm, &behaviour.captures)
}

/// # Safety
///
/// `storage` must hold a CapturingBehaviour<C, F>.
unsafe fn trace_captures<C: Trace, F>(storage: *const CaptureStorage, visitor: &mut Visitor) {
    // SAFETY: The caller passes storage that holds a CapturingBehaviour<C, F>, which fits and is aligned for it.
    let behaviour = unsafe { &*storage.cast::<CapturingBehaviour<C, F>>() };
    behaviour.captures.trace(visitor);
}

/// GC::create_function(heap, function). The behaviour must not capture anything: the state it needs is `captures`,
/// which the heap function keeps alive.
pub fn create_heap_function<C, F>(vm: &Vm, captures: C, behaviour: F) -> Gc<HeapFunction>
where
    C: Trace + Copy + 'static,
    F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + Copy + 'static,
{
    const {
        assert!(
            size_of::<F>() == 0,
            "a heap function captures its state through its captures, not its behaviour"
        );
        assert!(size_of::<CapturingBehaviour<C, F>>() <= size_of::<CaptureStorage>());
        assert!(align_of::<CapturingBehaviour<C, F>>() <= align_of::<CaptureStorage>());
    };
    let mut storage = CaptureStorage([MaybeUninit::uninit(); 48]);
    // SAFETY: The storage is large and aligned enough for the behaviour, as checked above.
    unsafe {
        core::ptr::from_mut(&mut storage)
            .cast::<CapturingBehaviour<C, F>>()
            .write(CapturingBehaviour { captures, behaviour });
    }
    // The captures stay in `storage` on the stack, where the collector finds them, until the function that keeps
    // them alive exists.
    vm.heap().allocate(HeapFunction {
        header: CellHeader::for_class(HeapFunction::CLASS),
        call: call_with_captures::<C, F>,
        trace: trace_captures::<C, F>,
        storage,
    })
}

impl HeapFunction {
    /// Calls the function. The caller's Gc keeps the function and the state it captures alive for the whole call.
    pub fn call(function: Gc<HeapFunction>, vm: &Vm) -> ThrowCompletionOr<Value> {
        // SAFETY: create_heap_function() stored the captures that `call` was instantiated for.
        let result = unsafe { (function.call)(&raw const function.storage, vm) };
        // NB: Using the Gc after the call keeps it where the conservative scan finds it until the call has returned.
        core::hint::black_box(function);
        result
    }
}
