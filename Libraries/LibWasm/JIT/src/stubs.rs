/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Code around compiled functions, which the runtime makes once per process:
//!
//! - The trap exit, which compiled code jumps to with a TRAP_* code in edi. It leaves the code through the
//!   interpreter's TrapFromNative handler, which returns from the interpreter's innermost entry with the trap.
//! - The entry trampoline, through which the runtime calls compiled code: an `extern "C"` function of a
//!   `NativeCall`, whose registers and home slot image it loads.
//! - The runtime adapter, the native entry of functions that run in the runtime (interpreted and host functions):
//!   it saves the argument registers and calls a function of the runtime with them, the callee's header and the
//!   frame, which puts the results where they go or returns a trap code.
//!
//! A host stub, of a signature, calls a host function through `wasm_runtime_call_host` directly, for the tests that
//! run compiled code without the runtime.

use crate::RuntimeInfo;
use crate::asm::x86_64::{Assembler, Cond, Precision, VexMap, VexOpcode, VexOperand, VexPrefix};
use crate::asm::{Address, Fpr, Gpr, Width};
use crate::convention::{self, Place, Register};
use crate::ir::Type;
use libwasm_module::abi::{ExecutionContext, FRAME_CONTEXT, InstanceData, TRAP_RUNTIME, TRAP_STACK_EXHAUSTED};
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
    let mut asm = Assembler::for_host();
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

/// What the entry trampoline calls compiled code with, and what it returns.
#[repr(C)]
#[derive(Debug, Default)]
pub struct NativeCall {
    /// The code to call: the callee's `native_entry`, or the entry of an OSR compile.
    pub entry: u64,
    pub header: u64,
    pub instance: u64,
    pub memory_base: u64,
    /// The integer argument registers, in `convention::GPR_ARGUMENTS` order.
    pub gprs: [u64; 6],
    /// The vector argument registers, low half first.
    pub xmms: [[u64; 2]; 8],
    /// The home slots of the call, and a result area if it needs one: copied to the outgoing area before the
    /// call, and back after it.
    pub image: u64,
    pub image_slots: u64,
    /// The slot of the result area pointer in the image, or u64::MAX, and the slot the result area starts at
    /// (`convention::result_area_slot()`); the image lays them out as the outgoing area does.
    pub result_pointer_slot: u64,
    pub result_area_slot: u64,
    /// rax and rdx after the call.
    pub result_gprs: [u64; 2],
    /// xmm0 and xmm1 after the call.
    pub result_xmms: [[u64; 2]; 2],
    /// Where the trampoline keeps its stack pointer during the call.
    pub saved_sp: u64,
}

/// The size of the area the runtime adapter saves the argument registers in: the six integer ones, then the eight
/// vector ones, 16 bytes each. The runtime function stores the results' registers in the same places: rax and rdx
/// where rdi and rsi were, xmm0 and xmm1 where they were.
pub const ADAPTER_SAVE_AREA: usize = 6 * 8 + 8 * 16;

fn gpr(register: Register) -> Gpr {
    Gpr(register)
}

fn field(offset: usize) -> i32 {
    offset as i32
}

/// The trap exit: stores the code in edi as the context's first argument, and the context in the frame that
/// trapped, and continues at the interpreter's TrapFromNative handler at `handler`.
pub fn trap_exit(handler: u64) -> Vec<u8> {
    let mut asm = Assembler::for_host();
    let context = gpr(convention::RAX);
    asm.mov_rm(Width::W64, context, &Address::new(FP, convention::FRAME_INSTANCE));
    asm.mov_rm(
        Width::W64,
        context,
        &Address::new(context, field(offset_of!(InstanceData, context))),
    );
    asm.mov_mr(Width::W64, &Address::new(FP, FRAME_CONTEXT as i32), context);
    asm.mov_mr(
        Width::W64,
        &Address::new(context, field(offset_of!(ExecutionContext, arguments))),
        gpr(convention::RDI),
    );
    asm.mov_ri(SCRATCH, handler);
    asm.jmp_r(SCRATCH);
    asm.finish().expect("stubs have no unbound labels")
}

/// Continues where the `resume` of the context in `context` says, as the runtime set it after unwinding an
/// exception: loads rsp and rbp and jumps.
pub fn resume(asm: &mut Assembler, context: Gpr) {
    let resume = field(offset_of!(ExecutionContext, resume));
    asm.mov_rm(Width::W64, SP, &Address::new(context, resume));
    asm.mov_rm(Width::W64, FP, &Address::new(context, resume + 8));
    asm.jmp_m(&Address::new(context, resume + 16));
}

