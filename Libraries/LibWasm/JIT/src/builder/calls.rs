/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls, and the instance data they and tables reach.
//!
//! Every call goes through the callee's `FunctionHeader`. A direct call of one of the module's own functions passes
//! the caller's instance and memory base; any other call (of an import, or through a table or reference) passes
//! the callee's instance, found from its header, and that instance's memory base. Host functions have no instance;
//! they get the caller's, through which their stub finds the runtime.

use super::{Builder, instance_offset, ir_type};
use crate::ir::{FLAG_NOT_NULL, Func, NONE, Node, Op, Type, Value};
use libwasm_module::Module;
use libwasm_module::abi::{InstanceData, TRAP_NULL_REFERENCE, TRAP_UNDEFINED_ELEMENT, TRAP_UNINITIALIZED_ELEMENT};
use libwasm_module::types::FuncType;
use std::mem::offset_of;

/// How a call ends the code that makes it.
#[derive(Clone, Copy, PartialEq, Eq)]
pub(super) enum CallKind {
    Normal,
    Tail,
}

impl Builder {
    /// A pointer field of the instance that never changes once it is instantiated.
    pub(super) fn instance_field(&mut self, func: &mut Func, offset: usize) -> Value {
        self.pure(
            func,
            Node::new(
                Op::InstanceField,
                Type::Pointer,
                0,
                [self.instance, instance_offset(offset), 0],
            ),
        )
    }

    /// Element `index` of the pointer array of the instance at `offset`, which never changes either.
    pub(super) fn instance_pointer(&mut self, func: &mut Func, offset: usize, index: u32, ty: Type) -> Value {
        let array = self.instance_field(func, offset);
        self.pure(func, Node::new(Op::LoadPointer, ty, 0, [array, index * 8, 0]))
    }

    /// The store-wide identity of the module's type `type_index`.
    pub(super) fn type_identity(&mut self, func: &mut Func, type_index: u32) -> Value {
        let identities = self.instance_field(func, offset_of!(InstanceData, type_identities));
        self.pure(
            func,
            Node::new(Op::LoadPointer, Type::I32, 4, [identities, type_index * 4, 0]),
        )
    }

    /// The `TableData` of table `table`.
    pub(super) fn table_data(&mut self, func: &mut Func, table: u32) -> Value {
        self.instance_pointer(func, offset_of!(InstanceData, tables), table, Type::Pointer)
    }

    /// The `MemoryData` of memory `memory`.
    pub(super) fn memory_data(&mut self, func: &mut Func, memory: u32) -> Value {
        self.instance_pointer(func, offset_of!(InstanceData, memories), memory, Type::Pointer)
    }

    /// `ref.func`: the function's header, which is what a funcref is.
    pub(super) fn function_reference(&mut self, func: &mut Func, function: u32) -> Value {
        self.instance_pointer(func, offset_of!(InstanceData, functions), function, Type::Ref)
    }

    pub(super) fn call(&mut self, func: &mut Func, module: &Module, function: u32, kind: CallKind) {
        let ty = module.function_type(function).expect("functions have function types");
        let header = self.instance_pointer(func, offset_of!(InstanceData, functions), function, Type::Pointer);
        let (instance, memory_base) = if function < module.imported_function_count {
            self.callee_instance(func, header, true)
        } else {
            (self.instance, self.memory_base)
        };
        self.emit_call(func, ty, [header, instance, memory_base], function, kind);
    }

    pub(super) fn call_indirect(
        &mut self,
        func: &mut Func,
        module: &Module,
        type_index: u32,
        table: u32,
        kind: CallKind,
    ) {
        let ty = module.types[type_index as usize]
            .as_func()
            .expect("call_indirect has a function type");
        let index = func.resolve(self.pop());
        let table = self.table_data(func, table);
        let element = Self::append(
            func,
            Node::new(
                Op::TableElement,
                Type::Ref,
                TRAP_UNDEFINED_ELEMENT as u8,
                [table, index, 0],
            ),
        );
        let identity = self.type_identity(func, type_index);
        Self::append(
            func,
            Node::new(
                Op::CheckFunction,
                Type::None,
                TRAP_UNINITIALIZED_ELEMENT as u8,
                [element, identity, 0],
            ),
        );
        let (instance, memory_base) = self.callee_instance(func, element, false);
        self.emit_call(func, ty, [element, instance, memory_base], NONE, kind);
    }

    pub(super) fn call_ref(&mut self, func: &mut Func, module: &Module, type_index: u32, kind: CallKind) {
        let ty = module.types[type_index as usize]
            .as_func()
            .expect("call_ref has a function type");
        let reference = func.resolve(self.pop());
        Self::append(
            func,
            Node::new(Op::CheckNull, Type::None, TRAP_NULL_REFERENCE as u8, [reference, 0, 0]),
        );
        let (instance, memory_base) = self.callee_instance(func, reference, false);
        self.emit_call(func, ty, [reference, instance, memory_base], NONE, kind);
    }

    /// The instance and memory base a call of the function whose header is `header` passes. Only a header that is
    /// never null is value numbered: any other is read where the call's null check guards it.
    fn callee_instance(&mut self, func: &mut Func, header: Value, not_null: bool) -> (Value, Value) {
        let node = Node::new(
            Op::CalleeInstance,
            Type::Pointer,
            if not_null { FLAG_NOT_NULL } else { 0 },
            [header, self.instance, 0],
        );
        let instance = if not_null {
            self.pure(func, node)
        } else {
            Self::append(func, node)
        };
        let memory_base = self.pure(
            func,
            Node::new(
                Op::InstanceField,
                Type::Pointer,
                0,
                [instance, instance_offset(offset_of!(InstanceData, memory0_base)), 0],
            ),
        );
        (instance, memory_base)
    }

    fn emit_call(
        &mut self,
        func: &mut Func,
        ty: &FuncType,
        [header, instance, memory_base]: [Value; 3],
        function: u32,
        kind: CallKind,
    ) {
        let argument_count = ty.params.len();
        let start = func.operands.len() as u32;
        func.operands.extend_from_slice(&[header, instance, memory_base]);
        let top = self.stack.len() - argument_count;
        for position in top..self.stack.len() {
            let value = func.resolve(self.stack[position]);
            func.operands.push(value);
        }
        self.stack.truncate(top);
        let first_result = ty
            .results
            .first()
            .and_then(|result| ir_type(*result))
            .unwrap_or(Type::None);
        let node = Node::new(Op::Call, first_result, 0, [start, 3 + argument_count as u32, function]);
        if kind == CallKind::Tail {
            self.terminate(
                func,
                Node {
                    op: Op::TailCall,
                    ty: Type::None,
                    ..node
                },
                &[],
            );
            self.set_unreachable();
            return;
        }
        let call = Self::append(func, node);
        if !ty.results.is_empty() {
            self.push(call);
        }
        for (index, &result) in ty.results.iter().enumerate().skip(1) {
            let result_type = ir_type(result).expect("the results' types were checked");
            let value = Self::append(func, Node::new(Op::CallResult, result_type, 0, [call, index as u32, 0]));
            self.push(value);
        }
        self.call_epoch += 1;
        self.check_exception(func, call);
    }
}
