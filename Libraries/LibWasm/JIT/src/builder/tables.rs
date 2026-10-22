/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Tables, the sizes of memories, and the bulk operations on both. Sizes and table elements are read where the code
//! uses them, since growing changes them; the bulk operations and growing call the runtime (`Op::Helper`).

use super::Builder;
use crate::ir::{Func, Helper, Node, Op, Type, Value};
use libwasm_module::Module;
use libwasm_module::abi::{InstanceData, MemoryData, Segment, TRAP_TABLE_OUT_OF_BOUNDS, TableData};
use libwasm_module::opcode::misc;
use libwasm_module::types::AddressType;
use std::mem::offset_of;

const PAGE_SHIFT: u64 = 16;

impl Builder {
    /// A call of the runtime's `helper` with `operands`, whose value has type `ty`.
    pub(super) fn helper(&mut self, func: &mut Func, helper: Helper, operands: &[Value], ty: Type) -> Value {
        let start = func.operands.len() as u32;
        func.operands.push(self.instance);
        for &operand in operands {
            func.operands.push(func.resolve(operand));
        }
        let value = Self::append(
            func,
            Node::new(Op::Helper, ty, 0, [start, 1 + operands.len() as u32, helper as u32]),
        );
        // NB: Growing a memory runs the embedder's callback, which may change globals.
        self.call_epoch += 1;
        value
    }

    /// The address of passive segment `index` in the instance's array of them at `offset`.
    pub(super) fn segment(&mut self, func: &mut Func, offset: usize, index: u32) -> Value {
        let segments = self.instance_field(func, offset);
        self.pure(
            func,
            Node::new(
                Op::Offset,
                Type::Pointer,
                0,
                [segments, index * size_of::<Segment>() as u32, 0],
            ),
        )
    }

    /// A size or previous size of a memory or table of `address_type`, as a value of that type.
    fn address_value(&mut self, func: &mut Func, address_type: AddressType, value: Value) -> Value {
        match address_type {
            AddressType::I32 => self.unary(func, Op::Wrap, Type::I32, value),
            AddressType::I64 => value,
        }
    }

    pub(super) fn memory_size(&mut self, func: &mut Func, module: &Module, memory: u32) {
        let data = self.memory_data(func, memory);
        let size = Self::append(
            func,
            Node::new(
                Op::ReadField,
                Type::I64,
                8,
                [data, offset_of!(MemoryData, size) as u32, 0],
            ),
        );
        let shift = self.constant(func, Type::I64, PAGE_SHIFT);
        let pages = self.binary(func, Op::ShrU, Type::I64, size, shift);
        let value = self.address_value(func, module.memory_space[memory as usize].limits.address_type, pages);
        self.push(value);
    }

    pub(super) fn memory_grow(&mut self, func: &mut Func, module: &Module, memory: u32) {
        let address_type = module.memory_space[memory as usize].limits.address_type;
        let delta = self.pop();
        let data = self.memory_data(func, memory);
        let previous = self.helper(func, Helper::MemoryGrow, &[data, delta], Type::I64);
        let value = self.address_value(func, address_type, previous);
        self.push(value);
    }

    pub(super) fn table_get(&mut self, func: &mut Func, table: u32) {
        let index = func.resolve(self.pop());
        let data = self.table_data(func, table);
        let value = Self::append(
            func,
            Node::new(
                Op::TableElement,
                Type::Ref,
                TRAP_TABLE_OUT_OF_BOUNDS as u8,
                [data, index, 0],
            ),
        );
        self.push(value);
    }

    pub(super) fn table_set(&mut self, func: &mut Func, table: u32) {
        let value = func.resolve(self.pop());
        let index = func.resolve(self.pop());
        let data = self.table_data(func, table);
        Self::append(
            func,
            Node::new(
                Op::TableStore,
                Type::None,
                TRAP_TABLE_OUT_OF_BOUNDS as u8,
                [data, index, value],
            ),
        );
    }

    /// The bulk memory and table instructions of the `0xFC` prefix, with their index immediates.
    pub(super) fn bulk(&mut self, func: &mut Func, module: &Module, sub_opcode: u32, [first, second]: [u32; 2]) {
        match sub_opcode {
            misc::MEMORY_INIT => {
                let (count, source, destination) = (self.pop(), self.pop(), self.pop());
                let segment = self.segment(func, offset_of!(InstanceData, data_segments), first);
                let memory = self.memory_data(func, second);
                self.helper(
                    func,
                    Helper::MemoryInit,
                    &[memory, segment, destination, source, count],
                    Type::None,
                );
            }
            misc::DATA_DROP | misc::ELEM_DROP => {
                let offset = if sub_opcode == misc::DATA_DROP {
                    offset_of!(InstanceData, data_segments)
                } else {
                    offset_of!(InstanceData, element_segments)
                };
                let segments = self.instance_field(func, offset);
                let zero = self.constant(func, Type::I64, 0);
                let field = first * size_of::<Segment>() as u32 + offset_of!(Segment, length) as u32;
                Self::append(func, Node::new(Op::WriteField, Type::None, 8, [segments, zero, field]));
            }
            misc::MEMORY_COPY => {
                let (count, source, destination) = (self.pop(), self.pop(), self.pop());
                let destination_memory = self.memory_data(func, first);
                let source_memory = self.memory_data(func, second);
                self.helper(
                    func,
                    Helper::MemoryCopy,
                    &[destination_memory, source_memory, destination, source, count],
                    Type::None,
                );
            }
            misc::MEMORY_FILL => {
                let (count, value, destination) = (self.pop(), self.pop(), self.pop());
                let memory = self.memory_data(func, first);
                self.helper(
                    func,
                    Helper::MemoryFill,
                    &[memory, destination, value, count],
                    Type::None,
                );
            }
            misc::TABLE_INIT => {
                let (count, source, destination) = (self.pop(), self.pop(), self.pop());
                let segment = self.segment(func, offset_of!(InstanceData, element_segments), first);
                let table = self.table_data(func, second);
                self.helper(
                    func,
                    Helper::TableInit,
                    &[table, segment, destination, source, count],
                    Type::None,
                );
            }
            misc::TABLE_COPY => {
                let (count, source, destination) = (self.pop(), self.pop(), self.pop());
                let destination_table = self.table_data(func, first);
                let source_table = self.table_data(func, second);
                self.helper(
                    func,
                    Helper::TableCopy,
                    &[destination_table, source_table, destination, source, count],
                    Type::None,
                );
            }
            misc::TABLE_GROW => {
                let address_type = module.table_space[first as usize].limits.address_type;
                let (delta, initial) = (self.pop(), self.pop());
                let table = self.table_data(func, first);
                let previous = self.helper(func, Helper::TableGrow, &[table, delta, initial], Type::I64);
                let value = self.address_value(func, address_type, previous);
                self.push(value);
            }
            misc::TABLE_SIZE => {
                let address_type = module.table_space[first as usize].limits.address_type;
                let table = self.table_data(func, first);
                let size = Self::append(
                    func,
                    Node::new(
                        Op::ReadField,
                        Type::I64,
                        8,
                        [table, offset_of!(TableData, size) as u32, 0],
                    ),
                );
                let value = self.address_value(func, address_type, size);
                self.push(value);
            }
            misc::TABLE_FILL => {
                let (count, value, destination) = (self.pop(), self.pop(), self.pop());
                let table = self.table_data(func, first);
                self.helper(func, Helper::TableFill, &[table, destination, value, count], Type::None);
            }
            _ => unreachable!("validated code has no unknown 0xFC instruction {sub_opcode:#x}"),
        }
    }
}
