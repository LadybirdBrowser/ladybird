/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use crate::gc::visitor::{Trace, Visitor};
use crate::layout::execution_context::{ExecutionContext, ScriptOrModule};
use crate::layout::value::Value;
use libjs_abi::register::RESERVED_REGISTER_COUNT;

impl ExecutionContext {
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
