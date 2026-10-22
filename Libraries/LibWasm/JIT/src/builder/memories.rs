/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Accesses of memories without guard pages (memory64, and memories other than memory 0), and atomics. Both access
//! the memory at a `CheckedAddress`, which checks the access against the memory's current size; atomic accesses are
//! checked for alignment too, whatever memory they access.

use super::{Builder, load_kind, store_size_log2};
use crate::decode::{Decoder, MemoryArgument};
use crate::ir::{
    AtomicOp, CHECKED_ATOMIC, CHECKED_MEMORY64, Func, Helper, LOAD_ATOMIC, LoadKind, Node, Op, Type, Value,
};
use libwasm_module::Module;
use libwasm_module::abi::TRAP_EXPECTED_SHARED_MEMORY;
use libwasm_module::instruction_tables::{AtomicKind, atomic_kind};
use libwasm_module::opcode::atomic;
use libwasm_module::types::{AddressType, ValType};

impl Builder {
    /// The address of an access of `1 << size_log2` bytes at `index` + `argument.offset` in `argument.memory`, after
    /// checking it (`Op::CheckedAddress`).
    pub(super) fn checked_address(
        &mut self,
        func: &mut Func,
        module: &Module,
        argument: MemoryArgument,
        index: Value,
        size_log2: u8,
        atomic: bool,
    ) -> Value {
        let memory64 = module.memory_space[argument.memory as usize].limits.address_type == AddressType::I64;
        let index = func.resolve(index);
        let data = self.memory_data(func, argument.memory);
        let offset = self.constant(func, Type::I64, argument.offset);
        let mut flags = size_log2;
        if atomic {
            flags |= CHECKED_ATOMIC;
        }
        if memory64 {
            flags |= CHECKED_MEMORY64;
        }
        Self::append(
            func,
            Node::new(Op::CheckedAddress, Type::Pointer, flags, [data, index, offset]),
        )
    }

    /// A load of `kind` at `address`, a pointer.
    pub(super) fn load_at(&mut self, func: &mut Func, kind: LoadKind, ty: Type, address: Value, offset: u32) -> Value {
        let zero = self.constant(func, Type::I32, 0);
        Self::append(func, Node::new(Op::Load, ty, kind as u8, [address, zero, offset]))
    }

    /// A store of `1 << size_log2` bytes of `value` at `address`, a pointer.
    pub(super) fn store_at(&mut self, func: &mut Func, size_log2: u8, address: Value, offset: u32, value: Value) {
        let zero = self.constant(func, Type::I32, 0);
        let value = func.resolve(value);
        let start = func.operands.len() as u32;
        func.operands.extend_from_slice(&[address, zero, value]);
        Self::append(func, Node::new(Op::Store, Type::None, size_log2, [start, 3, offset]));
    }

    /// A core load or store in its bounds-checked form (quickening rule 1).
    pub(super) fn checked_access(&mut self, func: &mut Func, module: &Module, opcode: u8, argument: MemoryArgument) {
        if opcode <= libwasm_module::opcode::I64_LOAD32_U {
            let (kind, ty) = load_kind(opcode);
            let index = self.pop();
            let size_log2 = kind.size().trailing_zeros() as u8;
            let address = self.checked_address(func, module, argument, index, size_log2, false);
            let value = self.load_at(func, kind, ty, address, 0);
            self.push(value);
        } else {
            let value = self.pop();
            let index = self.pop();
            let size_log2 = store_size_log2(opcode);
            let address = self.checked_address(func, module, argument, index, size_log2, false);
            self.store_at(func, size_log2, address, 0, value);
        }
    }

