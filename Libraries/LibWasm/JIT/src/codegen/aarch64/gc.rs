/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Accesses of memories without guard pages, atomics, and GC objects: their addresses and checks, and the tests of
//! references against heap types.

use super::{ADDRESS, CodeGenerator, FP, SCRATCH, gpr};
use crate::asm::aarch64::{AtomicRmw, AtomicSize, Cond, Extend, MemOperand, Shift};
use crate::asm::{Gpr, Label, Width};
use crate::convention::{self, Register};
use crate::ir::{AtomicOp, CHECKED_ATOMIC, CHECKED_MEMORY64, HeapTest, Op, REF_TEST_CAST, REF_TEST_NULLABLE, Value};
use crate::regalloc::{CONSTANT_INPUT, NO_REGISTER};
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
    Immediate(u32),
}

fn field(offset: usize) -> i32 {
    offset as i32
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
                let size = AtomicSize::from_log2(u32::from(node.flags));
                let (address, operand) = (input(0), input(1));
                let old = if record.output == NO_REGISTER {
                    SCRATCH
                } else {
                    gpr(record.output)
                };
                // The acquire-release forms make every atomic access sequentially consistent with the others.
                let (op, source) = match AtomicOp::from_index(node.args[2]) {
                    AtomicOp::Add => (AtomicRmw::Add, operand),
                    AtomicOp::Sub => {
                        self.asm.neg(Width::W64, SCRATCH, operand);
                        (AtomicRmw::Add, SCRATCH)
                    }
                    AtomicOp::And => {
                        self.asm.mvn(Width::W64, SCRATCH, operand);
                        (AtomicRmw::Clear, SCRATCH)
                    }
                    AtomicOp::Or => (AtomicRmw::Set, operand),
                    AtomicOp::Xor => (AtomicRmw::Xor, operand),
                    AtomicOp::Exchange => (AtomicRmw::Swap, operand),
                };
                self.asm.atomic_rmw(op, size, source, old, address);
            }
            Op::AtomicCompareExchange => {
                let size = AtomicSize::from_log2(u32::from(node.flags));
                // NB: Only the low bytes of the expected value are compared, which is the value wrapped at the access
                //     width. The old value comes back zero-extended in its place.
                self.asm.mov(Width::W64, SCRATCH, input(1));
                self.asm.compare_and_swap(size, SCRATCH, input(2), input(0));
                self.asm.mov(Width::W64, gpr(record.output), SCRATCH);
            }
            Op::AtomicFence => self.asm.dmb_ish(),
            Op::Rtt => {
                let (instance, output) = (input(0), gpr(record.output));
                self.asm.ldr(
                    Width::W64,
                    SCRATCH,
                    MemOperand::Offset(instance, field(offset_of!(InstanceData, type_identities))),
                );
                let operand = self.operand(SCRATCH, 4 * i64::from(node.args[1]), 2);
                self.asm.ldr(Width::W32, SCRATCH, operand);
                self.rtts(output, instance);
                self.asm.ldr(
                    Width::W64,
                    output,
                    MemOperand::Register {
                        base: output,
                        index: SCRATCH,
                        extend: Extend::Uxtx,
                        shifted: true,
                    },
                );
            }
            Op::I31New => {
                // NB: Shifting the 32-bit value left drops its bit 31: ((value & 0x7fffffff) << 1) | 1.
                let output = gpr(record.output);
                self.asm.lsl_imm(Width::W32, output, input(0), 1);
                self.asm.orr_imm(Width::W32, output, output, 1);
            }
            Op::I31Get => {
                let output = gpr(record.output);
                if node.flags != 0 {
                    self.asm.asr_imm(Width::W32, output, input(0), 1);
                } else {
                    self.asm.lsr_imm(Width::W32, output, input(0), 1);
                }
            }
            Op::ElementAddress => {
                let (array, index, output) = (input(0), input(1), gpr(record.output));
                let null = self.trap_label(TRAP_NULL_REFERENCE);
                self.asm.cbz(Width::W64, array, null);
                // NB: The index is zero-extended, so a 64-bit comparison checks it as an unsigned i32.
                let operand = self.operand(array, i64::from(field(OBJECT_LENGTH_OFFSET)), 3);
                self.asm.ldr(Width::W64, SCRATCH, operand);
                self.asm.cmp(Width::W64, index, SCRATCH);
                let out_of_bounds = self.trap_label(TRAP_ARRAY_OUT_OF_BOUNDS);
                self.asm.b_cond(Cond::Hs, out_of_bounds);
                let operand = self.operand(array, i64::from(field(OBJECT_DATA_OFFSET)), 3);
                self.asm.ldr(Width::W64, output, operand);
                // Elements take 8 bytes each, or 16 for v128s.
                self.asm
                    .add_extended(Width::W64, output, output, index, Extend::Uxtx, 3);
                if node.flags == 4 {
                    self.asm
                        .add_extended(Width::W64, output, output, index, Extend::Uxtx, 3);
                }
            }
            Op::RefTest => self.ref_test(value),
            _ => unreachable!("{} is no GC or atomic operation", node.op.name()),
        }
    }

    /// `output` = the runtime types of the store, from `instance`.
    fn rtts(&mut self, output: Gpr, instance: Gpr) {
        self.asm.ldr(
            Width::W64,
            output,
            MemOperand::Offset(instance, field(offset_of!(InstanceData, context))),
        );
        self.asm.ldr(
            Width::W64,
            output,
            MemOperand::Offset(output, field(offset_of!(ExecutionContext, rtts))),
        );
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
        if record.inputs[2] == CONSTANT_INPUT {
            let offset = self.immediate(value, 2);
            match offset.checked_add(size) {
                Some(total) if total < (1 << 12) => {
                    self.asm.adds_imm(Width::W64, end, index, total as u32, false);
                }
                Some(total) => {
                    self.asm.mov_imm(Width::W64, ADDRESS, total);
                    self.asm.adds(Width::W64, end, index, ADDRESS, Shift::Lsl, 0);
                }
                None => {
                    // The offset alone is within the size of 2^64: every index wraps around.
                    self.asm.b(out_of_bounds);
                    return;
                }
            }
            if memory64 {
                self.asm.b_cond(Cond::Hs, out_of_bounds);
            }
        } else {
            self.asm
                .adds(Width::W64, end, index, gpr(record.inputs[2]), Shift::Lsl, 0);
            if memory64 {
                self.asm.b_cond(Cond::Hs, out_of_bounds);
            }
            self.asm.adds_imm(Width::W64, end, end, size as u32, false);
            if memory64 {
                self.asm.b_cond(Cond::Hs, out_of_bounds);
            }
        }
        self.asm.ldr(
            Width::W64,
            ADDRESS,
            MemOperand::Offset(memory, field(offset_of!(MemoryData, size))),
        );
        self.asm.cmp(Width::W64, end, ADDRESS);
        self.asm.b_cond(Cond::Hi, out_of_bounds);
        if node.flags & CHECKED_ATOMIC != 0 && size > 1 {
            // NB: The end and the address differ by the size, a multiple of the alignment.
            let unaligned = self.trap_label(TRAP_UNALIGNED_ATOMIC);
            self.asm.tst_imm(Width::W64, end, size - 1);
            self.asm.b_cond(Cond::Ne, unaligned);
        }
        self.asm.ldr(
            Width::W64,
            output,
            MemOperand::Offset(memory, field(offset_of!(MemoryData, base))),
        );
        self.asm.add(Width::W64, output, output, end, Shift::Lsl, 0);
        self.asm.sub_imm(Width::W64, output, output, size as u32, false);
    }

    /// Compares the u32 in `register` with `operand`.
    fn compare32(&mut self, register: Gpr, operand: Operand32) {
        match operand {
            Operand32::Register(other) => self.asm.cmp(Width::W32, register, other),
            Operand32::Immediate(immediate) => self.compare_immediate(Width::W32, register, u64::from(immediate)),
        }
    }

    /// Branches to `failed` unless the type whose Rtt is in `rtt` is the expected type, of `identity` at `depth`, or
    /// a subtype of it: a type's display lists the identities of its supertypes by depth, and its own last. Clobbers
    /// `rtt` and ADDRESS.
    pub(super) fn check_subtype(&mut self, rtt: Gpr, depth: Operand32, identity: Operand32, failed: Label) {
        self.asm.ldr(
            Width::W32,
            ADDRESS,
            MemOperand::Offset(rtt, field(offset_of!(Rtt, depth))),
        );
        self.compare32(ADDRESS, depth);
        self.asm.b_cond(Cond::Lo, failed);
        self.asm.ldr(
            Width::W64,
            rtt,
            MemOperand::Offset(rtt, field(offset_of!(Rtt, display))),
        );
        let entry = match depth {
            Operand32::Register(depth) => MemOperand::Register {
                base: rtt,
                index: depth,
                extend: Extend::Uxtx,
                shifted: true,
            },
            Operand32::Immediate(depth) => self.operand(rtt, 4 * i64::from(depth), 2),
        };
        self.asm.ldr(Width::W32, ADDRESS, entry);
        self.compare32(ADDRESS, identity);
        self.asm.b_cond(Cond::Ne, failed);
    }

    /// `RefTest`: whether a reference matches a heap type, as 0 or 1, or for a cast a trap where it does not.
    fn ref_test(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let reference = gpr(record.inputs[0]);
        let identity: Register = record.inputs[1];
        let depth = node.args[2];
        let cast = node.flags & REF_TEST_CAST != 0;
        let matched = self.asm.new_label();
        let failed = if cast {
            self.trap_label(TRAP_CAST_FAILURE)
        } else {
            self.asm.new_label()
        };
        let null_matches = node.flags & REF_TEST_NULLABLE != 0;
        self.asm
            .cbz(Width::W64, reference, if null_matches { matched } else { failed });
        let identity = if identity == CONSTANT_INPUT {
            Operand32::Immediate(self.immediate(value, 1) as u32)
        } else {
            Operand32::Register(gpr(identity))
        };
        let subtype_check = |generator: &mut Self, rtt: Gpr| {
            generator.check_subtype(rtt, Operand32::Immediate(depth), identity, failed);
        };
        match HeapTest::from_flags(node.flags) {
            HeapTest::NonNull => {}
            HeapTest::Nothing => self.asm.b(failed),
            HeapTest::I31 => {
                self.asm.tbz(reference, 0, failed);
            }
            HeapTest::Eq => {
                // An i31 (bit 0), or a GC object (bits 1:0 clear) rather than a host value (0b10).
                self.asm.tbnz(reference, 0, matched);
                self.asm.tbnz(reference, 1, failed);
            }
            kind @ (HeapTest::Struct | HeapTest::Array) => {
                let rtt = gpr(record.temps[0]);
                self.asm.tst_imm(Width::W64, reference, 3);
                self.asm.b_cond(Cond::Ne, failed);
                self.asm.ldr(Width::W64, rtt, MemOperand::Offset(reference, 0));
                let expected = if kind == HeapTest::Struct {
                    RTT_KIND_STRUCT
                } else {
                    RTT_KIND_ARRAY
                };
                self.asm.ldr(
                    Width::W32,
                    ADDRESS,
                    MemOperand::Offset(rtt, field(offset_of!(Rtt, kind))),
                );
                self.compare_immediate(Width::W32, ADDRESS, u64::from(expected));
                self.asm.b_cond(Cond::Ne, failed);
            }
            HeapTest::ConcreteObject => {
                let rtt = gpr(record.temps[0]);
                self.asm.tst_imm(Width::W64, reference, 3);
                self.asm.b_cond(Cond::Ne, failed);
                self.asm.ldr(Width::W64, rtt, MemOperand::Offset(reference, 0));
                subtype_check(self, rtt);
            }
            HeapTest::ConcreteFunction => {
                let [rtt, rtts] = [gpr(record.temps[0]), gpr(record.temps[1])];
                let type_identity = MemOperand::Offset(reference, field(offset_of!(FunctionHeader, type_identity)));
                self.asm.ldr(Width::W32, ADDRESS, type_identity);
                self.compare32(ADDRESS, identity);
                self.asm.b_cond(Cond::Eq, matched);
                self.asm.ldr(Width::W32, rtt, type_identity);
                self.asm
                    .ldr(Width::W64, rtts, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
                self.rtts(rtts, rtts);
                self.asm.ldr(
                    Width::W64,
                    rtt,
                    MemOperand::Register {
                        base: rtts,
                        index: rtt,
                        extend: Extend::Uxtx,
                        shifted: true,
                    },
                );
                subtype_check(self, rtt);
            }
        }
        self.asm.bind(matched);
        if cast {
            return;
        }
        let output = gpr(record.output);
        let done = self.asm.new_label();
        self.asm.mov_imm(Width::W32, output, 1);
        self.asm.b(done);
        self.asm.bind(failed);
        self.asm.mov_imm(Width::W32, output, 0);
        self.asm.bind(done);
    }
}
