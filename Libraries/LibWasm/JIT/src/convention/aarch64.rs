/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The registers of the calling convention on AArch64.
//!
//! The allocator numbers x0 to x15 as 0 to 15, and v0 to v7 and v16 to v23 as 16 to 31 (`vector_register()`); it
//! never sees the other registers:
//!
//! - Parameters of integer types go in x0 to x7, in order, and those of float and vector types in v0 to v7. Results
//!   of integer types come back in x0 and x1, and those of float and vector types in v0 and v1.
//! - x13 holds the callee's instance and x14 the base of its memory 0 at the call, and x15 its `FunctionHeader`.
//! - x16 and x17 are the scratch registers of code generation, and v30 and v31 its vector ones. x18 is the
//!   platform's, x19 to x28 and v8 to v15 are left alone (the host ABI preserves them, so compiled code that leaves
//!   through the trap exit owes nobody their values), x29 is fp and x30 the link register.
//! - Every register the allocator knows is caller-saved. sp is 16-byte aligned at all times.

use super::{Register, RegisterMask};

pub const FPR_BASE: Register = 16;

pub const X0: Register = 0;
pub const X1: Register = 1;
pub const X2: Register = 2;
pub const X3: Register = 3;
pub const X4: Register = 4;
pub const X5: Register = 5;
pub const X6: Register = 6;
pub const X7: Register = 7;
pub const X8: Register = 8;
pub const X9: Register = 9;
pub const X10: Register = 10;
pub const X11: Register = 11;
pub const X12: Register = 12;
pub const X13: Register = 13;
pub const X14: Register = 14;
pub const X15: Register = 15;

/// The allocator's number of the vector register `index`, of v0 to v7 and v16 to v23.
pub const fn v(index: u8) -> Register {
    FPR_BASE + if index < 8 { index } else { index - 8 }
}

/// The hardware vector register of the allocator's register `register`.
pub const fn vector_register(register: Register) -> u8 {
    let index = register - FPR_BASE;
    if index < 8 { index } else { index + 8 }
}

pub const GPR_ARGUMENTS: [Register; 8] = [X0, X1, X2, X3, X4, X5, X6, X7];
pub const FPR_ARGUMENTS: [Register; 8] = [v(0), v(1), v(2), v(3), v(4), v(5), v(6), v(7)];
pub const GPR_RESULTS: [Register; 2] = [X0, X1];
pub const FPR_RESULTS: [Register; 2] = [v(0), v(1)];

pub const INSTANCE_REGISTER: Register = X13;
pub const MEMORY_BASE_REGISTER: Register = X14;
pub const CALLEE_REGISTER: Register = X15;

/// The registers values may live in, in the order the allocator prefers them.
pub const ALLOCATABLE_GPRS: [Register; 16] = [X0, X1, X2, X3, X4, X5, X6, X7, X8, X9, X10, X11, X12, X13, X14, X15];
pub const ALLOCATABLE_FPRS: [Register; 16] = [
    v(0),
    v(1),
    v(2),
    v(3),
    v(4),
    v(5),
    v(6),
    v(7),
    v(16),
    v(17),
    v(18),
    v(19),
    v(20),
    v(21),
    v(22),
    v(23),
];

/// Every allocatable register, as a mask.
pub const ALLOCATABLE: RegisterMask = RegisterMask::MAX;

/// Where a call of a function of the runtime (`Op::Helper`) takes its operands: the instance, from which it finds the
/// context, then the operands, which it stores in the context's `arguments`.
pub const HELPER_REGISTERS: [Register; 7] = [X0, X1, X2, X3, X4, X5, X6];

/// The registers a call of a function of the runtime overwrites: those the host ABI does not preserve, which is all of
/// those the allocator knows.
pub const HOST_CLOBBERS: RegisterMask = RegisterMask::MAX;

/// With `stress-registers`, the registers values get besides those of fixed constraints: few enough that most
/// values spill.
pub const STRESS_GPRS: [Register; 3] = [X0, X10, X11];
pub const STRESS_FPRS: [Register; 2] = [v(16), v(17)];
