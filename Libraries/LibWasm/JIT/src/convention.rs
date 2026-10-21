/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The calling convention between compiled functions on x86-64, and the layout of their frames.
//!
//! - Parameters of integer types go in rdi, rsi, rdx, rcx, r8 and r9, in order, and those of float and vector types
//!   in xmm0 to xmm7. Every parameter has a home slot in the caller's outgoing area, at the callee's fp + 8 * the
//!   slots of the parameters before it (a v128 takes two, like on the interpreter's operand stack); the parameters
//!   that do not fit in registers are passed there.
//! - Results of integer types come back in rax and rdx, and those of float and vector types in xmm0 and xmm1. The
//!   results that do not fit go to a result area the caller provides, at 8 * the slots of the results before them;
//!   the caller passes its address in the slot after the parameters' home slots.
//! - r14 holds the callee's instance and r15 the base of its memory 0 at the call, and r10 its `FunctionHeader`.
//! - Every register but rsp and rbp is caller-saved. rsp is 16-byte aligned at the call.
//! - A tail call writes its stack arguments into the incoming area and jumps to the callee. If they do not fit, it
//!   slides the return address and saved fp down first; callers set rsp from fp after every call, which undoes it.
//! - i32 values are zero-extended to 64 bits in registers and slots; f32 values use the low 32 bits.
//!
//! A compiled frame:
//!
//! ```text
//!   fp + 8 * i     the parameters' home slots (the caller's outgoing area), then the result area pointer
//!   fp - 8         return address
//!   fp - 16        caller's fp
//!   fp - 64..-16   the rest of the frame header, of which compiled code only writes FRAME_INSTANCE
//!   below          spill slots, 8 bytes each, two for a v128
//!   rsp            outgoing area of calls, 8 bytes per argument (and result area pointer, and result area)
//! ```
//!
//! fp is the caller's rsp at the call. The header's first two words are the reverse of the interpreter's current
//! frames; the interpreter moves to this order before compiled code runs in its frames (design, section 6).

use crate::ir::Type;

/// Registers as the register allocator numbers them: general purpose registers by their encoding, xmm registers
/// from `FPR_BASE` on.
pub type Register = u8;

pub const FPR_BASE: Register = 16;

pub const RAX: Register = 0;
pub const RCX: Register = 1;
pub const RDX: Register = 2;
pub const RBX: Register = 3;
pub const RSP: Register = 4;
pub const RBP: Register = 5;
pub const RSI: Register = 6;
pub const RDI: Register = 7;
pub const R8: Register = 8;
pub const R9: Register = 9;
pub const R10: Register = 10;
pub const R11: Register = 11;
pub const R12: Register = 12;
pub const R13: Register = 13;
pub const R14: Register = 14;
pub const R15: Register = 15;

pub const fn xmm(index: u8) -> Register {
    FPR_BASE + index
}

pub const GPR_ARGUMENTS: [Register; 6] = [RDI, RSI, RDX, RCX, R8, R9];
pub const FPR_ARGUMENTS: [Register; 8] = [xmm(0), xmm(1), xmm(2), xmm(3), xmm(4), xmm(5), xmm(6), xmm(7)];
pub const GPR_RESULTS: [Register; 2] = [RAX, RDX];
pub const FPR_RESULTS: [Register; 2] = [xmm(0), xmm(1)];

pub const INSTANCE_REGISTER: Register = R14;
pub const MEMORY_BASE_REGISTER: Register = R15;
pub const CALLEE_REGISTER: Register = R10;

/// Never allocated: moves between memory locations go through them, and lowerings may clobber them.
pub const SCRATCH_GPR: Register = R11;
pub const SCRATCH_FPR: Register = xmm(15);

/// The registers values may live in, in the order the allocator prefers them.
pub const ALLOCATABLE_GPRS: [Register; 13] = [RAX, RCX, RDX, RSI, RDI, R8, R9, R10, RBX, R12, R13, R14, R15];
pub const ALLOCATABLE_FPRS: [Register; 15] = [
    xmm(0),
    xmm(1),
    xmm(2),
    xmm(3),
    xmm(4),
    xmm(5),
    xmm(6),
    xmm(7),
    xmm(8),
    xmm(9),
    xmm(10),
    xmm(11),
    xmm(12),
    xmm(13),
    xmm(14),
];

