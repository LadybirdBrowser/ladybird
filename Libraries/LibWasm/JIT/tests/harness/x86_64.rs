/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The harness's code on x86-64: the trap exit, the stub of functions that did not compile, and the trampolines.

use super::{ENTRY_SP_OFFSET, SessionState, TRAP_NOT_COMPILED, TRAP_OFFSET};
use libwasm_engine::abi::InstanceData;
use libwasm_jit::asm::x86_64::{Assembler, Precision, VexMap, VexOpcode, VexOperand, VexPrefix};
use libwasm_jit::asm::{Address, Gpr, Width};
use libwasm_jit::convention::{self, Place};
use libwasm_jit::ir::Type;
use std::mem::offset_of;

const VMOVUPS_LOAD: VexOpcode = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x10);
const VMOVUPS_STORE: VexOpcode = VexOpcode::new(VexPrefix::None, VexMap::Map0F, 0x11);

/// The trap exit, entered with the trap code in edi: returns 1 from the trampoline.
pub(super) fn trap_exit_code(state: *mut SessionState) -> Vec<u8> {
    let mut asm = Assembler::for_host();
    let rax = Gpr(convention::RAX);
    let sp = Gpr(convention::RSP);
    asm.mov_ri(rax, state as u64);
    asm.mov_mr(Width::W32, &Address::new(rax, TRAP_OFFSET), Gpr(convention::RDI));
    asm.mov_rm(Width::W64, sp, &Address::new(rax, ENTRY_SP_OFFSET));
    restore_and_return(&mut asm, 1);
    asm.finish().unwrap()
}

/// The code of functions the JIT did not compile.
pub(super) fn not_compiled_code(trap_exit: u64) -> Vec<u8> {
    trap_stub(trap_exit, TRAP_NOT_COMPILED)
}

/// Code that traps with `code`.
pub(super) fn trap_stub(trap_exit: u64, code: u32) -> Vec<u8> {
    let mut asm = Assembler::for_host();
    asm.mov_ri32(Gpr(convention::RDI), code);
    asm.mov_ri(Gpr(convention::R11), trap_exit);
    asm.jmp_r(Gpr(convention::R11));
    asm.finish().unwrap()
}

fn restore_and_return(asm: &mut Assembler, status: u32) {
    asm.add_ri(Width::W64, Gpr(convention::RSP), 8);
    for register in [
        convention::R15,
        convention::R14,
        convention::R13,
        convention::R12,
        convention::RBX,
        convention::RBP,
    ] {
        asm.pop(Gpr(register));
    }
    asm.mov_ri32(Gpr(convention::RAX), status);
    asm.ret();
}

