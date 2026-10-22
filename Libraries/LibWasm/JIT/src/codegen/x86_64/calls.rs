/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls between compiled functions (see `convention`). The allocator moved the arguments into place, so a call
//! only passes the result area, if any, calls through the callee's `FunctionHeader`, and puts rsp back where the
//! frame wants it, which also undoes whatever a callee's tail calls did to it.

use super::{CodeGenerator, FP, SCRATCH, SP, fpr, gpr, precision, width};
use crate::asm::{Address, Gpr, Width};
use crate::convention::{self, Place};
use crate::ir::{FLAG_CHECKS_EXCEPTION, Func, Op, Type, Value};
use libwasm_module::abi::FunctionHeader;
use std::mem::offset_of;

/// The slots of a call's arguments.
fn argument_slots(func: &Func, call: Value) -> u32 {
    func.inputs(call)[3..]
        .iter()
        .map(|argument| func.node(*argument).ty.slots())
        .sum()
}
use crate::regalloc::call_result_types;

impl CodeGenerator<'_> {
    pub(super) fn lower_call(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        match node.op {
            Op::Call => {
                let argument_slots = argument_slots(func, value);
                let results = call_result_types(func, value);
                if convention::has_result_area(&results) {
                    let pointer = 8 * argument_slots as i32;
                    let area = 8 * (argument_slots + 1).next_multiple_of(2) as i32;
                    self.asm.lea(Width::W64, SCRATCH, &Address::new(SP, area));
                    self.asm.mov_mr(Width::W64, &Address::new(SP, pointer), SCRATCH);
                }
                let callee = gpr(convention::CALLEE_REGISTER);
                self.asm
                    .call_m(&Address::new(callee, offset_of!(FunctionHeader, native_entry) as i32));
                if node.flags & FLAG_CHECKS_EXCEPTION != 0 {
                    self.exception_sites.push(self.asm.offset() as u32);
                }
                self.asm
                    .lea(Width::W64, SP, &Address::new(FP, -(self.frame_size as i32)));
            }
            Op::CallResult => {
                let call = node.args[0];
                let index = node.args[1] as usize;
                let results = call_result_types(func, call);
                let mut places = Vec::with_capacity(results.len());
                convention::result_places(&results, &mut places);
                if let Place::Memory(slot) = places[index] {
                    let output = self.allocation.node(value).output;
                    let area = 8 * (argument_slots(func, call) + 1).next_multiple_of(2) as i32;
                    let address = Address::new(SP, area + 8 * slot as i32);
                    if node.ty == Type::V128 {
                        self.load_vector(fpr(output).0, &address);
                    } else if node.ty.is_float() {
                        self.asm.vmovs_rm(precision(node.ty), fpr(output), &address);
                    } else {
                        self.asm.mov_rm(width(node.ty), gpr(output), &address);
                    }
                }
            }
            _ => unreachable!("{} is no call", node.op.name()),
        }
    }

    /// A tail call: the allocator put the arguments where a call passes them, which this copies the stack arguments
    /// from into the incoming area (see `convention`).
    pub(super) fn lower_tail_call(&mut self, value: Value) {
        let func = self.func;
        let parameters = func.inputs(value)[3..]
            .iter()
            .map(|argument| func.node(*argument).ty)
            .collect::<Vec<_>>();
        let has_result_area = convention::has_result_area(&func.results);
        let callee_slots = convention::slots(&parameters);
        // NB: The callee returns the function's results, so both have a result area or neither has. Moving the bottom
        //     of the frame by the difference of their incoming areas, down or up, keeps the area's top in place.
        let needed = convention::incoming_area_slots(&parameters, &func.results);
        let available = convention::incoming_area_slots(&func.params, &func.results);
        let slide = 8 * (needed as i32 - available as i32);
        // NB: The callee's operands are in their registers, which leaves rax and rbx free.
        let (return_address, saved_fp) = (Gpr(convention::RAX), Gpr(convention::RBX));
        if slide != 0 {
            self.asm.mov_rm(
                Width::W64,
                return_address,
                &Address::new(FP, convention::FRAME_RETURN_ADDRESS),
            );
            self.asm
                .mov_rm(Width::W64, saved_fp, &Address::new(FP, convention::FRAME_SAVED_FP));
        }
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
            self.asm.mov_rm(Width::W64, SCRATCH, &Address::new(FP, pointer));
            self.asm
                .mov_mr(Width::W64, &Address::new(SP, 8 * callee_slots as i32), SCRATCH);
            copied.push(callee_slots);
        }
        // The incoming area is above the outgoing one, so copying down from the top never overwrites a source.
        for &slot in copied.iter().rev() {
            self.asm.mov_rm(Width::W64, SCRATCH, &Address::new(SP, 8 * slot as i32));
            self.asm
                .mov_mr(Width::W64, &Address::new(FP, 8 * slot as i32 - slide), SCRATCH);
        }
        if slide != 0 {
            self.asm.mov_mr(
                Width::W64,
                &Address::new(FP, convention::FRAME_RETURN_ADDRESS - slide),
                return_address,
            );
            self.asm.mov_mr(
                Width::W64,
                &Address::new(FP, convention::FRAME_SAVED_FP - slide),
                saved_fp,
            );
        }
        self.asm
            .lea(Width::W64, SP, &Address::new(FP, convention::FRAME_SAVED_FP - slide));
        self.asm.pop(FP);
        let callee = gpr(convention::CALLEE_REGISTER);
        self.asm
            .jmp_m(&Address::new(callee, offset_of!(FunctionHeader, native_entry) as i32));
    }
}
