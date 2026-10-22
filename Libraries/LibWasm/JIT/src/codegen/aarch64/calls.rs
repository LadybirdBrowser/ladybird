/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls between compiled functions (see `convention`). The allocator moved the arguments into place, so a call
//! only passes the result area, if any, calls through the callee's `FunctionHeader`, and puts sp back where the
//! frame wants it, which also undoes whatever a callee's tail calls did to it.

use super::{ADDRESS, CodeGenerator, FP, LR, SCRATCH, SP, fpr, fpr_access, gpr, gpr_access};
use crate::asm::aarch64::{MemOperand, PairOperand};
use crate::asm::{Gpr, Width};
use crate::convention::{self, Place};
use crate::ir::{FLAG_CHECKS_EXCEPTION, Func, Op, Type, Value};
use crate::regalloc::call_result_types;
use libwasm_module::abi::FunctionHeader;
use std::mem::offset_of;

/// The slots of a call's arguments.
fn argument_slots(func: &Func, call: Value) -> u32 {
    func.inputs(call)[3..]
        .iter()
        .map(|argument| func.node(*argument).ty.slots())
        .sum()
}

impl CodeGenerator<'_> {
    pub(super) fn lower_call(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        match node.op {
            Op::Call => {
                let argument_slots = argument_slots(func, value);
                let results = call_result_types(func, value);
                if convention::has_result_area(&results) {
                    let pointer = 8 * i64::from(argument_slots);
                    let area = 8 * i64::from((argument_slots + 1).next_multiple_of(2));
                    self.asm.add_any(SCRATCH, SP, area, ADDRESS);
                    let operand = self.operand(SP, pointer, 3);
                    self.asm.str(Width::W64, SCRATCH, operand);
                }
                let callee = gpr(convention::CALLEE_REGISTER);
                self.asm.ldr(
                    Width::W64,
                    SCRATCH,
                    MemOperand::Offset(callee, offset_of!(FunctionHeader, native_entry) as i32),
                );
                self.asm.blr(SCRATCH);
                if node.flags & FLAG_CHECKS_EXCEPTION != 0 {
                    self.exception_sites.push(self.asm.offset() as u32);
                }
                self.reset_sp();
            }
            Op::CallResult => {
                let call = node.args[0];
                let index = node.args[1] as usize;
                let results = call_result_types(func, call);
                let mut places = Vec::with_capacity(results.len());
                convention::result_places(&results, &mut places);
                if let Place::Memory(slot) = places[index] {
                    let output = self.allocation.node(value).output;
                    let area = 8 * i64::from((argument_slots(func, call) + 1).next_multiple_of(2));
                    let size_log2 = if node.ty == Type::V128 { 4 } else { 3 };
                    let operand = self.operand(SP, area + 8 * i64::from(slot), size_log2);
                    if node.ty.uses_xmm() {
                        self.asm.load_store(fpr_access(size_log2, true), fpr(output).0, operand);
                    } else {
                        self.asm.load_store(gpr_access(size_log2, true), gpr(output).0, operand);
                    }
                }
            }
            _ => unreachable!("{} is no call", node.op.name()),
        }
    }

    /// A tail call: the allocator put the arguments where a call passes them, which this copies the stack arguments
    /// from into the incoming area (see `convention`). The return address stays in the link register, so only sp
    /// moves where the callee's incoming area differs from the function's.
    pub(super) fn lower_tail_call(&mut self, value: Value) {
        let func = self.func;
        let parameters = func.inputs(value)[3..]
            .iter()
            .map(|argument| func.node(*argument).ty)
            .collect::<Vec<_>>();
        let has_result_area = convention::has_result_area(&func.results);
        let callee_slots = convention::slots(&parameters);
        // NB: The callee returns the function's results, so both have a result area or neither has. Moving sp by the
        //     difference of their incoming areas, down or up, keeps the area's top in place.
        let needed = convention::incoming_area_slots(&parameters, &func.results);
        let available = convention::incoming_area_slots(&func.params, &func.results);
        let slide = 8 * (i64::from(needed) - i64::from(available));
        // SECURITY: The frame record at fp - 16 is in the incoming area once it grows, so the saved fp and the return
        //           address come out before the arguments go in. The callee's operands are in their registers, which
        //           leaves x9 free.
        let saved_fp = Gpr(9);
        self.asm
            .ldp(saved_fp, LR, PairOperand::Offset(FP, convention::FRAME_SAVED_FP));
        let mut copied = Vec::new();
        let mut places = Vec::new();
        convention::parameter_places(&parameters, &mut places);
        for (index, place) in places.iter().enumerate() {
            if let Place::Memory(slot) = *place {
                copied.extend(slot..slot + parameters[index].slots());
            }
        }
        if has_result_area {
            // The function's caller gets the callee's results.
            let pointer = convention::home_slot_offset(convention::result_area_pointer(&func.params));
            let operand = self.operand(FP, i64::from(pointer), 3);
            self.asm.ldr(Width::W64, SCRATCH, operand);
            let operand = self.operand(SP, 8 * i64::from(callee_slots), 3);
            self.asm.str(Width::W64, SCRATCH, operand);
            copied.push(callee_slots);
        }
        // The incoming area is above the outgoing one, so copying down from the top never overwrites a source.
        for &slot in copied.iter().rev() {
            let operand = self.operand(SP, 8 * i64::from(slot), 3);
            self.asm.ldr(Width::W64, SCRATCH, operand);
            let operand = self.operand(FP, 8 * i64::from(slot) - slide, 3);
            self.asm.str(Width::W64, SCRATCH, operand);
        }
        // sp goes where the function's caller had it, less the slide, and fp back to the caller's.
        self.asm.add_any(SP, FP, -slide, SCRATCH);
        self.asm.mov(Width::W64, FP, saved_fp);
        let callee = gpr(convention::CALLEE_REGISTER);
        self.asm.ldr(
            Width::W64,
            SCRATCH,
            MemOperand::Offset(callee, offset_of!(FunctionHeader, native_entry) as i32),
        );
        self.asm.br(SCRATCH);
    }
}
