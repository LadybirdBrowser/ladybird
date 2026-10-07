/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::alloc::{Layout, alloc, dealloc};

use core::cell::Cell;
use core::ptr::NonNull;

use crate::layout::execution_context::ExecutionContext;
use crate::layout::value::Value;
use crate::layout::vm::InterpreterStack;

/// The memory execution contexts are bump-allocated from, by the runtime and by the interpreter itself.
pub struct InterpreterStackMemory {
    memory: NonNull<u8>,
}

impl InterpreterStackMemory {
    const SIZE: usize = 8 * 1024 * 1024;
    const LAYOUT: Layout = match Layout::from_size_align(Self::SIZE, align_of::<ExecutionContext>()) {
        Ok(layout) => layout,
        Err(_) => panic!("the interpreter stack layout is valid"),
    };

    pub fn allocate() -> Self {
        // SAFETY: The layout has a non-zero size.
        let memory = unsafe { alloc(Self::LAYOUT) };
        Self {
            memory: NonNull::new(memory).expect("allocate the interpreter stack"),
        }
    }

    pub fn initial_state(&self) -> InterpreterStack {
        let base = self.memory.as_ptr();
        InterpreterStack {
            base: Cell::new(base),
            top: Cell::new(base),
            // SAFETY: The end of the allocation is in bounds.
            limit: Cell::new(unsafe { base.add(Self::SIZE) }),
            next_frame_id: Cell::new(1),
        }
    }
}

impl Drop for InterpreterStackMemory {
    fn drop(&mut self) {
        // SAFETY: The memory was allocated with this layout.
        unsafe { dealloc(self.memory.as_ptr(), Self::LAYOUT) };
    }
}

impl InterpreterStack {
    pub fn is_exhausted(&self) -> bool {
        self.top.get() >= self.limit.get()
    }

    /// Allocates a context whose frame has room for `registers_and_locals_count` registers and locals, `constants`
    /// constants and `argument_count` arguments, or returns None if the stack is full.
    pub fn allocate(
        &self,
        registers_and_locals_count: u32,
        constant_count: u32,
        argument_count: u32,
    ) -> Option<NonNull<ExecutionContext>> {
        let slot_count = registers_and_locals_count
            .checked_add(constant_count)?
            .checked_add(argument_count)?;
        let size = size_of::<ExecutionContext>() + slot_count as usize * size_of::<Value>();
        let size = size.next_multiple_of(align_of::<ExecutionContext>());
        let top = self.top.get();
        if (self.limit.get() as usize) - (top as usize) < size {
            return None;
        }
        let frame_id = self.next_frame_id.get();
        self.next_frame_id.set(frame_id + 1);
        let context = top.cast::<ExecutionContext>();
        // SAFETY: The frame fits between top and limit, and nothing else uses that memory.
        unsafe {
            ExecutionContext::initialize_at(
                context,
                registers_and_locals_count,
                slot_count,
                argument_count,
                frame_id,
            );
            self.top.set(top.add(size));
            Some(NonNull::new_unchecked(context))
        }
    }

    /// Frees every context allocated after `mark` was taken.
    pub fn deallocate(&self, mark: *mut u8) {
        assert!(mark >= self.base.get() && mark <= self.top.get());
        self.top.set(mark);
    }
}
