/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! GC::Function<ThrowCompletionOr<Value>()> of Libraries/LibGC/Function.h: a closure that lives in the heap, which
//! the job queues hold.

use core::cell::OnceCell;

use libjs_runtime_macros::Trace;

use super::class::{GcCell, define_cell};
use super::visitor::{Trace, Visitor};
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;

/// The behaviour of a heap function together with the state it captures.
trait HeapFunctionBehaviour: Trace + 'static {
    fn call(&self, vm: &Vm) -> ThrowCompletionOr<Value>;
}

struct CapturingBehaviour<C, F> {
    captures: C,
    behaviour: F,
}

// SAFETY: The behaviour is zero-sized, so the captures are all the cells this reaches.
unsafe impl<C: Trace, F> Trace for CapturingBehaviour<C, F> {
    fn trace(&self, visitor: &mut Visitor) {
        self.captures.trace(visitor);
    }
}

impl<C, F> HeapFunctionBehaviour for CapturingBehaviour<C, F>
where
    C: Trace + 'static,
    F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
{
    fn call(&self, vm: &Vm) -> ThrowCompletionOr<Value> {
        (self.behaviour)(vm, &self.captures)
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct HeapFunction {
    header: CellHeader,
    function: OnceCell<Box<dyn HeapFunctionBehaviour>>,
}

define_cell!(HeapFunction, Other);

/// GC::create_function(heap, function). The behaviour must not capture anything: the state it needs is `captures`,
/// which the heap function keeps alive.
pub fn create_heap_function<C, F>(vm: &Vm, captures: C, behaviour: F) -> Gc<HeapFunction>
where
    C: Trace + 'static,
    F: Fn(&Vm, &C) -> ThrowCompletionOr<Value> + 'static,
{
    const {
        assert!(
            size_of::<F>() == 0,
            "a heap function captures its state through its captures, not its behaviour"
        );
    };
    let function = vm.heap().allocate(HeapFunction {
        header: CellHeader::for_class(HeapFunction::CLASS),
        function: OnceCell::new(),
    });
    // The captures stay on the stack, where the collector finds them, until the function that keeps them alive
    // exists. Nothing allocates from the heap between allocating the function and storing them in it.
    let behaviour: Box<dyn HeapFunctionBehaviour> = Box::new(CapturingBehaviour { captures, behaviour });
    if function.function.set(behaviour).is_err() {
        unreachable!("the behaviour is stored once");
    }
    function
}

impl HeapFunction {
    /// Calls the function. The caller's Gc keeps the function and the state it captures alive for the whole call.
    pub fn call(function: Gc<HeapFunction>, vm: &Vm) -> ThrowCompletionOr<Value> {
        let result = function
            .function
            .get()
            .expect("a heap function has its behaviour")
            .call(vm);
        // NB: The behaviour lives out of line, so nothing in the call needs to point into the function cell. Using the
        //     Gc after the call keeps it where the conservative scan finds it until the behaviour has returned.
        core::hint::black_box(function);
        result
    }
}