/// Every allocatable register, as a mask.
pub const ALLOCATABLE: u32 = {
    let mut mask = 0;
    let mut index = 0;
    while index < ALLOCATABLE_GPRS.len() {
        mask |= 1 << ALLOCATABLE_GPRS[index];
        index += 1;
    }
    index = 0;
    while index < ALLOCATABLE_FPRS.len() {
        mask |= 1 << ALLOCATABLE_FPRS[index];
        index += 1;
    }
    mask
};

/// Where a call of a function of the runtime (`Op::Helper`) takes its operands: the instance, from which it finds the
/// context, then the operands, which it stores in the context's `arguments`.
pub const HELPER_REGISTERS: [Register; 7] = [RDI, RSI, RDX, RCX, R8, R9, R10];

/// The registers a call of a function of the runtime overwrites: those the host ABI does not preserve.
pub const HOST_CLOBBERS: u32 = {
    let preserved = (1 << RBX) | (1 << RSP) | (1 << RBP) | (1 << R12) | (1 << R13) | (1 << R14) | (1 << R15);
    !preserved
};

pub fn is_fpr(register: Register) -> bool {
    register >= FPR_BASE
}

/// The frame header, as offsets from fp (see the module documentation).
pub const FRAME_RETURN_ADDRESS: i32 = -8;
pub const FRAME_SAVED_FP: i32 = -16;
pub const FRAME_INSTANCE: i32 = libwasm_module::abi::FRAME_INSTANCE as i32;
pub const FRAME_HEADER_SIZE: i32 = libwasm_module::abi::FRAME_HEADER_SIZE as i32;

/// The fp offset of spill slot `index`.
pub fn spill_slot_offset(index: u32) -> i32 {
    -FRAME_HEADER_SIZE - 8 * (index as i32 + 1)
}

/// The fp offset of the home slot at `slot`, in 8-byte slots from fp.
pub fn home_slot_offset(slot: u32) -> i32 {
    8 * slot as i32
}

/// Where a parameter or result goes.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Place {
    Register(Register),
    /// A parameter's home slot, or a result's place in the result area, in 8-byte slots from its start.
    Memory(u32),
}

/// Assigns registers of each class in order, and memory to the rest; every value's memory place is at the sum of
/// the slots of the values before it, so that each has one whether or not it is in a register.
fn places(types: &[Type], gprs: &[Register], xmms: &[Register], places: &mut Vec<Place>) {
    places.clear();
    let (mut gpr, mut xmm, mut slot) = (0, 0, 0);
    for ty in types {
        let register = if ty.uses_xmm() {
            xmm += 1;
            xmms.get(xmm - 1)
        } else {
            gpr += 1;
            gprs.get(gpr - 1)
        };
        places.push(register.map_or(Place::Memory(slot), |register| Place::Register(*register)));
        slot += ty.slots();
    }
}

/// Where each parameter of a function with parameter types `types` is passed.
pub fn parameter_places(types: &[Type], output: &mut Vec<Place>) {
    places(types, &GPR_ARGUMENTS, &FPR_ARGUMENTS, output);
}

/// Where each result of a function with result types `types` is returned.
pub fn result_places(types: &[Type], output: &mut Vec<Place>) {
    places(types, &GPR_RESULTS, &FPR_RESULTS, output);
}

/// The home slot of parameter `index` of a function with parameter types `types`, in slots from fp.
pub fn home_slot(types: &[Type], index: usize) -> u32 {
    types[..index].iter().map(|ty| ty.slots()).sum()
}

/// The slots of a list of values.
pub fn slots(types: &[Type]) -> u32 {
    types.iter().map(|ty| ty.slots()).sum()
}

/// Whether a function with these results takes a result area.
pub fn has_result_area(types: &[Type]) -> bool {
    let xmms = types.iter().filter(|ty| ty.uses_xmm()).count();
    xmms > FPR_RESULTS.len() || types.len() - xmms > GPR_RESULTS.len()
}

/// Where the pointer to the result area is, in slots from fp, for a function with these parameters.
pub fn result_area_pointer(params: &[Type]) -> u32 {
    slots(params)
}

/// The bytes of outgoing area a call with these parameter and result types needs: the home slots, and if the
/// results need a result area, its pointer and the area.
pub fn outgoing_size(params: &[Type], results: &[Type]) -> u32 {
    let mut total = slots(params);
    if has_result_area(results) {
        total += 1 + slots(results);
    }
    8 * total
}
