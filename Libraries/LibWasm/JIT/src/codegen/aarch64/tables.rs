/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runtime data: fields of memories, tables and segments, table elements, the checks of indirect calls, and calls of
//! the runtime's functions.

use super::gc::Operand32;
use super::{ADDRESS, CodeGenerator, FP, SCRATCH, gpr};
use crate::asm::aarch64::{Cond, Extend, MemOperand, XZR};
use crate::asm::{Gpr, Label, Width};
use crate::convention;
use crate::ir::{Helper, Op, Value};
use crate::regalloc::CONSTANT_INPUT;
use libwasm_module::abi::{
    ExecutionContext, FunctionHeader, InstanceData, Rtt, TRAP_INDIRECT_CALL_TYPE_MISMATCH, TableData,
};
use std::mem::offset_of;

/// The out-of-line part of a `CheckFunction`: whether a function whose type is not the expected one has a subtype of
/// it, which only GC modules can declare.
pub(super) struct SubtypeCheck {
    label: Label,
    back: Label,
    reference: Gpr,
    identity: Gpr,
    temps: [Gpr; 2],
}

fn field_width(bytes: u8) -> Width {
    if bytes == 4 { Width::W32 } else { Width::W64 }
}

fn field(offset: usize) -> i32 {
    offset as i32
}

