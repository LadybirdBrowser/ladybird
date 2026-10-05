/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::{Cell, RefCell};
use core::ptr::NonNull;
use std::alloc::{Layout, alloc, dealloc, handle_alloc_error};

use crate::gc::visitor::{Trace, Visitor};
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
use crate::layout::value::Value;
use libjs_abi::register::RESERVED_REGISTER_COUNT;

impl ExecutionContext {
    /// Constructs a context for a frame of `slot_count` slots, the last `argument_count` of which hold arguments, at
    /// `context`.
    ///
    /// # Safety
    ///
    /// `context` must point to writable memory for the context and its slots, which nothing else uses.
    pub(crate) unsafe fn initialize_at(
        context: *mut ExecutionContext,
        registers_and_locals_count: u32,
        slot_count: u32,
        argument_count: u32,
        frame_id: u64,
    ) {
        // SAFETY: The caller passes memory for the context and its slots.
        unsafe {
            context.write(ExecutionContext {
                function: Cell::new(None),
                realm: Cell::new(None),
                script_or_module: Cell::new(ScriptOrModule::Empty),
                lexical_environment: Cell::new(None),
                variable_environment: Cell::new(None),
                private_environment: Cell::new(None),
                frame_id: Cell::new(frame_id),
                program_counter: Cell::new(0),
                skip_when_determining_incumbent_counter: Cell::new(0),
                yield_continuation: Cell::new(ExecutionContext::NO_YIELD_CONTINUATION),
                yield_is_await: Cell::new(false),
                yield_value_is_iterator_result: Cell::new(false),
                caller_is_construct: Cell::new(false),
                frame_initialized: Cell::new(false),
                this_value: Cell::new(Value::EMPTY),
                executable: Cell::new(None),
                caller_frame: Cell::new(core::ptr::null_mut()),
                passed_argument_count: Cell::new(0),
                caller_return_pc: Cell::new(0),
                caller_dst_raw: Cell::new(0),
                registers_and_constants_and_locals_and_arguments_count: Cell::new(slot_count),
                argument_count: Cell::new(argument_count),
            });
            // NB: Enter initializes the remaining registers, locals, and constants.
            for slot in (*context)
                .slots()
                .iter()
                .take(registers_and_locals_count.min(RESERVED_REGISTER_COUNT) as usize)
            {
                slot.set(Value::EMPTY);
            }
        }
    }

    /// The frame's registers, locals, constants and arguments, which follow the context directly.
    pub fn slots(&self) -> &[Cell<Value>] {
        let count = self.registers_and_constants_and_locals_and_arguments_count.get() as usize;
        // SAFETY: Contexts are only ever allocated with their slots following them.
        unsafe { core::slice::from_raw_parts(core::ptr::from_ref(self).add(1).cast::<Cell<Value>>(), count) }
    }

    pub fn register(&self, index: u32) -> &Cell<Value> {
        &self.slots()[index as usize]
    }

    pub fn arguments(&self) -> &[Cell<Value>] {
        let slots = self.slots();
        &slots[slots.len() - self.argument_count.get() as usize..]
    }

    pub fn argument(&self, index: usize) -> Value {
        self.arguments().get(index).map_or(Value::UNDEFINED, Cell::get)
    }

    /// ExecutionContext::copy(), for the generator that keeps the context of the call that created it.
    pub fn copy(&self) -> OwnedExecutionContext {
        let slot_count = self.registers_and_constants_and_locals_and_arguments_count.get();
        let argument_count = self.argument_count.get();
        // NB: We pass the entire non-argument count as registers_and_locals_count with 0 constants.
        let copy = OwnedExecutionContext::create(slot_count - argument_count, 0, argument_count);
        copy.function.set(self.function.get());
        copy.realm.set(self.realm.get());
        copy.script_or_module.set(self.script_or_module.get());
        copy.lexical_environment.set(self.lexical_environment.get());
        copy.variable_environment.set(self.variable_environment.get());
        copy.private_environment.set(self.private_environment.get());
        copy.program_counter.set(self.program_counter.get());
        copy.frame_id.set(self.frame_id.get());
        copy.yield_continuation.set(self.yield_continuation.get());
        copy.yield_is_await.set(self.yield_is_await.get());
        copy.yield_value_is_iterator_result
            .set(self.yield_value_is_iterator_result.get());
        copy.caller_is_construct.set(self.caller_is_construct.get());
        copy.frame_initialized.set(self.frame_initialized.get());
        copy.this_value.set(self.this_value.get());
        copy.executable.set(self.executable.get());
        copy.passed_argument_count.set(self.passed_argument_count.get());
        let frame_initialized = self.frame_initialized.get();
        let non_argument_count = (slot_count - argument_count) as usize;
        for (index, (copied_slot, slot)) in copy.slots().iter().zip(self.slots()).enumerate() {
            if !frame_initialized && index >= RESERVED_REGISTER_COUNT as usize && index < non_argument_count {
                continue;
            }
            copied_slot.set(slot.get());
        }
        copy
    }
}

