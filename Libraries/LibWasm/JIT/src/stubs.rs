/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Code that is not a compiled function but is called like one. A host function's `native_entry` is the host stub of
//! its signature, which lays the arguments out as the interpreter's operand stack would hold them, calls the host
//! function through the runtime's `wasm_runtime_call_host` like the interpreter does, and returns its results under the compiled
//! convention.

use crate::RuntimeInfo;
use crate::asm::x86_64::{Assembler, Cond, Precision, VexMap, VexOpcode, VexOperand, VexPrefix};
use crate::asm::{Address, Fpr, Gpr, Width};
use crate::convention::{self, Place, Register};
use crate::ir::Type;
use libwasm_module::abi::{ExecutionContext, InstanceData, TRAP_RUNTIME, TRAP_STACK_EXHAUSTED};
use std::mem::offset_of;

const VMOVUPS_LOAD: VexOpcode = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x10);
const VMOVUPS_STORE: VexOpcode = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x11);

const FP: Gpr = Gpr(convention::RBP);
const SP: Gpr = Gpr(convention::RSP);
const SCRATCH: Gpr = Gpr(convention::SCRATCH_GPR);
const SCRATCH_FPR: Register = convention::SCRATCH_FPR;

/// Loads (if `load`) or stores the value of type `ty` in `register` at `address`.
fn transfer(asm: &mut Assembler, ty: Type, register: Register, address: Address, load: bool) {
    if ty == Type::V128 {
        let opcode = if load { VMOVUPS_LOAD } else { VMOVUPS_STORE };
        asm.vex(
            opcode,
            register - convention::FPR_BASE,
            0,
            VexOperand::Memory(address),
            None,
        );
    } else if convention::is_fpr(register) {
        let register = Fpr(register - convention::FPR_BASE);
        if load {
            asm.vmovs_rm(Precision::Double, register, &address);
        } else {
            asm.vmovs_mr(Precision::Double, &address, register);
        }
    } else if load {
        // NB: i32 values are zero-extended in registers.
        let width = if ty == Type::I32 { Width::W32 } else { Width::W64 };
        asm.mov_rm(width, Gpr(register), &address);
    } else {
        asm.mov_mr(Width::W64, &address, Gpr(register));
    }
}

/// Copies a value of type `ty` between memory locations.
fn copy(asm: &mut Assembler, ty: Type, from: Address, to: Address) {
    let register = if ty == Type::V128 || ty.is_float() {
        SCRATCH_FPR
    } else {
        SCRATCH.0
    };
    let ty = if ty.is_float() { Type::F64 } else { ty };
    transfer(asm, ty, register, from, true);
    transfer(asm, ty, register, to, false);
}