    /// The instructions of the `0xFE` prefix.
    pub(super) fn atomic(&mut self, func: &mut Func, module: &Module, sub_opcode: u32, decoder: &Decoder<'_>) {
        let argument = decoder.atomic_argument();
        let kind = atomic_kind(sub_opcode).expect("validated code has no unknown atomic instructions");
        let ir = |value: ValType| if value == ValType::I64 { Type::I64 } else { Type::I32 };
        match kind {
            AtomicKind::Fence => {
                Self::append(func, Node::new(Op::AtomicFence, Type::None, 0, [0; 3]));
            }
            AtomicKind::Notify => {
                let count = self.pop();
                let index = self.pop();
                let address = self.checked_address(func, module, argument, index, 2, true);
                // Only a shared memory can have threads waiting on it.
                let woken = if module.memory_space[argument.memory as usize].limits.shared {
                    self.helper(func, Helper::AtomicNotify, &[address, count], Type::I32)
                } else {
                    self.constant(func, Type::I32, 0)
                };
                self.push(woken);
            }
            AtomicKind::Wait { value } => {
                let timeout = self.pop();
                let expected = self.pop();
                let index = self.pop();
                let size_log2 = if value == ValType::I64 { 3 } else { 2 };
                let address = self.checked_address(func, module, argument, index, size_log2, true);
                if !module.memory_space[argument.memory as usize].limits.shared {
                    self.terminate(
                        func,
                        Node::new(Op::Trap, Type::None, 0, [TRAP_EXPECTED_SHARED_MEMORY, 0, 0]),
                        &[],
                    );
                    self.set_unreachable();
                    return;
                }
                let size = self.constant(func, Type::I64, 1 << size_log2);
                let result = self.helper(func, Helper::AtomicWait, &[address, expected, timeout, size], Type::I32);
                self.push(result);
            }
            AtomicKind::Load { value, size } => {
                let index = self.pop();
                let size_log2 = size.trailing_zeros() as u8;
                let address = self.checked_address(func, module, argument, index, size_log2, true);
                let kind = match (value, size) {
                    (ValType::I64, 8) => LoadKind::I64,
                    (ValType::I64, 4) => LoadKind::I64U32,
                    (ValType::I64, 2) => LoadKind::I64U16,
                    (ValType::I64, _) => LoadKind::I64U8,
                    (_, 4) => LoadKind::I32,
                    (_, 2) => LoadKind::I32U16,
                    _ => LoadKind::I32U8,
                };
                // NB: Plain loads are sequentially consistent on x86-64, where stores are locked exchanges; AArch64
                //     loads them with acquire semantics.
                let zero = self.constant(func, Type::I32, 0);
                let loaded = Self::append(
                    func,
                    Node::new(Op::Load, ir(value), kind as u8 | LOAD_ATOMIC, [address, zero, 0]),
                );
                self.push(loaded);
            }
            AtomicKind::Store { value, size } => {
                let stored = func.resolve(self.pop());
                let index = self.pop();
                let size_log2 = size.trailing_zeros() as u8;
                let address = self.checked_address(func, module, argument, index, size_log2, true);
                // An exchange is a sequentially consistent store.
                Self::append(
                    func,
                    Node::new(
                        Op::AtomicRmw,
                        ir(value),
                        size_log2,
                        [address, stored, AtomicOp::Exchange as u32],
                    ),
                );
            }
            AtomicKind::ReadModifyWrite { value, size } => {
                let operand = func.resolve(self.pop());
                let index = self.pop();
                let size_log2 = size.trailing_zeros() as u8;
                let address = self.checked_address(func, module, argument, index, size_log2, true);
                let op = AtomicOp::from_index((sub_opcode - atomic::I32_ATOMIC_RMW_ADD) / 7);
                let old = Self::append(
                    func,
                    Node::new(Op::AtomicRmw, ir(value), size_log2, [address, operand, op as u32]),
                );
                self.push(old);
            }
            AtomicKind::CompareExchange { value, size } => {
                let replacement = func.resolve(self.pop());
                let expected = func.resolve(self.pop());
                let index = self.pop();
                let size_log2 = size.trailing_zeros() as u8;
                let address = self.checked_address(func, module, argument, index, size_log2, true);
                let old = Self::append(
                    func,
                    Node::new(
                        Op::AtomicCompareExchange,
                        ir(value),
                        size_log2,
                        [address, expected, replacement],
                    ),
                );
                self.push(old);
            }
        }
    }
}
