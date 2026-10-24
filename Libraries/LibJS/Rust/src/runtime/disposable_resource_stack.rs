/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, class_of, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::{CellHeader, Gc};
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    DisposeCapability, add_disposable_resource, dispose_resources, new_dispose_capability,
};
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::environment::InitializeBindingHint;
use libjs_abi::value as nan_box;

/// The [[DisposableResourceStack]] of a Block, ForStatement, ForInOfStatement iteration, function body or module that
/// contains `using` declarations. The bytecode keeps it in a register rather than in an environment, since the
/// bindings of these declarations are often locals that live in no environment.
#[repr(C)]
#[derive(Trace)]
pub struct DisposableResourceStack {
    header: CellHeader,
    dispose_capability: GcRefCell<DisposeCapability>,
}

define_cell!(DisposableResourceStack, Other, finalize: finalize);

impl Finalize for DisposableResourceStack {
    fn finalize(&self) {
        drop(self.dispose_capability.replace(new_dispose_capability()));
    }
}

impl DisposableResourceStack {
    pub fn create(vm: &Vm) -> Gc<DisposableResourceStack> {
        vm.heap().allocate(DisposableResourceStack {
            header: CellHeader::for_class(Self::CLASS),
            dispose_capability: GcRefCell::new(new_dispose_capability()),
        })
    }

    /// The stack as the interpreter keeps it in a register: a cell value without a more specific tag.
    pub fn to_value(stack: Gc<DisposableResourceStack>) -> Value {
        Value::with_cell_tag(nan_box::IS_CELL_BIT, stack)
    }

    /// The stack a register holds, which must be one.
    pub fn from_value(value: Value) -> Gc<DisposableResourceStack> {
        assert!(value.is_cell());
        // SAFETY: The value holds a cell, and every cell starts with its class, which is checked below.
        let stack = unsafe { value.cell::<DisposableResourceStack>() };
        assert!(
            class_of(stack).is_subclass_of(Self::CLASS),
            "the cell is not a disposable resource stack"
        );
        stack
    }

    pub fn add_disposable_resource(&self, vm: &Vm, value: Value, hint: InitializeBindingHint) -> ThrowCompletionOr<()> {
        add_disposable_resource(vm, &self.dispose_capability, value, hint, None)
    }

    pub fn dispose_resources(&self, vm: &Vm, completion: Completion) -> Completion {
        // NB: Every resource of the stack has the sync-dispose kind, so this never awaits.
        dispose_resources(vm, &self.dispose_capability, completion)
    }
}
