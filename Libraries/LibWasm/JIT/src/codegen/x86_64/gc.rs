/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Accesses of memories without guard pages, atomics, and GC objects: their addresses and checks, and the tests of
//! references against heap types.

use super::{CodeGenerator, FP, SCRATCH, gpr};
use crate::asm::x86_64::{AluOp, Cond};
use crate::asm::{Address, Gpr, Label, Scale, Width};
use crate::convention::{self, Register};
use crate::ir::{AtomicOp, CHECKED_ATOMIC, CHECKED_MEMORY64, HeapTest, Op, REF_TEST_CAST, REF_TEST_NULLABLE, Value};
use crate::regalloc::CONSTANT_INPUT;
use libwasm_module::abi::{
    ExecutionContext, FunctionHeader, InstanceData, MemoryData, OBJECT_DATA_OFFSET, OBJECT_LENGTH_OFFSET,
    RTT_KIND_ARRAY, RTT_KIND_STRUCT, Rtt, TRAP_ARRAY_OUT_OF_BOUNDS, TRAP_CAST_FAILURE, TRAP_MEMORY_OUT_OF_BOUNDS,
    TRAP_NULL_REFERENCE, TRAP_UNALIGNED_ATOMIC,
};
use std::mem::offset_of;

/// An operand of a 32-bit comparison: a register, or an immediate.
#[derive(Clone, Copy)]
pub(super) enum Operand32 {
    Register(Gpr),
    Immediate(i32),
}

fn field(offset: usize) -> i32 {
    offset as i32
}

/// The width of the instructions for an access of `size` bytes whose value is zero-extended to 32 or 64 bits.
fn access_width(size: u32) -> Width {
    if size == 8 { Width::W64 } else { Width::W32 }
}