/// The host stub for functions with these parameter and result types.
pub fn host_stub(params: &[Type], results: &[Type], runtime: &RuntimeInfo) -> Vec<u8> {
    let mut asm = Assembler::new();
    let parameter_slots = convention::slots(params) as i32;
    let result_slots = convention::slots(results) as i32;
    let buffer_slots = parameter_slots.max(result_slots);
    // Below the frame header: a zero word, which makes the frame the runtime unwinds an exception from look like an
    // entry frame, and the operand stack image at rsp.
    let zero = -convention::FRAME_HEADER_SIZE - 8;
    let frame_size = (convention::FRAME_HEADER_SIZE as u32 + 8 + 8 * buffer_slots as u32).next_multiple_of(16) as i32;
    let context = Gpr(convention::RAX);
    let instance = Gpr(convention::INSTANCE_REGISTER);

    asm.push(FP);
    asm.lea(Width::W64, FP, &Address::new(SP, 16));
    asm.mov_mr(Width::W64, &Address::new(FP, convention::FRAME_INSTANCE), instance);
    asm.mov_rm(
        Width::W64,
        context,
        &Address::new(instance, offset_of!(InstanceData, context) as i32),
    );
    asm.lea(Width::W64, SCRATCH, &Address::new(SP, -(frame_size - 16)));
    asm.alu_rm(
        crate::asm::x86_64::AluOp::Cmp,
        Width::W64,
        SCRATCH,
        &Address::new(context, offset_of!(ExecutionContext, stack_limit) as i32),
    );
    let exhausted = asm.new_label();
    asm.jcc(Cond::Below, exhausted);
    asm.mov_rr(Width::W64, SP, SCRATCH);

    // The operand stack grows down, so the first argument is in the highest slots.
    let top = 8 * (buffer_slots - parameter_slots);
    let mut places = Vec::new();
    convention::parameter_places(params, &mut places);
    for (index, place) in places.iter().enumerate() {
        let home = convention::home_slot(params, index) as i32;
        let slot = parameter_slots - home - params[index].slots() as i32;
        let address = Address::new(SP, top + 8 * slot);
        match *place {
            Place::Register(register) => transfer(&mut asm, params[index], register, address, false),
            Place::Memory(home) => copy(&mut asm, params[index], Address::new(FP, 8 * home as i32), address),
        }
    }

    let arguments = offset_of!(ExecutionContext, arguments) as i32;
    asm.mov_mr(
        Width::W64,
        &Address::new(context, arguments),
        Gpr(convention::CALLEE_REGISTER),
    );
    asm.lea(Width::W64, SCRATCH, &Address::new(SP, top));
    asm.mov_mr(Width::W64, &Address::new(context, arguments + 8), SCRATCH);
    asm.mov_mi(Width::W64, &Address::new(context, arguments + 16), 0);
    asm.mov_mi(Width::W64, &Address::new(FP, zero), 0);
    asm.lea(
        Width::W64,
        SCRATCH,
        &Address::new(FP, zero - convention::FRAME_INSTANCE),
    );
    asm.mov_mr(Width::W64, &Address::new(context, arguments + 24), SCRATCH);
    asm.mov_rr(Width::W64, Gpr(convention::RDI), context);
    asm.mov_ri(SCRATCH, runtime.call_host);
    asm.call_r(SCRATCH);

    // 0 is a trap, which the runtime keeps; 1 would resume at a handler, which the zero word rules out.
    let failed = asm.new_label();
    asm.cmp_ri(Width::W64, Gpr(convention::RAX), 1);
    asm.jcc(Cond::BelowOrEqual, failed);
    let return_top = Gpr(convention::RAX);
    let area = Gpr(convention::R11);
    if convention::has_result_area(results) {
        asm.mov_rm(Width::W64, area, &Address::new(FP, 8 * parameter_slots));
    }
    convention::result_places(results, &mut places);
    // NB: Backwards, so that rax, which the first integer result goes to, is written last.
    for (index, place) in places.iter().enumerate().rev() {
        let home = convention::home_slot(results, index) as i32;
        let slot = result_slots - home - results[index].slots() as i32;
        let address = Address::new(return_top, 8 * slot);
        match *place {
            Place::Register(register) => transfer(&mut asm, results[index], register, address, true),
            Place::Memory(home) => {
                let to = Address::new(area, 8 * home as i32);
                let register = if results[index] == Type::V128 || results[index].is_float() {
                    SCRATCH_FPR
                } else {
                    convention::RCX
                };
                let ty = if results[index].is_float() {
                    Type::F64
                } else {
                    results[index]
                };
                transfer(&mut asm, ty, register, address, true);
                transfer(&mut asm, ty, register, to, false);
            }
        }
    }
    asm.lea(Width::W64, SP, &Address::new(FP, convention::FRAME_SAVED_FP));
    asm.pop(FP);
    asm.ret();

    for (label, code) in [(failed, TRAP_RUNTIME), (exhausted, TRAP_STACK_EXHAUSTED)] {
        asm.bind(label);
        asm.mov_ri32(Gpr(convention::RDI), code);
        asm.mov_ri(SCRATCH, runtime.trap_exit);
        asm.jmp_r(SCRATCH);
    }
    asm.finish().expect("stubs have no unbound labels")
}