// SAFETY: Visits every cell a frame can reach. Until the frame is initialized only its reserved registers and its
// arguments hold values; the other slots are left over from earlier frames.
unsafe impl Trace for ExecutionContext {
    fn trace(&self, visitor: &mut Visitor) {
        self.function.trace(visitor);
        self.realm.trace(visitor);
        self.variable_environment.trace(visitor);
        self.lexical_environment.trace(visitor);
        self.private_environment.trace(visitor);
        self.this_value.trace(visitor);
        self.executable.trace(visitor);
        match self.script_or_module.get() {
            ScriptOrModule::Empty => {}
            ScriptOrModule::Script(script) => script.trace(visitor),
            ScriptOrModule::Module(module) => module.trace(visitor),
        }
        let slots = self.slots();
        if self.frame_initialized.get() {
            trace_slots(slots, visitor);
        } else {
            let non_argument_count = slots.len() - self.argument_count.get() as usize;
            trace_slots(
                &slots[..non_argument_count.min(RESERVED_REGISTER_COUNT as usize)],
                visitor,
            );
            trace_slots(self.arguments(), visitor);
        }
    }
}

fn trace_slots(slots: &[Cell<Value>], visitor: &mut Visitor) {
    // SAFETY: Cell<Value> has the same layout as Value, and nothing writes the slots while they are visited.
    let values = unsafe { core::slice::from_raw_parts(slots.as_ptr().cast::<Value>(), slots.len()) };
    visitor.visit_values(values);
}

/// The slot counts that C++ ExecutionContextAllocator rounds the slots of a context outside the interpreter stack up
/// to. Contexts of these sizes are not freed, but kept for the next context of their size, as the host creates and
/// drops one for many callbacks and promise jobs.
const REUSED_SLOT_COUNTS: [u32; 6] = [4, 16, 64, 128, 256, 512];

/// The contexts of each of REUSED_SLOT_COUNTS that were dropped on this thread, which is the thread of its VM.
struct ReusableExecutionContexts([Vec<NonNull<ExecutionContext>>; REUSED_SLOT_COUNTS.len()]);

impl Drop for ReusableExecutionContexts {
    fn drop(&mut self) {
        for (contexts, &slot_count) in self.0.iter().zip(REUSED_SLOT_COUNTS.iter()) {
            for context in contexts {
                // SAFETY: The context was allocated with the layout of its reused slot count, and nothing uses it.
                unsafe {
                    dealloc(
                        context.as_ptr().cast(),
                        OwnedExecutionContext::layout_for_slot_count(slot_count),
                    );
                }
            }
        }
    }
}

thread_local! {
    static REUSABLE_EXECUTION_CONTEXTS: RefCell<ReusableExecutionContexts> =
        const { RefCell::new(ReusableExecutionContexts([const { Vec::new() }; REUSED_SLOT_COUNTS.len()])) };
}

/// The index in REUSED_SLOT_COUNTS of the size a context of `slot_count` slots is allocated with, if it is reused.
fn reused_slot_count_index(slot_count: u32) -> Option<usize> {
    REUSED_SLOT_COUNTS
        .iter()
        .position(|&reused_slot_count| slot_count <= reused_slot_count)
}