impl CodeGenerator<'_> {
    pub(super) fn lower_gc(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let input = |index: usize| gpr(record.inputs[index]);
        match node.op {
            Op::CheckedAddress => self.checked_address(value),
            Op::AtomicRmw => {
                let size = 1u32 << node.flags;
                let (address, operand) = (Address::new(input(0), 0), input(1));
                let old = Gpr(convention::RAX);
                match AtomicOp::from_index(node.args[2]) {
                    op @ (AtomicOp::Add | AtomicOp::Sub) => {
                        self.asm.mov_rr(Width::W64, old, operand);
                        if op == AtomicOp::Sub {
                            self.asm.neg(Width::W64, old);
                        }
                        self.asm.lock_xadd(size, &address, old);
                    }
                    AtomicOp::Exchange => {
                        self.asm.mov_rr(Width::W64, old, operand);
                        self.asm.xchg_mr(size, &address, old);
                    }
                    op => {
                        // A compare-exchange loop, until no other thread changed the location in between.
                        let updated = gpr(record.temps[1]);
                        let alu = match op {
                            AtomicOp::And => AluOp::And,
                            AtomicOp::Or => AluOp::Or,
                            _ => AluOp::Xor,
                        };
                        self.load_zero_extended(size, old, &address);
                        let again = self.asm.new_label();
                        self.asm.bind(again);
                        self.asm.mov_rr(Width::W64, updated, old);
                        self.asm.alu_rr(alu, Width::W64, updated, operand);
                        self.asm.lock_cmpxchg(size, &address, updated);
                        self.asm.jcc(Cond::NotEqual, again);
                    }
                }
                self.zero_extend(size, old);
            }
            Op::AtomicCompareExchange => {
                let size = 1u32 << node.flags;
                // NB: Only the low `size` bytes of rax are compared, which is the expected value wrapped at the
                //     access width.
                self.asm.lock_cmpxchg(size, &Address::new(input(0), 0), input(2));
                self.zero_extend(size, Gpr(convention::RAX));
            }
            Op::AtomicFence => self.asm.mfence(),
            Op::Rtt => {
                let (instance, output) = (input(0), gpr(record.output));
                self.asm.mov_rm(
                    Width::W64,
                    SCRATCH,
                    &Address::new(instance, field(offset_of!(InstanceData, type_identities))),
                );
                self.asm
                    .mov_rm(Width::W32, SCRATCH, &Address::new(SCRATCH, 4 * node.args[1] as i32));
                self.rtts(output, instance);
                self.asm
                    .mov_rm(Width::W64, output, &Address::indexed(output, SCRATCH, Scale::Eight, 0));
            }
            Op::I31New => {
                // NB: Shifting the 32-bit value left drops its bit 31: ((value & 0x7fffffff) << 1) | 1.
                self.asm.lea(
                    Width::W32,
                    gpr(record.output),
                    &Address::indexed(input(0), input(0), Scale::One, 1),
                );
            }
            Op::I31Get => {
                let output = gpr(record.output);
                self.asm.mov_rr(Width::W32, output, input(0));
                if node.flags != 0 {
                    self.asm.sar_ri(Width::W32, output, 1);
                } else {
                    self.asm.shr_ri(Width::W32, output, 1);
                }
            }
            Op::ElementAddress => {
                let (array, index, output) = (input(0), input(1), gpr(record.output));
                let null = self.trap_label(TRAP_NULL_REFERENCE);
                self.asm.test_rr(Width::W64, array, array);
                self.asm.jcc(Cond::Equal, null);
                // NB: The index is zero-extended, so a 64-bit comparison checks it as an unsigned i32.
                self.asm.alu_rm(
                    AluOp::Cmp,
                    Width::W64,
                    index,
                    &Address::new(array, field(OBJECT_LENGTH_OFFSET)),
                );
                let out_of_bounds = self.trap_label(TRAP_ARRAY_OUT_OF_BOUNDS);
                self.asm.jcc(Cond::AboveOrEqual, out_of_bounds);
                self.asm
                    .mov_rm(Width::W64, output, &Address::new(array, field(OBJECT_DATA_OFFSET)));
                self.asm
                    .lea(Width::W64, output, &Address::indexed(output, index, Scale::Eight, 0));
                if node.flags == 4 {
                    self.asm
                        .lea(Width::W64, output, &Address::indexed(output, index, Scale::Eight, 0));
                }
            }
            Op::RefTest => self.ref_test(value),
            _ => unreachable!("{} is no GC or atomic operation", node.op.name()),
        }
    }

    /// `output` = the runtime types of the store, from `instance`.
    fn rtts(&mut self, output: Gpr, instance: Gpr) {
        self.asm.mov_rm(
            Width::W64,
            output,
            &Address::new(instance, field(offset_of!(InstanceData, context))),
        );
        self.asm.mov_rm(
            Width::W64,
            output,
            &Address::new(output, field(offset_of!(ExecutionContext, rtts))),
        );
    }

    fn load_zero_extended(&mut self, size: u32, dst: Gpr, address: &Address) {
        match size {
            1 => self.asm.movzx8_rm(dst, address),
            2 => self.asm.movzx16_rm(dst, address),
            _ => self.asm.mov_rm(access_width(size), dst, address),
        }
    }

    fn zero_extend(&mut self, size: u32, register: Gpr) {
        match size {
            1 => self.asm.movzx8_rr(register, register),
            2 => self.asm.movzx16_rr(register, register),
            4 => self.asm.mov_rr(Width::W32, register, register),
            _ => {}
        }
    }

    /// The address of a checked access: the memory's base + index + offset, after checking that index + offset +
    /// size is at most the memory's size, and for atomics that index + offset is aligned.
    fn checked_address(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let size = 1u64 << (node.flags & 7);
        let memory64 = node.flags & CHECKED_MEMORY64 != 0;
        let (memory, index, output) = (gpr(record.inputs[0]), gpr(record.inputs[1]), gpr(record.output));
        let out_of_bounds = self.trap_label(TRAP_MEMORY_OUT_OF_BOUNDS);
        // The end of the access, which may wrap around for a memory64; i32 indices are zero-extended already.
        let end = SCRATCH;
        self.asm.mov_rr(Width::W64, end, index);
        if record.inputs[2] == CONSTANT_INPUT {
            let offset = self.immediate(value, 2);
            self.asm.add_ri(Width::W64, end, (offset + size) as i32);
            if memory64 {
                self.asm.jcc(Cond::Below, out_of_bounds);
            }
        } else {
            self.asm.add_rr(Width::W64, end, gpr(record.inputs[2]));
            if memory64 {
                self.asm.jcc(Cond::Below, out_of_bounds);
            }
            self.asm.add_ri(Width::W64, end, size as i32);
            if memory64 {
                self.asm.jcc(Cond::Below, out_of_bounds);
            }
        }
        self.asm.alu_rm(
            AluOp::Cmp,
            Width::W64,
            end,
            &Address::new(memory, field(offset_of!(MemoryData, size))),
        );
        self.asm.jcc(Cond::Above, out_of_bounds);
        if node.flags & CHECKED_ATOMIC != 0 && size > 1 {
            // NB: The end and the address differ by the size, a multiple of the alignment.
            let unaligned = self.trap_label(TRAP_UNALIGNED_ATOMIC);
            self.asm.test_ri(Width::W32, end, size as i32 - 1);
            self.asm.jcc(Cond::NotEqual, unaligned);
        }
        self.asm.mov_rm(
            Width::W64,
            output,
            &Address::new(memory, field(offset_of!(MemoryData, base))),
        );
        self.asm.lea(
            Width::W64,
            output,
            &Address::indexed(output, end, Scale::One, -(size as i32)),
        );
    }

    /// `cmp dword [address], operand`.
    fn compare32(&mut self, address: &Address, operand: Operand32) {
        match operand {
            Operand32::Register(register) => self.asm.cmp_mr(Width::W32, address, register),
            Operand32::Immediate(immediate) => self.asm.cmp_mi(Width::W32, address, immediate),
        }
    }

    /// Jumps to `failed` unless the type whose Rtt is in `rtt` is the expected type, of `identity` at `depth`, or a
    /// subtype of it: a type's display lists the identities of its supertypes by depth, and its own last. Clobbers
    /// `rtt`.
    pub(super) fn check_subtype(&mut self, rtt: Gpr, depth: Operand32, identity: Operand32, failed: Label) {
        self.compare32(&Address::new(rtt, field(offset_of!(Rtt, depth))), depth);
        self.asm.jcc(Cond::Below, failed);
        self.asm
            .mov_rm(Width::W64, rtt, &Address::new(rtt, field(offset_of!(Rtt, display))));
        let entry = match depth {
            Operand32::Register(depth) => Address::indexed(rtt, depth, Scale::Four, 0),
            Operand32::Immediate(depth) => Address::new(rtt, 4 * depth),
        };
        self.compare32(&entry, identity);
        self.asm.jcc(Cond::NotEqual, failed);
    }

    /// `RefTest`: whether a reference matches a heap type, as 0 or 1, or for a cast a trap where it does not.
    fn ref_test(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let reference = gpr(record.inputs[0]);
        let identity: Register = record.inputs[1];
        let depth = node.args[2] as i32;
        let cast = node.flags & REF_TEST_CAST != 0;
        let matched = self.asm.new_label();
        let failed = if cast {
            self.trap_label(TRAP_CAST_FAILURE)
        } else {
            self.asm.new_label()
        };
        self.asm.test_rr(Width::W64, reference, reference);
        let null_matches = node.flags & REF_TEST_NULLABLE != 0;
        self.asm.jcc(Cond::Equal, if null_matches { matched } else { failed });
        let identity = if identity == CONSTANT_INPUT {
            Operand32::Immediate(self.immediate(value, 1) as i32)
        } else {
            Operand32::Register(gpr(identity))
        };
        let subtype_check = |generator: &mut Self, rtt: Gpr| {
            generator.check_subtype(rtt, Operand32::Immediate(depth), identity, failed);
        };
        match HeapTest::from_flags(node.flags) {
            HeapTest::NonNull => {}
            HeapTest::Nothing => self.asm.jmp(failed),
            HeapTest::I31 => {
                self.asm.test_ri(Width::W32, reference, 1);
                self.asm.jcc(Cond::Equal, failed);
            }
            HeapTest::Eq => {
                // An i31 (bit 0), or a GC object (bits 1:0 clear) rather than a host value (0b10).
                self.asm.test_ri(Width::W32, reference, 1);
                self.asm.jcc(Cond::NotEqual, matched);
                self.asm.test_ri(Width::W32, reference, 2);
                self.asm.jcc(Cond::NotEqual, failed);
            }
            kind @ (HeapTest::Struct | HeapTest::Array) => {
                let rtt = gpr(record.temps[0]);
                self.asm.test_ri(Width::W32, reference, 3);
                self.asm.jcc(Cond::NotEqual, failed);
                self.asm.mov_rm(Width::W64, rtt, &Address::new(reference, 0));
                let expected = if kind == HeapTest::Struct {
                    RTT_KIND_STRUCT
                } else {
                    RTT_KIND_ARRAY
                };
                self.asm.cmp_mi(
                    Width::W32,
                    &Address::new(rtt, field(offset_of!(Rtt, kind))),
                    expected as i32,
                );
                self.asm.jcc(Cond::NotEqual, failed);
            }
            HeapTest::ConcreteObject => {
                let rtt = gpr(record.temps[0]);
                self.asm.test_ri(Width::W32, reference, 3);
                self.asm.jcc(Cond::NotEqual, failed);
                self.asm.mov_rm(Width::W64, rtt, &Address::new(reference, 0));
                subtype_check(self, rtt);
            }
            HeapTest::ConcreteFunction => {
                let [rtt, rtts] = [gpr(record.temps[0]), gpr(record.temps[1])];
                let type_identity = Address::new(reference, field(offset_of!(FunctionHeader, type_identity)));
                self.compare32(&type_identity, identity);
                self.asm.jcc(Cond::Equal, matched);
                self.asm.mov_rm(Width::W32, rtt, &type_identity);
                self.asm
                    .mov_rm(Width::W64, rtts, &Address::new(FP, convention::FRAME_INSTANCE));
                self.rtts(rtts, rtts);
                self.asm
                    .mov_rm(Width::W64, rtt, &Address::indexed(rtts, rtt, Scale::Eight, 0));
                subtype_check(self, rtt);
            }
        }
        self.asm.bind(matched);
        if cast {
            return;
        }
        let output = gpr(record.output);
        let done = self.asm.new_label();
        self.asm.mov_ri32(output, 1);
        self.asm.jmp(done);
        self.asm.bind(failed);
        self.asm.mov_ri32(output, 0);
        self.asm.bind(done);
    }
}
