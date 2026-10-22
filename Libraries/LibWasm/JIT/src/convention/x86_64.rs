/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The registers of the calling convention on x86-64.
//!
//! - Parameters of integer types go in rdi, rsi, rdx, rcx, r8 and r9, in order, and those of float and vector types
//!   in xmm0 to xmm7. Results of integer types come back in rax and rdx, and those of float and vector types in xmm0
//!   and xmm1.
//! - r14 holds the callee's instance and r15 the base of its memory 0 at the call, and r10 its `FunctionHeader`.
//! - Every register but rsp and rbp is caller-saved. rsp is 16-byte aligned at the call.

use super::{Register, RegisterMask};

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
pub const ALLOCATABLE: RegisterMask = {
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
pub const HOST_CLOBBERS: RegisterMask = {
    let preserved = (1 << RBX) | (1 << RSP) | (1 << RBP) | (1 << R12) | (1 << R13) | (1 << R14) | (1 << R15);
    !preserved
};

/// With `stress-registers`, the registers values get besides those of fixed constraints: few enough that most
/// values spill.
pub const STRESS_GPRS: [Register; 3] = [RAX, RBX, R12];
pub const STRESS_FPRS: [Register; 2] = [xmm(8), xmm(9)];