/// An execution context that lives outside the interpreter stack, like the ones C++ ExecutionContext::create()
/// allocates for realms. It must be popped off the execution context stack before it is dropped.
pub struct OwnedExecutionContext {
    context: NonNull<ExecutionContext>,
}

impl OwnedExecutionContext {
    /// ExecutionContext::create(registers_and_locals_count, constants, arguments_count)
    pub fn create(registers_and_locals_count: u32, constant_count: u32, argument_count: u32) -> Self {
        let slot_count = registers_and_locals_count
            .checked_add(constant_count)
            .and_then(|count| count.checked_add(argument_count))
            .expect("the slot count of an execution context fits in u32");
        let reused_slot_count_index = reused_slot_count_index(slot_count);
        // The free lists are gone once the thread's thread-locals are destroyed, which on macOS happens before C++
        // static destructors run, so a context made or dropped after that is allocated or freed directly.
        let reused_context = reused_slot_count_index.and_then(|index| {
            REUSABLE_EXECUTION_CONTEXTS
                .try_with(|reusable_contexts| reusable_contexts.borrow_mut().0[index].pop())
                .ok()
                .flatten()
        });
        let context = reused_context.unwrap_or_else(|| {
            let allocated_slot_count = reused_slot_count_index.map_or(slot_count, |index| REUSED_SLOT_COUNTS[index]);
            let layout = Self::layout_for_slot_count(allocated_slot_count);
            // SAFETY: The layout has room for at least the context.
            let memory = unsafe { alloc(layout) }.cast::<ExecutionContext>();
            NonNull::new(memory).unwrap_or_else(|| handle_alloc_error(layout))
        });
        // SAFETY: The memory has room for the context and its slots, and nothing else uses it.
        unsafe {
            ExecutionContext::initialize_at(
                context.as_ptr(),
                registers_and_locals_count,
                slot_count,
                argument_count,
                0,
            );
        }
        Self { context }
    }

    fn layout_for_slot_count(slot_count: u32) -> Layout {
        Layout::from_size_align(
            size_of::<ExecutionContext>() + slot_count as usize * size_of::<Value>(),
            align_of::<ExecutionContext>(),
        )
        .expect("the layout of an execution context is valid")
    }

    pub fn as_non_null(&self) -> NonNull<ExecutionContext> {
        self.context
    }

    /// Gives up ownership of the context, which from_raw() takes back.
    pub fn into_raw(self) -> NonNull<ExecutionContext> {
        let context = self.context;
        core::mem::forget(self);
        context
    }

    /// Takes back ownership of a context that into_raw() gave up.
    ///
    /// # Safety
    ///
    /// `context` must come from into_raw(), and nothing may own it already.
    pub unsafe fn from_raw(context: NonNull<ExecutionContext>) -> Self {
        Self { context }
    }
}

// SAFETY: Visits every cell the context reaches.
unsafe impl Trace for OwnedExecutionContext {
    fn trace(&self, visitor: &mut Visitor) {
        (**self).trace(visitor);
    }
}

impl core::ops::Deref for OwnedExecutionContext {
    type Target = ExecutionContext;

    fn deref(&self) -> &ExecutionContext {
        // SAFETY: The context lives until this is dropped.
        unsafe { self.context.as_ref() }
    }
}

impl Drop for OwnedExecutionContext {
    fn drop(&mut self) {
        // Like C++ ExecutionContext::operator delete, this finds the size of the allocation from the context's slot
        // count, which never changes after it is created.
        let slot_count = self.registers_and_constants_and_locals_and_arguments_count.get();
        let allocated_slot_count = match reused_slot_count_index(slot_count) {
            Some(index) => {
                let context = self.context;
                let returned_to_free_list = REUSABLE_EXECUTION_CONTEXTS
                    .try_with(|reusable_contexts| reusable_contexts.borrow_mut().0[index].push(context))
                    .is_ok();
                if returned_to_free_list {
                    return;
                }
                REUSED_SLOT_COUNTS[index]
            }
            None => slot_count,
        };
        // SAFETY: The context was allocated with this layout, and contexts hold nothing that needs dropping.
        unsafe {
            dealloc(
                self.context.as_ptr().cast(),
                Self::layout_for_slot_count(allocated_slot_count),
            );
        }
    }
}
