/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The calling convention between compiled functions, and the layout of their frames. The registers are the
//! architecture's (`x86_64`, `aarch64`).
//!
//! - Parameters of integer types go in the integer argument registers, in order, and those of float and vector types
//!   in the vector argument registers. Every parameter has a home slot in the caller's outgoing area, at the callee's
//!   fp + 8 * the slots of the parameters before it (a v128 takes two, like on the interpreter's operand stack); the
//!   parameters that do not fit in registers are passed there.
//! - Results of integer types come back in the first two integer result registers, and those of float and vector
//!   types in the first two vector ones. The results that do not fit go to a result area the caller provides, at 8 *
//!   the slots of the results before them; the caller passes its address in the slot after the parameters' home
//!   slots. The area itself starts at the next even slot (`result_area_slot()`).
//! - `INSTANCE_REGISTER` holds the callee's instance and `MEMORY_BASE_REGISTER` the base of its memory 0 at the call,
//!   and `CALLEE_REGISTER` its `FunctionHeader`.
//! - Every register but the stack pointer and fp is caller-saved. The stack pointer is 16-byte aligned at the call.
//! - A tail call writes its stack arguments into the incoming area and jumps to the callee. Every caller provides an
//!   incoming area of `incoming_area_slots()`, an even number of slots, so a tail call moves the frame's bottom (on
//!   x86-64 with the return address and saved fp) down or up by whole 16 bytes to make the area exactly the callee's,
//!   with its top where it was: mutual tail calls run in constant space. Callers set the stack pointer from fp after
//!   every call, which undoes the moves.
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
//!   sp             outgoing area of calls, 8 bytes per argument (and result area pointer, and result area)
//! ```
//!
//! fp is the caller's stack pointer at the call, and the header is the interpreter's: its first two words are where the
//! call and `push rbp` put them on x86-64, and where `stp x29, x30, [sp, #-16]!` puts them on AArch64.

use crate::ir::Type;

#[cfg(target_arch = "aarch64")]
mod aarch64;
#[cfg(target_arch = "x86_64")]
mod x86_64;

#[cfg(target_arch = "aarch64")]
pub use aarch64::*;
#[cfg(target_arch = "x86_64")]
pub use x86_64::*;

/// Registers as the register allocator numbers them: general purpose registers by their encoding, vector registers
/// from `FPR_BASE` on.
pub type Register = u8;

/// A set of registers, by their numbers.
pub type RegisterMask = u32;

/// The registers the allocator tracks.
pub const REGISTER_COUNT: usize = 32;

pub fn is_fpr(register: Register) -> bool {
    register >= FPR_BASE
}

/// The frame header, as offsets from fp (see the module documentation).
pub const FRAME_RETURN_ADDRESS: i32 = libwasm_module::abi::FRAME_RETURN_IP as i32;
pub const FRAME_SAVED_FP: i32 = libwasm_module::abi::FRAME_SAVED_FP as i32;
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

/// Where the result area of a call with these parameters starts, in slots from the callee's fp: the first even slot
/// after the result area pointer, so that the incoming area below it is a whole number of 16 bytes.
pub fn result_area_slot(params: &[Type]) -> u32 {
    (slots(params) + 1).next_multiple_of(2)
}

/// The slots of the incoming area a function with these parameter and result types may use, and that a tail call
/// to it needs: the home slots and the result area pointer if any, rounded up to an even number.
pub fn incoming_area_slots(params: &[Type], results: &[Type]) -> u32 {
    (slots(params) + u32::from(has_result_area(results))).next_multiple_of(2)
}

/// The bytes of outgoing area a call with these parameter and result types needs: the incoming area, then the result
/// area if the results need one.
pub fn outgoing_size(params: &[Type], results: &[Type]) -> u32 {
    if has_result_area(results) {
        return 8 * (result_area_slot(params) + slots(results));
    }
    8 * incoming_area_slots(params, results)
}
