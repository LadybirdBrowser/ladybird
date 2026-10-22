/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The harness's code on AArch64: the trap exit, the stub of functions that did not compile, and the trampolines.

use super::{ENTRY_SP_OFFSET, SessionState, TRAP_NOT_COMPILED, TRAP_OFFSET};
use libwasm_engine::abi::InstanceData;
use libwasm_jit::asm::aarch64::{Assembler, MemOp, MemOperand, PairOperand, X16, X17};
use libwasm_jit::asm::{Fpr, Gpr, Width};
use libwasm_jit::convention::{self, Place};
use libwasm_jit::ir::Type;
use std::mem::offset_of;

const FP: Gpr = Gpr(29);
const LR: Gpr = Gpr(30);
const SP: Gpr = Gpr(31);
const SCRATCH_FPR: Fpr = Fpr(31);
/// The bytes of the trampoline's frame record and saved registers.
const SAVE_AREA: i32 = 96;

fn hardware_fpr(register: u8) -> Fpr {
    Fpr(convention::vector_register(register))
}

/// An operand at `base` + `offset` for an access of `1 << size_log2` bytes, through x17 where it does not fit.
fn operand(asm: &mut Assembler, base: Gpr, offset: i32, size_log2: u32) -> MemOperand {
    if libwasm_jit::codegen::aarch64::fits_offset(i64::from(offset), size_log2) {
        return MemOperand::Offset(base, offset);
    }
    asm.add_any(X17, base, i64::from(offset), X17);
    MemOperand::Offset(X17, 0)
}

/// The trap exit, entered with the trap code in w0: returns 1 from the trampoline.
pub(super) fn trap_exit_code(state: *mut SessionState) -> Vec<u8> {
    let mut asm = Assembler::new();
    asm.mov_imm(Width::W64, X16, state as u64);
    asm.str(Width::W32, Gpr(0), MemOperand::Offset(X16, TRAP_OFFSET));
    asm.ldr(Width::W64, X17, MemOperand::Offset(X16, ENTRY_SP_OFFSET));
    asm.mov_sp(SP, X17);
    restore_and_return(&mut asm, 1);
    asm.finish().unwrap()
}

/// The code of functions the JIT did not compile.
pub(super) fn not_compiled_code(trap_exit: u64) -> Vec<u8> {
    trap_stub(trap_exit, TRAP_NOT_COMPILED)
}

/// Code that traps with `code`.
pub(super) fn trap_stub(trap_exit: u64, code: u32) -> Vec<u8> {
    let mut asm = Assembler::new();
    asm.mov_imm(Width::W32, Gpr(0), u64::from(code));
    asm.mov_imm(Width::W64, X16, trap_exit);
    asm.br(X16);
    asm.finish().unwrap()
}

/// Restores what the trampoline saved, from sp at its entry_sp, and returns `status`.
fn restore_and_return(asm: &mut Assembler, status: u32) {
    asm.add_imm(Width::W64, SP, SP, 16, false);
    for (index, pair) in [(19, 20), (21, 22), (23, 24), (25, 26), (27, 28)].iter().enumerate() {
        asm.ldp(
            Gpr(pair.0),
            Gpr(pair.1),
            PairOperand::Offset(SP, 16 + 16 * index as i32),
        );
    }
    asm.ldp(FP, LR, PairOperand::PostIndex(SP, SAVE_AREA));
    asm.mov_imm(Width::W32, Gpr(0), u64::from(status));
    asm.ret();
}

/// Loads (if `load`) or stores the value of type `ty` in the register `register` at `base` + `offset`.
fn transfer(asm: &mut Assembler, ty: Type, register: u8, base: Gpr, offset: i32, load: bool) {
    let (op, encoding, size_log2) = if ty == Type::V128 {
        (
            if load { MemOp::LdrQ } else { MemOp::StrQ },
            hardware_fpr(register).0,
            4,
        )
    } else if convention::is_fpr(register) {
        (
            if load { MemOp::LdrD } else { MemOp::StrD },
            hardware_fpr(register).0,
            3,
        )
    } else {
        (if load { MemOp::LdrX } else { MemOp::StrX }, register, 3)
    };
    let operand = operand(asm, base, offset, size_log2);
    asm.load_store(op, encoding, operand);
}