impl CodeGenerator<'_> {
    pub(super) fn lower_runtime(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let input = |index: usize| gpr(record.inputs[index]);
        match node.op {
            Op::Offset => {
                self.asm
                    .add_any(gpr(record.output), input(0), i64::from(node.args[1] as i32), ADDRESS);
            }
            Op::ReadField => {
                let width = field_width(node.flags);
                let operand = self.operand(
                    input(0),
                    i64::from(node.args[1] as i32),
                    if width == Width::W32 { 2 } else { 3 },
                );
                self.asm.ldr(width, gpr(record.output), operand);
            }
            Op::WriteField => {
                let width = field_width(node.flags);
                let source = if record.inputs[1] == CONSTANT_INPUT {
                    let bits = self.immediate(value, 1);
                    if bits == 0 {
                        XZR
                    } else {
                        self.asm.mov_imm(Width::W64, SCRATCH, bits);
                        SCRATCH
                    }
                } else {
                    input(1)
                };
                let operand = self.operand(
                    input(0),
                    i64::from(node.args[2] as i32),
                    if width == Width::W32 { 2 } else { 3 },
                );
                self.asm.str(width, source, operand);
            }
            Op::TableElement | Op::TableStore => {
                let (table, index) = (input(0), input(1));
                self.asm.ldr(
                    Width::W64,
                    SCRATCH,
                    MemOperand::Offset(table, field(offset_of!(TableData, size))),
                );
                self.asm.cmp(Width::W64, index, SCRATCH);
                let out_of_bounds = self.trap_label(u32::from(node.flags));
                self.asm.b_cond(Cond::Hs, out_of_bounds);
                self.asm.ldr(
                    Width::W64,
                    SCRATCH,
                    MemOperand::Offset(table, field(offset_of!(TableData, elements))),
                );
                let element = MemOperand::Register {
                    base: SCRATCH,
                    index,
                    extend: Extend::Uxtx,
                    shifted: true,
                };
                if node.op == Op::TableElement {
                    self.asm.ldr(Width::W64, gpr(record.output), element);
                } else {
                    self.asm.str(Width::W64, input(2), element);
                }
            }
            Op::CalleeInstance => {
                let (header, caller, output) = (input(0), input(1), gpr(record.output));
                self.asm.ldr(
                    Width::W64,
                    SCRATCH,
                    MemOperand::Offset(header, field(offset_of!(FunctionHeader, instance))),
                );
                self.asm.cmp_imm(Width::W64, SCRATCH, 0, false);
                self.asm.csel(Width::W64, output, SCRATCH, caller, Cond::Ne);
            }
            Op::CheckNull => {
                let null = self.trap_label(u32::from(node.flags));
                self.asm.cbz(Width::W64, input(0), null);
            }
            Op::CheckFunction => {
                let (reference, identity) = (input(0), input(1));
                let null = self.trap_label(u32::from(node.flags));
                self.asm.cbz(Width::W64, reference, null);
                self.asm.ldr(
                    Width::W32,
                    SCRATCH,
                    MemOperand::Offset(reference, field(offset_of!(FunctionHeader, type_identity))),
                );
                self.asm.cmp(Width::W32, SCRATCH, identity);
                let label = self.asm.new_label();
                let back = self.asm.new_label();
                self.asm.b_cond(Cond::Ne, label);
                self.asm.bind(back);
                self.subtype_checks.push(SubtypeCheck {
                    label,
                    back,
                    reference,
                    identity,
                    temps: [gpr(record.temps[0]), gpr(record.temps[1])],
                });
            }
            Op::Helper => {
                let helper = Helper::from_index(node.args[2]);
                let context = Gpr(convention::HELPER_REGISTERS[0]);
                self.asm.ldr(
                    Width::W64,
                    context,
                    MemOperand::Offset(context, field(offset_of!(InstanceData, context))),
                );
                let arguments = offset_of!(ExecutionContext, arguments) as i32;
                for index in 1..func.inputs(value).len() {
                    self.asm.str(
                        Width::W64,
                        Gpr(convention::HELPER_REGISTERS[index]),
                        MemOperand::Offset(context, arguments + 8 * (index as i32 - 1)),
                    );
                }
                self.asm.mov_imm(
                    Width::W64,
                    SCRATCH,
                    crate::runtime::helper_address(helper, self.runtime),
                );
                self.asm.blr(SCRATCH);
                let result = Gpr(convention::GPR_RESULTS[0]);
                if helper.may_trap() {
                    let trap = self.dynamic_trap_label();
                    self.asm.cbnz(Width::W64, result, trap);
                } else if helper.traps_on_zero() {
                    let trap = self.trap_label(libwasm_module::abi::TRAP_RUNTIME);
                    self.asm.cbz(Width::W64, result, trap);
                } else if let Some(value) = helper.trap_value() {
                    let trap = self.trap_label(libwasm_module::abi::TRAP_RUNTIME);
                    self.asm.cmp_imm(Width::W64, result, value as u32, false);
                    self.asm.b_cond(crate::asm::aarch64::Cond::Eq, trap);
                }
            }
            _ => unreachable!("{} is no runtime operation", node.op.name()),
        }
    }

    pub(super) fn subtype_checks(&mut self) {
        let checks = std::mem::take(&mut self.subtype_checks);
        if checks.is_empty() {
            return;
        }
        let mismatch = self.trap_label(TRAP_INDIRECT_CALL_TYPE_MISMATCH);
        for check in checks {
            let [rtts, actual] = check.temps;
            self.asm.bind(check.label);
            self.asm
                .ldr(Width::W64, rtts, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
            self.asm.ldr(
                Width::W64,
                rtts,
                MemOperand::Offset(rtts, field(offset_of!(InstanceData, context))),
            );
            self.asm.ldr(
                Width::W64,
                rtts,
                MemOperand::Offset(rtts, field(offset_of!(ExecutionContext, rtts))),
            );
            self.asm.ldr(
                Width::W32,
                SCRATCH,
                MemOperand::Offset(check.reference, field(offset_of!(FunctionHeader, type_identity))),
            );
            let indexed = |index: Gpr, shift: bool| MemOperand::Register {
                base: rtts,
                index,
                extend: Extend::Uxtx,
                shifted: shift,
            };
            self.asm.ldr(Width::W64, actual, indexed(SCRATCH, true));
            // The expected type's Rtt, by the identity, which is zero-extended in its register.
            let target = rtts;
            self.asm.ldr(Width::W64, target, indexed(check.identity, true));
            self.asm.ldr(
                Width::W32,
                SCRATCH,
                MemOperand::Offset(target, field(offset_of!(Rtt, depth))),
            );
            self.check_subtype(
                actual,
                Operand32::Register(SCRATCH),
                Operand32::Register(check.identity),
                mismatch,
            );
            self.asm.b(check.back);
        }
    }
}