/// Copies `count` 8-byte words from `from` up to `to` through `temp`, clobbering all four.
fn copy_words(asm: &mut Assembler, from: Gpr, to: Gpr, count: Gpr, temp: Gpr) {
    let done = asm.new_label();
    let again = asm.new_label();
    asm.test_rr(Width::W64, count, count);
    asm.jcc(Cond::Equal, done);
    asm.bind(again);
    asm.mov_rm(Width::W64, temp, &Address::new(from, 0));
    asm.mov_mr(Width::W64, &Address::new(to, 0), temp);
    asm.add_ri(Width::W64, from, 8);
    asm.add_ri(Width::W64, to, 8);
    asm.sub_ri(Width::W64, count, 1);
    asm.jcc(Cond::NotEqual, again);
    asm.bind(done);
}

/// The entry trampoline, `extern "C" fn(*mut NativeCall)`, and the offset of the address its call of compiled code
/// returns to, where an exception the compiled code does not handle continues (see `RuntimeInfo::throw`).
pub fn entry_trampoline() -> (Vec<u8>, usize) {
    let mut asm = Assembler::for_host();
    let record = gpr(convention::R11);
    let (rax, rcx, rdx, rsi, rdi) = (
        gpr(convention::RAX),
        gpr(convention::RCX),
        gpr(convention::RDX),
        gpr(convention::RSI),
        gpr(convention::RDI),
    );
    let call_field = |offset: usize| Address::new(record, field(offset));
    asm.push(FP);
    asm.mov_rr(Width::W64, FP, SP);
    for register in [
        convention::RBX,
        convention::R12,
        convention::R13,
        convention::R14,
        convention::R15,
    ] {
        asm.push(gpr(register));
    }
    // NB: Six pushes after the return address leave rsp 16-byte aligned. The record is the sixth.
    asm.push(rdi);
    asm.mov_rr(Width::W64, record, rdi);
    asm.mov_rm(Width::W64, rcx, &call_field(offset_of!(NativeCall, image_slots)));
    asm.mov_rr(Width::W64, rax, rcx);
    asm.shl_ri(Width::W64, rax, 3);
    asm.add_ri(Width::W64, rax, 15);
    asm.and_ri(Width::W64, rax, -16);
    asm.sub_rr(Width::W64, SP, rax);
    asm.mov_mr(Width::W64, &call_field(offset_of!(NativeCall, saved_sp)), SP);
    asm.mov_rm(Width::W64, rsi, &call_field(offset_of!(NativeCall, image)));
    asm.mov_rr(Width::W64, rdi, SP);
    copy_words(&mut asm, rsi, rdi, rcx, rdx);
    // The result area pointer points at the result area on the stack.
    let no_area = asm.new_label();
    asm.mov_rm(
        Width::W64,
        rax,
        &call_field(offset_of!(NativeCall, result_pointer_slot)),
    );
    asm.cmp_ri(Width::W64, rax, -1);
    asm.jcc(Cond::Equal, no_area);
    asm.mov_rm(Width::W64, rcx, &call_field(offset_of!(NativeCall, result_area_slot)));
    asm.lea(Width::W64, rsi, &Address::indexed(SP, rcx, crate::asm::Scale::Eight, 0));
    asm.mov_mr(Width::W64, &Address::indexed(SP, rax, crate::asm::Scale::Eight, 0), rsi);
    asm.bind(no_area);
    for index in 0..8u8 {
        let address = Address::new(record, field(offset_of!(NativeCall, xmms) + 16 * index as usize));
        asm.vex(VMOVUPS_LOAD, index, 0, VexOperand::Memory(address), None);
    }
    for (index, register) in convention::GPR_ARGUMENTS.iter().enumerate() {
        let address = Address::new(record, field(offset_of!(NativeCall, gprs) + 8 * index));
        asm.mov_rm(Width::W64, gpr(*register), &address);
    }
    for (register, offset) in [
        (convention::CALLEE_REGISTER, offset_of!(NativeCall, header)),
        (convention::INSTANCE_REGISTER, offset_of!(NativeCall, instance)),
        (convention::MEMORY_BASE_REGISTER, offset_of!(NativeCall, memory_base)),
    ] {
        asm.mov_rm(Width::W64, gpr(register), &Address::new(record, field(offset)));
    }
    asm.call_m(&call_field(offset_of!(NativeCall, entry)));
    let return_offset = asm.offset();
    // NB: Tail calls may have moved rsp; the record has it.
    asm.mov_rm(Width::W64, record, &Address::new(FP, -48));
    asm.mov_rm(Width::W64, SP, &call_field(offset_of!(NativeCall, saved_sp)));
    asm.mov_mr(Width::W64, &call_field(offset_of!(NativeCall, result_gprs)), rax);
    asm.mov_mr(Width::W64, &call_field(offset_of!(NativeCall, result_gprs) + 8), rdx);
    for index in 0..2u8 {
        let address = Address::new(record, field(offset_of!(NativeCall, result_xmms) + 16 * index as usize));
        asm.vex(VMOVUPS_STORE, index, 0, VexOperand::Memory(address), None);
    }
    asm.mov_rm(Width::W64, rcx, &call_field(offset_of!(NativeCall, image_slots)));
    asm.mov_rm(Width::W64, rdi, &call_field(offset_of!(NativeCall, image)));
    asm.mov_rr(Width::W64, rsi, SP);
    copy_words(&mut asm, rsi, rdi, rcx, rdx);
    asm.lea(Width::W64, SP, &Address::new(FP, -40));
    for register in [
        convention::R15,
        convention::R14,
        convention::R13,
        convention::R12,
        convention::RBX,
    ] {
        asm.pop(gpr(register));
    }
    asm.pop(FP);
    asm.ret();
    (asm.finish().expect("stubs have no unbound labels"), return_offset)
}

