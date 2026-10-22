/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Code around compiled functions on AArch64, which the runtime makes once per process (see `stubs.rs` for what each
//! stub does). The trap exit takes the TRAP_* code in w0.

use crate::RuntimeInfo;
use crate::asm::aarch64::{Assembler, Cond, Extend, MemOp, MemOperand, PairOperand, Shift, X16, X17, XZR};
use crate::asm::{Fpr, Gpr, Width};
use crate::codegen::aarch64::{fits_offset, fpr};
use crate::convention::{self, Place, Register};
use crate::ir::Type;
use libwasm_module::abi::{ExecutionContext, FRAME_CONTEXT, InstanceData, TRAP_RUNTIME, TRAP_STACK_EXHAUSTED};
use std::mem::offset_of;

const FP: Gpr = Gpr(29);
const LR: Gpr = Gpr(30);
const SP: Gpr = Gpr(31);
const SCRATCH: Gpr = X16;
const SCRATCH_FPR: Fpr = Fpr(31);

fn x(index: u8) -> Gpr {
    Gpr(index)
}

fn field(offset: usize) -> i32 {
    offset as i32
}

/// An operand for an access of `1 << size_log2` bytes at `base` + `offset`, through x17 where the offset does not fit.
fn operand(asm: &mut Assembler, base: Gpr, offset: i32, size_log2: u32) -> MemOperand {
    if fits_offset(i64::from(offset), size_log2) {
        return MemOperand::Offset(base, offset);
    }
    asm.add_any(X17, base, i64::from(offset), X17);
    MemOperand::Offset(X17, 0)
}

/// Loads (if `load`) or stores the value of type `ty` in the register `register` (as the allocator numbers it) at
/// `base` + `offset`.
fn transfer(asm: &mut Assembler, ty: Type, register: Register, base: Gpr, offset: i32, load: bool) {
    if ty == Type::V128 || convention::is_fpr(register) {
        transfer_fpr(asm, ty, fpr(register), base, offset, load);
        return;
    }
    // NB: i32 values are zero-extended in registers.
    let op = match (ty == Type::I32, load) {
        (true, true) => MemOp::LdrW,
        (false, true) => MemOp::LdrX,
        (_, false) => MemOp::StrX,
    };
    let size_log2 = if op == MemOp::LdrW { 2 } else { 3 };
    let operand = operand(asm, base, offset, size_log2);
    asm.load_store(op, register, operand);
}

fn transfer_fpr(asm: &mut Assembler, ty: Type, register: Fpr, base: Gpr, offset: i32, load: bool) {
    let (op, size_log2) = match (ty == Type::V128, load) {
        (true, true) => (MemOp::LdrQ, 4),
        (true, false) => (MemOp::StrQ, 4),
        (false, true) => (MemOp::LdrD, 3),
        (false, false) => (MemOp::StrD, 3),
    };
    let operand = operand(asm, base, offset, size_log2);
    asm.load_store(op, register.0, operand);
}

/// Copies a value of type `ty` between memory locations, through the scratch vector register.
fn copy(asm: &mut Assembler, ty: Type, from: (Gpr, i32), to: (Gpr, i32)) {
    let ty = if ty == Type::V128 { Type::V128 } else { Type::F64 };
    transfer_fpr(asm, ty, SCRATCH_FPR, from.0, from.1, true);
    transfer_fpr(asm, ty, SCRATCH_FPR, to.0, to.1, false);
}

/// Jumps to the trap exit with the code `code`.
fn trap(asm: &mut Assembler, code: u32, runtime: &RuntimeInfo) {
    asm.mov_imm(Width::W32, x(0), u64::from(code));
    asm.mov_imm(Width::W64, SCRATCH, runtime.trap_exit);
    asm.br(SCRATCH);
}

/// Pushes a frame record and makes fp the caller's sp, as compiled functions do, then allocates `frame_size` bytes
/// below fp, the header included, and stores the instance in x13 in the header.
fn enter(asm: &mut Assembler, frame_size: i32) {
    asm.stp(FP, LR, PairOperand::PreIndex(SP, -16));
    asm.add_imm(Width::W64, FP, SP, 16, false);
    asm.add_any(SP, SP, -i64::from(frame_size - 16), X17);
    asm.str(
        Width::W64,
        x(convention::INSTANCE_REGISTER),
        MemOperand::Offset(FP, convention::FRAME_INSTANCE),
    );
}

fn leave(asm: &mut Assembler) {
    asm.sub_imm(Width::W64, SP, FP, 16, false);
    asm.ldp(FP, LR, PairOperand::PostIndex(SP, 16));
    asm.ret();
}

/// The host stub for functions with these parameter and result types.
pub fn host_stub(params: &[Type], results: &[Type], runtime: &RuntimeInfo) -> Vec<u8> {
    let mut asm = Assembler::new();
    let parameter_slots = convention::slots(params) as i32;
    let result_slots = convention::slots(results) as i32;
    let buffer_slots = parameter_slots.max(result_slots);
    // Below the frame header: a zero word, which makes the frame the runtime unwinds an exception from look like an
    // entry frame, and the operand stack image at sp.
    let zero = -convention::FRAME_HEADER_SIZE - 8;
    let frame_size = (convention::FRAME_HEADER_SIZE as u32 + 8 + 8 * buffer_slots as u32).next_multiple_of(16) as i32;
    let context = x(9);
    let instance = x(convention::INSTANCE_REGISTER);

    // The stack check comes first, against the frame's bottom, with the header already in place for the trap.
    asm.stp(FP, LR, PairOperand::PreIndex(SP, -16));
    asm.add_imm(Width::W64, FP, SP, 16, false);
    asm.sub_imm(Width::W64, SP, SP, (convention::FRAME_HEADER_SIZE - 16) as u32, false);
    asm.str(Width::W64, instance, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
    asm.ldr(
        Width::W64,
        context,
        MemOperand::Offset(instance, field(offset_of!(InstanceData, context))),
    );
    asm.ldr(
        Width::W64,
        SCRATCH,
        MemOperand::Offset(context, field(offset_of!(ExecutionContext, stack_limit))),
    );
    asm.add_any(X17, FP, -i64::from(frame_size), X17);
    asm.cmp(Width::W64, X17, SCRATCH);
    let exhausted = asm.new_label();
    asm.b_cond(Cond::Lo, exhausted);
    asm.mov_sp(SP, X17);

    // The operand stack grows down, so the first argument is in the highest slots.
    let top = 8 * (buffer_slots - parameter_slots);
    let mut places = Vec::new();
    convention::parameter_places(params, &mut places);
    for (index, place) in places.iter().enumerate() {
        let home = convention::home_slot(params, index) as i32;
        let slot = parameter_slots - home - params[index].slots() as i32;
        let offset = top + 8 * slot;
        match *place {
            Place::Register(register) => transfer(&mut asm, params[index], register, SP, offset, false),
            Place::Memory(home) => copy(&mut asm, params[index], (FP, 8 * home as i32), (SP, offset)),
        }
    }

    let arguments = field(offset_of!(ExecutionContext, arguments));
    asm.str(
        Width::W64,
        x(convention::CALLEE_REGISTER),
        MemOperand::Offset(context, arguments),
    );
    asm.add_any(SCRATCH, SP, i64::from(top), X17);
    asm.str(Width::W64, SCRATCH, MemOperand::Offset(context, arguments + 8));
    asm.str(Width::W64, XZR, MemOperand::Offset(context, arguments + 16));
    asm.str(Width::W64, XZR, MemOperand::Offset(FP, zero));
    asm.sub_imm(
        Width::W64,
        SCRATCH,
        FP,
        (convention::FRAME_INSTANCE - zero) as u32,
        false,
    );
    asm.str(Width::W64, SCRATCH, MemOperand::Offset(context, arguments + 24));
    asm.mov(Width::W64, x(0), context);
    asm.mov_imm(Width::W64, SCRATCH, runtime.call_host);
    asm.blr(SCRATCH);

    // 0 is a trap, which the runtime keeps; 1 would resume at a handler, which the zero word rules out.
    let failed = asm.new_label();
    asm.cmp_imm(Width::W64, x(0), 1, false);
    asm.b_cond(Cond::Ls, failed);
    let return_top = x(9);
    asm.mov(Width::W64, return_top, x(0));
    let area = x(10);
    if convention::has_result_area(results) {
        let operand = operand(&mut asm, FP, 8 * parameter_slots, 3);
        asm.ldr(Width::W64, area, operand);
    }
    convention::result_places(results, &mut places);
    for (index, place) in places.iter().enumerate() {
        let home = convention::home_slot(results, index) as i32;
        let slot = result_slots - home - results[index].slots() as i32;
        let offset = 8 * slot;
        match *place {
            Place::Register(register) => transfer(&mut asm, results[index], register, return_top, offset, true),
            Place::Memory(home) => copy(&mut asm, results[index], (return_top, offset), (area, 8 * home as i32)),
        }
    }
    leave(&mut asm);

    for (label, code) in [(failed, TRAP_RUNTIME), (exhausted, TRAP_STACK_EXHAUSTED)] {
        asm.bind(label);
        trap(&mut asm, code, runtime);
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
    pub gprs: [u64; 8],
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
    /// x0 and x1 after the call.
    pub result_gprs: [u64; 2],
    /// v0 and v1 after the call.
    pub result_xmms: [[u64; 2]; 2],
    /// Where the trampoline keeps its stack pointer during the call.
    pub saved_sp: u64,
}

/// The size of the area the runtime adapter saves the argument registers in: the eight integer ones, then the eight
/// vector ones, 16 bytes each. The runtime function stores the results' registers in the same places: x0 and x1, v0
/// and v1.
pub const ADAPTER_SAVE_AREA: usize = 8 * 8 + 8 * 16;

/// The trap exit: stores the code in w0 as the context's first argument, and the context in the frame that trapped
/// (fp), and continues at the interpreter's TrapFromNative handler at `handler`.
pub fn trap_exit(handler: u64) -> Vec<u8> {
    let mut asm = Assembler::new();
    let context = SCRATCH;
    asm.ldr(Width::W64, context, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
    asm.ldr(
        Width::W64,
        context,
        MemOperand::Offset(context, field(offset_of!(InstanceData, context))),
    );
    asm.str(Width::W64, context, MemOperand::Offset(FP, FRAME_CONTEXT as i32));
    asm.mov(Width::W32, x(0), x(0));
    asm.str(
        Width::W64,
        x(0),
        MemOperand::Offset(context, field(offset_of!(ExecutionContext, arguments))),
    );
    asm.mov_imm(Width::W64, X17, handler);
    asm.br(X17);
    asm.finish().expect("stubs have no unbound labels")
}

/// Continues where the `resume` of the context in `context` says, as the runtime set it after unwinding an
/// exception: loads sp and fp and jumps.
pub fn resume(asm: &mut Assembler, context: Gpr) {
    let resume = field(offset_of!(ExecutionContext, resume));
    asm.ldr(Width::W64, X17, MemOperand::Offset(context, resume));
    asm.mov_sp(SP, X17);
    asm.ldr(Width::W64, FP, MemOperand::Offset(context, resume + 8));
    asm.ldr(Width::W64, X17, MemOperand::Offset(context, resume + 16));
    asm.br(X17);
}

/// Copies `count` 8-byte words from `from` up to `to` through `temp`, clobbering all four.
fn copy_words(asm: &mut Assembler, from: Gpr, to: Gpr, count: Gpr, temp: Gpr) {
    let done = asm.new_label();
    let again = asm.new_label();
    asm.cbz(Width::W64, count, done);
    asm.bind(again);
    asm.ldr(Width::W64, temp, MemOperand::PostIndex(from, 8));
    asm.str(Width::W64, temp, MemOperand::PostIndex(to, 8));
    asm.subs_imm(Width::W64, count, count, 1, false);
    asm.b_cond(Cond::Ne, again);
    asm.bind(done);
}

/// The entry trampoline, `extern "C" fn(*mut NativeCall)`, and the offset of the address its call of compiled code
/// returns to, where an exception the compiled code does not handle continues (see `RuntimeInfo::throw`). It keeps
/// the record in x19, which compiled code never uses, and saves nothing else: compiled code uses only the registers
/// the host ABI does not preserve (see `convention`).
pub fn entry_trampoline() -> (Vec<u8>, usize) {
    let mut asm = Assembler::new();
    let record = Gpr(19);
    let call_field = |offset: usize| field(offset);
    asm.stp(FP, LR, PairOperand::PreIndex(SP, -32));
    asm.mov_sp(FP, SP);
    asm.stp(record, Gpr(20), PairOperand::Offset(SP, 16));
    asm.mov(Width::W64, record, x(0));
    // The image goes at the bottom of the stack, 16-byte aligned.
    let (count, size) = (x(9), x(10));
    asm.ldr(
        Width::W64,
        count,
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, image_slots))),
    );
    asm.lsl_imm(Width::W64, size, count, 3);
    asm.add_imm(Width::W64, size, size, 15, false);
    asm.and_imm(Width::W64, size, size, !15u64);
    asm.sub_extended(Width::W64, SP, SP, size, Extend::Uxtx, 0);
    asm.mov_sp(x(11), SP);
    asm.str(
        Width::W64,
        x(11),
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, saved_sp))),
    );
    asm.ldr(
        Width::W64,
        x(12),
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, image))),
    );
    copy_words(&mut asm, x(12), x(11), count, x(13));
    // The result area pointer points at the result area on the stack.
    let no_area = asm.new_label();
    let (pointer, area) = (x(10), x(15));
    asm.ldr(
        Width::W64,
        pointer,
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, result_pointer_slot))),
    );
    asm.cmn_imm(Width::W64, pointer, 1, false);
    asm.b_cond(Cond::Eq, no_area);
    asm.ldr(
        Width::W64,
        area,
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, result_area_slot))),
    );
    asm.mov_sp(x(11), SP);
    asm.add(Width::W64, x(12), x(11), area, Shift::Lsl, 3);
    asm.add(Width::W64, x(13), x(11), pointer, Shift::Lsl, 3);
    asm.str(Width::W64, x(12), MemOperand::Offset(x(13), 0));
    asm.bind(no_area);
    for index in 0..8u8 {
        let offset = call_field(offset_of!(NativeCall, xmms) + 16 * index as usize);
        transfer_fpr(
            &mut asm,
            Type::V128,
            fpr(convention::FPR_ARGUMENTS[index as usize]),
            record,
            offset,
            true,
        );
    }
    for (index, register) in convention::GPR_ARGUMENTS.iter().enumerate() {
        let offset = call_field(offset_of!(NativeCall, gprs) + 8 * index);
        asm.ldr(Width::W64, x(*register), MemOperand::Offset(record, offset));
    }
    for (register, offset) in [
        (convention::CALLEE_REGISTER, offset_of!(NativeCall, header)),
        (convention::INSTANCE_REGISTER, offset_of!(NativeCall, instance)),
        (convention::MEMORY_BASE_REGISTER, offset_of!(NativeCall, memory_base)),
    ] {
        asm.ldr(Width::W64, x(register), MemOperand::Offset(record, field(offset)));
    }
    asm.ldr(
        Width::W64,
        SCRATCH,
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, entry))),
    );
    asm.blr(SCRATCH);
    let return_offset = asm.offset();
    // NB: Tail calls may have moved sp; the record has it.
    asm.ldr(
        Width::W64,
        x(11),
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, saved_sp))),
    );
    asm.mov_sp(SP, x(11));
    let results = call_field(offset_of!(NativeCall, result_gprs));
    asm.str(Width::W64, x(0), MemOperand::Offset(record, results));
    asm.str(Width::W64, x(1), MemOperand::Offset(record, results + 8));
    for index in 0..2u8 {
        let offset = call_field(offset_of!(NativeCall, result_xmms) + 16 * index as usize);
        transfer_fpr(
            &mut asm,
            Type::V128,
            fpr(convention::FPR_RESULTS[index as usize]),
            record,
            offset,
            false,
        );
    }
    asm.ldr(
        Width::W64,
        x(9),
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, image_slots))),
    );
    asm.ldr(
        Width::W64,
        x(12),
        MemOperand::Offset(record, call_field(offset_of!(NativeCall, image))),
    );
    asm.mov_sp(x(11), SP);
    copy_words(&mut asm, x(11), x(12), x(9), x(13));
    asm.mov_sp(SP, FP);
    asm.ldp(record, Gpr(20), PairOperand::Offset(SP, 16));
    asm.ldp(FP, LR, PairOperand::PostIndex(SP, 32));
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
    let mut asm = Assembler::new();
    let instance = x(convention::INSTANCE_REGISTER);
    // Below the frame header, the save area at sp.
    let frame_size = (convention::FRAME_HEADER_SIZE as usize + ADAPTER_SAVE_AREA).next_multiple_of(16) as i32;
    enter(&mut asm, frame_size);
    for (index, register) in convention::GPR_ARGUMENTS.iter().enumerate() {
        asm.str(Width::W64, x(*register), MemOperand::Offset(SP, 8 * index as i32));
    }
    let vectors = 8 * convention::GPR_ARGUMENTS.len() as i32;
    for (index, register) in convention::FPR_ARGUMENTS.iter().enumerate() {
        transfer_fpr(
            &mut asm,
            Type::V128,
            fpr(*register),
            SP,
            vectors + 16 * index as i32,
            false,
        );
    }
    let context = SCRATCH;
    asm.ldr(
        Width::W64,
        context,
        MemOperand::Offset(instance, field(offset_of!(InstanceData, context))),
    );
    let arguments = field(offset_of!(ExecutionContext, arguments));
    asm.str(
        Width::W64,
        x(convention::CALLEE_REGISTER),
        MemOperand::Offset(context, arguments),
    );
    asm.mov_sp(X17, SP);
    asm.str(Width::W64, X17, MemOperand::Offset(context, arguments + 8));
    asm.str(Width::W64, FP, MemOperand::Offset(context, arguments + 16));
    asm.mov(Width::W64, x(0), context);
    asm.mov_imm(Width::W64, SCRATCH, function);
    asm.blr(SCRATCH);
    let trap = asm.new_label();
    asm.cbnz(Width::W64, x(0), trap);
    for (index, register) in convention::GPR_RESULTS.iter().enumerate() {
        asm.ldr(Width::W64, x(*register), MemOperand::Offset(SP, 8 * index as i32));
    }
    for (index, register) in convention::FPR_RESULTS.iter().enumerate() {
        transfer_fpr(
            &mut asm,
            Type::V128,
            fpr(*register),
            SP,
            vectors + 16 * index as i32,
            true,
        );
    }
    leave(&mut asm);
    asm.bind(trap);
    let unwound = asm.new_label();
    asm.cmn_imm(Width::W64, x(0), 1, false);
    asm.b_cond(Cond::Eq, unwound);
    // NB: The code is in x0 already.
    asm.mov_imm(Width::W64, SCRATCH, runtime.trap_exit);
    asm.br(SCRATCH);
    asm.bind(unwound);
    let context = SCRATCH;
    asm.ldr(Width::W64, context, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
    asm.ldr(
        Width::W64,
        context,
        MemOperand::Offset(context, field(offset_of!(InstanceData, context))),
    );
    resume(&mut asm, context);
    asm.finish().expect("stubs have no unbound labels")
}