/// Copies the 8 or 16 bytes of a value of type `ty` between memory locations, through v31.
fn copy(asm: &mut Assembler, ty: Type, from: (Gpr, i32), to: (Gpr, i32)) {
    let (load, store, size_log2) = if ty == Type::V128 {
        (MemOp::LdrQ, MemOp::StrQ, 4)
    } else {
        (MemOp::LdrD, MemOp::StrD, 3)
    };
    let operand_from = operand(asm, from.0, from.1, size_log2);
    asm.load_store(load, SCRATCH_FPR.0, operand_from);
    let operand_to = operand(asm, to.0, to.1, size_log2);
    asm.load_store(store, SCRATCH_FPR.0, operand_to);
}

/// `extern "C" fn(entry, arguments, results, instance) -> u32`: calls `entry` with the arguments' slots under the
/// compiled convention, stores the results' slots, and returns 0, or 1 if the code trapped.
pub(super) fn trampoline_code(params: &[Type], results: &[Type], state: *mut SessionState) -> Vec<u8> {
    let mut asm = Assembler::new();
    asm.stp(FP, LR, PairOperand::PreIndex(SP, -SAVE_AREA));
    asm.mov_sp(FP, SP);
    for (index, pair) in [(19, 20), (21, 22), (23, 24), (25, 26), (27, 28)].iter().enumerate() {
        asm.stp(
            Gpr(pair.0),
            Gpr(pair.1),
            PairOperand::Offset(SP, 16 + 16 * index as i32),
        );
    }
    // The results pointer, at the entry_sp the trap exit restores.
    asm.sub_imm(Width::W64, SP, SP, 16, false);
    asm.str(Width::W64, Gpr(2), MemOperand::Offset(SP, 0));
    asm.mov_imm(Width::W64, X16, state as u64);
    asm.mov_sp(X17, SP);
    asm.str(Width::W64, X17, MemOperand::Offset(X16, ENTRY_SP_OFFSET));
    let outgoing = convention::outgoing_size(params, results).next_multiple_of(16) as i64;
    asm.add_any(SP, SP, -outgoing, X17);
    // The entry goes in x16 and the arguments' pointer in x19, which compiled code never uses.
    let (entry, arguments) = (X16, Gpr(19));
    asm.mov(Width::W64, entry, Gpr(0));
    asm.mov(Width::W64, arguments, Gpr(1));
    asm.mov(Width::W64, Gpr(convention::INSTANCE_REGISTER), Gpr(3));
    asm.ldr(
        Width::W64,
        Gpr(convention::MEMORY_BASE_REGISTER),
        MemOperand::Offset(Gpr(3), offset_of!(InstanceData, memory0_base) as i32),
    );
    let mut places = Vec::new();
    convention::parameter_places(params, &mut places);
    // Argument slots are laid out like home slots, so a parameter's slot is the same in both.
    for (index, place) in places.iter().enumerate() {
        match place {
            Place::Memory(slot) => {
                let offset = 8 * *slot as i32;
                copy(&mut asm, params[index], (arguments, offset), (SP, offset));
            }
            Place::Register(register) => {
                let slot = convention::home_slot(params, index);
                transfer(&mut asm, params[index], *register, arguments, 8 * slot as i32, true);
            }
        }
    }
    let parameter_slots = convention::slots(params) as i32;
    if convention::has_result_area(results) {
        let pointer = 8 * parameter_slots;
        let area = 8 * convention::result_area_slot(params) as i32;
        asm.add_any(Gpr(9), SP, i64::from(area), X17);
        let operand = operand(&mut asm, SP, pointer, 3);
        asm.str(Width::W64, Gpr(9), operand);
    }
    asm.blr(entry);
    // NB: Tail calls may have moved sp.
    asm.mov_imm(Width::W64, X16, state as u64);
    asm.ldr(Width::W64, X17, MemOperand::Offset(X16, ENTRY_SP_OFFSET));
    asm.mov_sp(SP, X17);
    let results_pointer = Gpr(19);
    asm.ldr(Width::W64, results_pointer, MemOperand::Offset(SP, 0));
    asm.add_any(SP, SP, -outgoing, X17);
    convention::result_places(results, &mut places);
    for (index, place) in places.iter().enumerate() {
        let slot = convention::home_slot(results, index) as i32;
        match place {
            Place::Register(register) => {
                transfer(&mut asm, results[index], *register, results_pointer, 8 * slot, false);
            }
            Place::Memory(area_slot) => {
                let area = 8 * convention::result_area_slot(params) as i32 + 8 * *area_slot as i32;
                copy(&mut asm, results[index], (SP, area), (results_pointer, 8 * slot));
            }
        }
    }
    asm.add_any(SP, SP, outgoing, X17);
    restore_and_return(&mut asm, 0);
    asm.finish().unwrap()
}