/// What the runtime function of the adapter returns when the call threw an exception, which the runtime unwound: the
/// adapter continues where the context's `resume` says.
pub const ADAPTER_RESUME: u64 = u64::MAX;

/// The runtime adapter. `function` is the runtime's `extern "C" fn(*mut ExecutionContext) -> u64`, which it calls
/// with the callee's header, the save area and fp as the context's first three arguments. A nonzero result is a
/// trap code, or `ADAPTER_RESUME`.
pub fn runtime_adapter(function: u64, runtime: &RuntimeInfo) -> Vec<u8> {
    let mut asm = Assembler::for_host();
    let instance = gpr(convention::INSTANCE_REGISTER);
    // Below the frame header, the save area at rsp.
    let frame_size = (convention::FRAME_HEADER_SIZE as usize + ADAPTER_SAVE_AREA).next_multiple_of(16) as i32;
    asm.push(FP);
    asm.lea(Width::W64, FP, &Address::new(SP, 16));
    asm.mov_mr(Width::W64, &Address::new(FP, convention::FRAME_INSTANCE), instance);
    asm.sub_ri(Width::W64, SP, frame_size - 16);
    for (index, register) in convention::GPR_ARGUMENTS.iter().enumerate() {
        asm.mov_mr(Width::W64, &Address::new(SP, 8 * index as i32), gpr(*register));
    }
    for index in 0..8u8 {
        let address = Address::new(SP, 48 + 16 * i32::from(index));
        asm.vex(VMOVUPS_STORE, index, 0, VexOperand::Memory(address), None);
    }
    let context = gpr(convention::RAX);
    asm.mov_rm(
        Width::W64,
        context,
        &Address::new(instance, field(offset_of!(InstanceData, context))),
    );
    let arguments = field(offset_of!(ExecutionContext, arguments));
    asm.mov_mr(
        Width::W64,
        &Address::new(context, arguments),
        gpr(convention::CALLEE_REGISTER),
    );
    asm.mov_mr(Width::W64, &Address::new(context, arguments + 8), SP);
    asm.mov_mr(Width::W64, &Address::new(context, arguments + 16), FP);
    asm.mov_rr(Width::W64, gpr(convention::RDI), context);
    asm.mov_ri(SCRATCH, function);
    asm.call_r(SCRATCH);
    let trap = asm.new_label();
    asm.test_rr(Width::W64, gpr(convention::RAX), gpr(convention::RAX));
    asm.jcc(Cond::NotEqual, trap);
    asm.mov_rm(Width::W64, gpr(convention::RAX), &Address::new(SP, 0));
    asm.mov_rm(Width::W64, gpr(convention::RDX), &Address::new(SP, 8));
    for index in 0..2u8 {
        let address = Address::new(SP, 48 + 16 * i32::from(index));
        asm.vex(VMOVUPS_LOAD, index, 0, VexOperand::Memory(address), None);
    }
    asm.lea(Width::W64, SP, &Address::new(FP, convention::FRAME_SAVED_FP));
    asm.pop(FP);
    asm.ret();
    asm.bind(trap);
    let unwound = asm.new_label();
    asm.cmp_ri(Width::W64, gpr(convention::RAX), -1);
    asm.jcc(Cond::Equal, unwound);
    asm.mov_rr(Width::W32, gpr(convention::RDI), gpr(convention::RAX));
    asm.mov_ri(SCRATCH, runtime.trap_exit);
    asm.jmp_r(SCRATCH);
    asm.bind(unwound);
    let context = gpr(convention::RAX);
    asm.mov_rm(Width::W64, context, &Address::new(FP, convention::FRAME_INSTANCE));
    asm.mov_rm(
        Width::W64,
        context,
        &Address::new(context, field(offset_of!(InstanceData, context))),
    );
    resume(&mut asm, context);
    asm.finish().expect("stubs have no unbound labels")
}
