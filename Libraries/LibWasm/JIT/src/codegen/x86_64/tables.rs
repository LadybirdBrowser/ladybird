/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Runtime data: fields of memories, tables and segments, table elements, the checks of indirect calls, and calls of
//! the runtime's functions.

use super::gc::Operand32;
use super::{CodeGenerator, FP, SCRATCH, gpr};
use crate::asm::x86_64::AluOp;
use crate::asm::x86_64::Cond;
use crate::asm::{Address, Gpr, Label, Scale, Width};
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

impl CodeGenerator<'_> {
    pub(super) fn lower_runtime(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        let input = |index: usize| gpr(record.inputs[index]);
        match node.op {
            Op::Offset => {
                self.asm.lea(
                    Width::W64,
                    gpr(record.output),
                    &Address::new(input(0), node.args[1] as i32),
                );
            }
            Op::ReadField => {
                self.asm.mov_rm(
                    field_width(node.flags),
                    gpr(record.output),
                    &Address::new(input(0), node.args[1] as i32),
                );
            }
            Op::WriteField => {
                let address = Address::new(input(0), node.args[2] as i32);
                let width = field_width(node.flags);
                if record.inputs[1] == CONSTANT_INPUT {
                    let bits = self.immediate(value, 1);
                    self.asm.mov_mi(width, &address, bits as i32);
                } else {
                    self.asm.mov_mr(width, &address, input(1));
                }
            }
            Op::TableElement | Op::TableStore => {
                let (table, index) = (input(0), input(1));
                self.asm.alu_rm(
                    AluOp::Cmp,
                    Width::W64,
                    index,
                    &Address::new(table, offset_of!(TableData, size) as i32),
                );
                let out_of_bounds = self.trap_label(u32::from(node.flags));
                self.asm.jcc(Cond::AboveOrEqual, out_of_bounds);
                self.asm.mov_rm(
                    Width::W64,
                    SCRATCH,
                    &Address::new(table, offset_of!(TableData, elements) as i32),
                );
                let element = Address::indexed(SCRATCH, index, Scale::Eight, 0);
                if node.op == Op::TableElement {
                    self.asm.mov_rm(Width::W64, gpr(record.output), &element);
                } else {
                    self.asm.mov_mr(Width::W64, &element, input(2));
                }
            }
            Op::CalleeInstance => {
                let (header, caller, output) = (input(0), input(1), gpr(record.output));
                self.asm.mov_rm(
                    Width::W64,
                    SCRATCH,
                    &Address::new(header, offset_of!(FunctionHeader, instance) as i32),
                );
                if output != caller {
                    self.asm.mov_rr(Width::W64, output, caller);
                }
                self.asm.test_rr(Width::W64, SCRATCH, SCRATCH);
                self.asm.cmovcc(Cond::NotEqual, Width::W64, output, SCRATCH);
            }
            Op::CheckNull => {
                let null = self.trap_label(u32::from(node.flags));
                self.asm.test_rr(Width::W64, input(0), input(0));
                self.asm.jcc(Cond::Equal, null);
            }
            Op::CheckFunction => {
                let (reference, identity) = (input(0), input(1));
                let null = self.trap_label(u32::from(node.flags));
                self.asm.test_rr(Width::W64, reference, reference);
                self.asm.jcc(Cond::Equal, null);
                self.asm.cmp_mr(
                    Width::W32,
                    &Address::new(reference, offset_of!(FunctionHeader, type_identity) as i32),
                    identity,
                );
                let label = self.asm.new_label();
                let back = self.asm.new_label();
                self.asm.jcc(Cond::NotEqual, label);
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
                let context = Gpr(convention::RDI);
                self.asm.mov_rm(
                    Width::W64,
                    context,
                    &Address::new(context, offset_of!(InstanceData, context) as i32),
                );
                let arguments = offset_of!(ExecutionContext, arguments) as i32;
                for index in 1..func.inputs(value).len() {
                    self.asm.mov_mr(
                        Width::W64,
                        &Address::new(context, arguments + 8 * (index as i32 - 1)),
                        Gpr(convention::HELPER_REGISTERS[index]),
                    );
                }
                let target = Gpr(convention::RAX);
                self.asm
                    .mov_ri(target, crate::runtime::helper_address(helper, self.runtime));
                self.asm.call_r(target);
                if helper.may_trap() {
                    let trap = self.dynamic_trap_label();
                    self.asm.test_rr(Width::W64, target, target);
                    self.asm.jcc(Cond::NotEqual, trap);
                } else if helper.traps_on_zero() {
                    let trap = self.trap_label(libwasm_module::abi::TRAP_RUNTIME);
                    self.asm.test_rr(Width::W64, target, target);
                    self.asm.jcc(Cond::Equal, trap);
                } else if let Some(value) = helper.trap_value() {
                    let trap = self.trap_label(libwasm_module::abi::TRAP_RUNTIME);
                    self.asm.cmp_ri(Width::W64, target, value as i32);
                    self.asm.jcc(Cond::Equal, trap);
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
                .mov_rm(Width::W64, rtts, &Address::new(FP, convention::FRAME_INSTANCE));
            self.asm.mov_rm(
                Width::W64,
                rtts,
                &Address::new(rtts, offset_of!(InstanceData, context) as i32),
            );
            self.asm.mov_rm(
                Width::W64,
                rtts,
                &Address::new(rtts, offset_of!(ExecutionContext, rtts) as i32),
            );
            self.asm.mov_rm(
                Width::W32,
                SCRATCH,
                &Address::new(check.reference, offset_of!(FunctionHeader, type_identity) as i32),
            );
            self.asm
                .mov_rm(Width::W64, actual, &Address::indexed(rtts, SCRATCH, Scale::Eight, 0));
            self.asm.mov_rr(Width::W32, SCRATCH, check.identity);
            let target = rtts;
            self.asm
                .mov_rm(Width::W64, target, &Address::indexed(rtts, SCRATCH, Scale::Eight, 0));
            self.asm.mov_rm(
                Width::W32,
                SCRATCH,
                &Address::new(target, offset_of!(Rtt, depth) as i32),
            );
            self.check_subtype(
                actual,
                Operand32::Register(SCRATCH),
                Operand32::Register(check.identity),
                mismatch,
            );
            self.asm.jmp(check.back);
        }
    }
}
