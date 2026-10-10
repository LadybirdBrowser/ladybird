/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The entry trampoline: how native code enters JIT code.
//!
//! JIT code expects the VM in its pinned register when it is entered, and
//! neither keeps nor restores the pinned VM and scratch registers, which JIT
//! code that calls other JIT code needs to set up or keep for nothing. Native
//! code enters JIT code through the trampoline instead, which saves those
//! registers like the platform ABI wants, loads the VM, and calls the entry:
//! `JitResult trampoline(VM*, ExecutionContext* frame, entry)`, whose result
//! is the entry's.

use super::target_registers;
use crate::CompileFailure;
use crate::asm::FprSet;
use crate::asm::GprSet;
use crate::asm::PortableMacroAssembler;

/// Generates the entry trampoline.
pub fn generate_entry_trampoline<M: PortableMacroAssembler>() -> Result<Vec<u8>, CompileFailure> {
    let (_, pinned) = target_registers::<M>();
    let arguments = M::ARGUMENT_GPRS;
    let frame = M::frame(GprSet::EMPTY.with(pinned.vm).with(pinned.scratch), FprSet::EMPTY, 0);
    let mut masm = M::new();
    masm.emit_prologue(&frame);
    masm.move64(pinned.vm, arguments[0]);
    masm.call_register(arguments[2]);
    masm.emit_epilogue(&frame);
    masm.ret();
    let (code, _) = masm
        .finish_with_data_offset()
        .map_err(|_| CompileFailure::CodeGeneration)?;
    Ok(code)
}