/// `extern "C" fn(entry, arguments, results, instance) -> u32`: calls `entry` with the arguments' slots under the
/// compiled convention, stores the results' slots, and returns 0, or 1 if the code trapped.
pub(super) fn trampoline_code(params: &[Type], results: &[Type], state: *mut SessionState) -> Vec<u8> {
    let mut asm = Assembler::for_host();
    let g = Gpr;
    let (rdi, rsi, rdx, rcx) = (
        g(convention::RDI),
        g(convention::RSI),
        g(convention::RDX),
        g(convention::RCX),
    );
    let sp = g(convention::RSP);
    let scratch = g(convention::R11);
    for register in [
        convention::RBP,
        convention::RBX,
        convention::R12,
        convention::R13,
        convention::R14,
        convention::R15,
    ] {
        asm.push(g(register));
    }
    asm.sub_ri(Width::W64, sp, 8);
    asm.mov_mr(Width::W64, &Address::new(sp, 0), rdx);
    asm.mov_ri(scratch, state as u64);
    asm.mov_mr(Width::W64, &Address::new(scratch, ENTRY_SP_OFFSET), sp);
    let outgoing = convention::outgoing_size(params, results).next_multiple_of(16) as i32;
    if outgoing > 0 {
        asm.sub_ri(Width::W64, sp, outgoing);
    }
    let entry = g(convention::R10);
    asm.mov_rr(Width::W64, entry, rdi);
    asm.mov_rr(Width::W64, scratch, rsi);
    asm.mov_rr(Width::W64, g(convention::INSTANCE_REGISTER), rcx);
    asm.mov_rm(
        Width::W64,
        g(convention::MEMORY_BASE_REGISTER),
        &Address::new(rcx, offset_of!(InstanceData, memory0_base) as i32),
    );
    // Copies the 8 or 16 bytes of a value of type `ty` between memory locations, through xmm15.
    let copy = |asm: &mut Assembler, ty: Type, from: Address, to: Address| {
        if ty == Type::V128 {
            asm.vex(VMOVUPS_LOAD, 15, 0, VexOperand::Memory(from), None);
            asm.vex(VMOVUPS_STORE, 15, 0, VexOperand::Memory(to), None);
        } else {
            asm.vmovs_rm(Precision::Double, libwasm_jit::asm::Fpr(15), &from);
            asm.vmovs_mr(Precision::Double, &to, libwasm_jit::asm::Fpr(15));
        }
    };
    // Loads or stores a value of type `ty` in `register` (true) at `address`.
    let transfer = |asm: &mut Assembler, ty: Type, register: u8, address: Address, load: bool| {
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
            let register = libwasm_jit::asm::Fpr(register - convention::FPR_BASE);
            if load {
                asm.vmovs_rm(Precision::Double, register, &address);
            } else {
                asm.vmovs_mr(Precision::Double, &address, register);
            }
        } else if load {
            asm.mov_rm(Width::W64, g(register), &address);
        } else {
            asm.mov_mr(Width::W64, &address, g(register));
        }
    };
    let mut places = Vec::new();
    convention::parameter_places(params, &mut places);
    // Stack arguments first, then registers, so that rsi and the others are read before they are written. Argument
    // slots are laid out like home slots, so a parameter's slot is the same in both.
    for (index, place) in places.iter().enumerate() {
        if let Place::Memory(slot) = place {
            let from = Address::new(scratch, 8 * *slot as i32);
            copy(&mut asm, params[index], from, Address::new(sp, 8 * *slot as i32));
        }
    }
    for (index, place) in places.iter().enumerate() {
        if let Place::Register(register) = place {
            let slot = convention::home_slot(params, index);
            transfer(
                &mut asm,
                params[index],
                *register,
                Address::new(scratch, 8 * slot as i32),
                true,
            );
        }
    }
    let parameter_slots = convention::slots(params) as i32;
    if convention::has_result_area(results) {
        let pointer = 8 * parameter_slots;
        let area = 8 * convention::result_area_slot(params) as i32;
        asm.lea(Width::W64, g(convention::RAX), &Address::new(sp, area));
        asm.mov_mr(Width::W64, &Address::new(sp, pointer), g(convention::RAX));
    }
    asm.call_r(entry);
    // NB: Tail calls may have moved rsp.
    asm.mov_ri(scratch, state as u64);
    asm.mov_rm(Width::W64, sp, &Address::new(scratch, ENTRY_SP_OFFSET));
    if outgoing > 0 {
        asm.sub_ri(Width::W64, sp, outgoing);
    }
    asm.mov_rm(Width::W64, scratch, &Address::new(sp, outgoing));
    convention::result_places(results, &mut places);
    for (index, place) in places.iter().enumerate() {
        let slot = convention::home_slot(results, index);
        let address = Address::new(scratch, 8 * slot as i32);
        match place {
            Place::Register(register) => transfer(&mut asm, results[index], *register, address, false),
            Place::Memory(slot) => {
                let area = 8 * convention::result_area_slot(params) as i32 + 8 * *slot as i32;
                copy(&mut asm, results[index], Address::new(sp, area), address);
            }
        }
    }
    if outgoing > 0 {
        asm.add_ri(Width::W64, sp, outgoing);
    }
    restore_and_return(&mut asm, 0);
    asm.finish().unwrap()
}
