/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! AArch64 code generation: the blocks in linear order, each node lowered with the registers the allocator gave it
//! (`numeric`, `memory`, `calls`, `tables`, `gc`, `vector`), then the out-of-line code: stubs for edges that need
//! moves, trap stubs and the subtype checks of indirect calls. The code is position independent: it reaches the
//! runtime through absolute addresses, has its jump tables inline, and builds its constants with moves, so it needs
//! no literal pool.
//!
//! x16 and x17 are the scratch registers, v30 and v31 the vector ones; the allocator never gives them out. x17 only
//! ever holds an address being formed (`operand()`), so a lowering may keep a value in x16 across an access.
//! Frame slots are addressed from sp, which is fp minus the frame size everywhere but in tail calls, so that their
//! offsets are positive and fit the scaled immediate forms.

mod calls;
mod gc;
mod memory;
mod numeric;
mod tables;
mod vector;

pub(crate) use vector::{is_vector_shift, vector_temps};

use crate::RuntimeInfo;
use crate::asm::aarch64::{Assembler, Cond, Extend, FloatSize, MemOp, MemOperand, PairOperand, Shift, X16, X17, XZR};
use crate::asm::{Fpr, Gpr, Label, Width};
use crate::convention::{self, Register};
use crate::ir::{BlockId, FloatCondition, Func, IntCondition, NONE, Op, Type, Value};
use crate::regalloc::{Allocation, Location, Move, NO_REGISTER, NO_SLOT};
use libwasm_module::abi::{ExecutionContext, InstanceData};
use std::mem::offset_of;

pub(crate) fn gpr(register: Register) -> Gpr {
    debug_assert!(
        !convention::is_fpr(register),
        "r{register} is no general purpose register"
    );
    Gpr(register)
}

pub(crate) fn fpr(register: Register) -> Fpr {
    debug_assert!(convention::is_fpr(register), "r{register} is no vector register");
    Fpr(convention::vector_register(register))
}

pub(crate) fn width(ty: Type) -> Width {
    if ty.is_64() { Width::W64 } else { Width::W32 }
}

pub(crate) fn float_size(ty: Type) -> FloatSize {
    if ty == Type::F64 {
        FloatSize::Double
    } else {
        FloatSize::Single
    }
}

pub(crate) const SCRATCH: Gpr = X16;
/// Only for forming addresses (see the module documentation).
pub(crate) const ADDRESS: Gpr = X17;
pub(crate) const SCRATCH_FPR: Fpr = Fpr(31);
pub(crate) const SCRATCH_FPR2: Fpr = Fpr(30);
pub(crate) const FP: Gpr = Gpr(29);
pub(crate) const LR: Gpr = Gpr(30);
pub(crate) const SP: Gpr = Gpr(31);

pub(crate) fn condition(condition: IntCondition) -> Cond {
    match condition {
        IntCondition::Eq => Cond::Eq,
        IntCondition::Ne => Cond::Ne,
        IntCondition::LtS => Cond::Lt,
        IntCondition::LtU => Cond::Lo,
        IntCondition::GtS => Cond::Gt,
        IntCondition::GtU => Cond::Hi,
        IntCondition::LeS => Cond::Le,
        IntCondition::LeU => Cond::Ls,
        IntCondition::GeS => Cond::Ge,
        IntCondition::GeU => Cond::Hs,
    }
}

/// The condition after `fcmp a, b` that holds where a float comparison does; all are false for unordered operands
/// but `Ne`.
pub(crate) fn float_condition(condition: FloatCondition) -> Cond {
    match condition {
        FloatCondition::Eq => Cond::Eq,
        FloatCondition::Ne => Cond::Ne,
        FloatCondition::Lt => Cond::Mi,
        FloatCondition::Le => Cond::Ls,
        FloatCondition::Gt => Cond::Gt,
        FloatCondition::Ge => Cond::Ge,
    }
}

/// Whether `offset` fits the immediate of a load or store of `1 << size_log2` bytes.
pub fn fits_offset(offset: i64, size_log2: u32) -> bool {
    let size = 1i64 << size_log2;
    (offset >= 0 && offset % size == 0 && offset / size < 4096) || (-256..256).contains(&offset)
}

/// The memory operation of a load (`load`) or store of `1 << size_log2` bytes to or from a general purpose register,
/// zero-extended.
pub(crate) fn gpr_access(size_log2: u32, load: bool) -> MemOp {
    match (size_log2, load) {
        (0, true) => MemOp::Ldrb,
        (0, false) => MemOp::Strb,
        (1, true) => MemOp::Ldrh,
        (1, false) => MemOp::Strh,
        (2, true) => MemOp::LdrW,
        (2, false) => MemOp::StrW,
        (_, true) => MemOp::LdrX,
        (_, false) => MemOp::StrX,
    }
}

/// The memory operation of a load or store of `1 << size_log2` bytes to or from a vector register.
pub(crate) fn fpr_access(size_log2: u32, load: bool) -> MemOp {
    match (size_log2, load) {
        (2, true) => MemOp::LdrS,
        (2, false) => MemOp::StrS,
        (3, true) => MemOp::LdrD,
        (3, false) => MemOp::StrD,
        (_, true) => MemOp::LdrQ,
        (_, false) => MemOp::StrQ,
    }
}

/// What code generation produced.
#[derive(Debug, Default)]
pub struct Generated {
    pub code: Vec<u8>,
    pub frame_size: u32,
    /// The offsets of the return addresses of calls that check for an exception once they return.
    pub exception_sites: Vec<u32>,
}

/// Out-of-line code for an edge that needs moves: its label, the edge's slot and its target.
struct EdgeStub {
    label: Label,
    slot: u32,
    target: BlockId,
}

pub struct CodeGenerator<'a> {
    func: &'a Func,
    allocation: &'a Allocation,
    runtime: &'a RuntimeInfo,
    asm: Assembler,
    block_labels: Vec<Label>,
    trap_labels: Vec<Option<Label>>,
    /// The trap with the code in x0, for the runtime's functions that return one.
    dynamic_trap: Option<Label>,
    subtype_checks: Vec<tables::SubtypeCheck>,
    edge_stubs: Vec<EdgeStub>,
    frame_size: u32,
    /// The block whose code is being generated.
    block: BlockId,
    exception_sites: Vec<u32>,
}

impl<'a> CodeGenerator<'a> {
    pub fn new(func: &'a Func, allocation: &'a Allocation, runtime: &'a RuntimeInfo) -> Self {
        let size = convention::FRAME_HEADER_SIZE as u32 + 8 * allocation.slot_count + allocation.outgoing_size;
        Self {
            func,
            allocation,
            runtime,
            asm: Assembler::new(),
            block_labels: Vec::new(),
            trap_labels: vec![None; 32],
            dynamic_trap: None,
            subtype_checks: Vec::new(),
            edge_stubs: Vec::new(),
            frame_size: size.next_multiple_of(16),
            block: 0,
            exception_sites: Vec::new(),
        }
    }

    pub fn generate(mut self) -> Result<Generated, crate::CompileFailure> {
        let func = self.func;
        for _ in 0..func.blocks.len() {
            let label = self.asm.new_label();
            self.block_labels.push(label);
        }
        self.prologue();
        for block in 0..func.blocks.len() as BlockId {
            self.block = block;
            let label = self.block_labels[block as usize];
            self.asm.bind(label);
            // Phis in registers that have a slot too are stored there where they are defined.
            for phi in func.phis(block) {
                let record = self.allocation.node(phi);
                let slot = self.allocation.slots[phi as usize];
                if record.output != NO_REGISTER && slot != NO_SLOT {
                    self.store_to_slot(record.output, slot, func.node(phi).ty == Type::V128);
                }
            }
            let data = func.blocks[block as usize];
            for value in data.start..data.end {
                let op = func.node(value).op;
                if op.is_entry_value() {
                    continue;
                }
                for item in self.allocation.node_moves(value) {
                    self.emit_move(item);
                }
                self.lower(value);
                let record = self.allocation.node(value);
                let slot = self.allocation.slots[value as usize];
                if record.output != NO_REGISTER && slot != NO_SLOT {
                    self.store_to_slot(record.output, slot, func.node(value).ty == Type::V128);
                }
            }
        }
        self.out_of_line();
        let code = self.asm.finish().map_err(|_| crate::CompileFailure::CodeGeneration)?;
        Ok(Generated {
            code,
            frame_size: self.frame_size,
            exception_sites: self.exception_sites,
        })
    }

    // Frames.

    fn prologue(&mut self) {
        let func = self.func;
        let instance = gpr(convention::INSTANCE_REGISTER);
        self.asm.stp(FP, LR, PairOperand::PreIndex(SP, -16));
        self.asm.add_imm(Width::W64, FP, SP, 16, false);
        // NB: The header goes below sp first, since nothing may be stored below sp. The instance goes in it before the
        //     stack check, which traps through the frame.
        let header = (convention::FRAME_HEADER_SIZE - 16) as u32;
        self.asm.sub_imm(Width::W64, SP, SP, header, false);
        self.asm
            .str(Width::W64, instance, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
        self.asm.ldr(
            Width::W64,
            SCRATCH,
            MemOperand::Offset(instance, offset_of!(InstanceData, context) as i32),
        );
        self.asm.ldr(
            Width::W64,
            SCRATCH,
            MemOperand::Offset(SCRATCH, offset_of!(ExecutionContext, stack_limit) as i32),
        );
        self.asm.add_any(ADDRESS, FP, -i64::from(self.frame_size), ADDRESS);
        self.asm.cmp(Width::W64, ADDRESS, SCRATCH);
        let overflow = self.trap_label(libwasm_module::abi::TRAP_STACK_EXHAUSTED);
        self.asm.b_cond(Cond::Lo, overflow);
        self.asm.mov_sp(SP, ADDRESS);
        // Values arriving in registers that have slots.
        let data = func.blocks[0];
        for value in data.start..data.end {
            let node = func.node(value);
            if !matches!(node.op, Op::Param | Op::MemoryBase) {
                continue;
            }
            let record = self.allocation.node(value);
            let slot = self.allocation.slots[value as usize];
            if record.output != NO_REGISTER && slot != NO_SLOT {
                self.store_to_slot(record.output, slot, node.ty == Type::V128);
            }
        }
    }

    /// Has the runtime unwind the exception in `exception` from this frame, and continues where it says: at a call
    /// that checks for exceptions, or in the entry trampoline.
    fn throw(&mut self, exception: Gpr) {
        let context = SCRATCH;
        self.asm
            .ldr(Width::W64, context, MemOperand::Offset(FP, convention::FRAME_INSTANCE));
        self.asm.ldr(
            Width::W64,
            context,
            MemOperand::Offset(context, offset_of!(InstanceData, context) as i32),
        );
        let arguments = offset_of!(ExecutionContext, arguments) as i32;
        self.asm
            .str(Width::W64, exception, MemOperand::Offset(context, arguments));
        self.asm.str(Width::W64, FP, MemOperand::Offset(context, arguments + 8));
        self.asm.mov(Width::W64, Gpr(0), context);
        self.asm.mov_imm(Width::W64, ADDRESS, self.runtime.throw);
        self.asm.blr(ADDRESS);
        crate::stubs::resume(&mut self.asm, Gpr(0));
    }

    fn epilogue(&mut self) {
        self.asm.sub_imm(Width::W64, SP, FP, 16, false);
        self.asm.ldp(FP, LR, PairOperand::PostIndex(SP, 16));
        self.asm.ret();
    }

    /// Sets sp back to the bottom of the frame, after a call.
    fn reset_sp(&mut self) {
        self.asm.add_any(SP, FP, -i64::from(self.frame_size), ADDRESS);
    }

    fn trap_label(&mut self, code: u32) -> Label {
        if let Some(label) = self.trap_labels[code as usize] {
            return label;
        }
        let label = self.asm.new_label();
        self.trap_labels[code as usize] = Some(label);
        label
    }

    fn dynamic_trap_label(&mut self) -> Label {
        *self.dynamic_trap.get_or_insert_with(|| self.asm.new_label())
    }

    fn out_of_line(&mut self) {
        self.subtype_checks();
        let stubs = std::mem::take(&mut self.edge_stubs);
        for stub in &stubs {
            self.asm.bind(stub.label);
            for item in self.allocation.edge(stub.slot) {
                self.emit_move(item);
            }
            let target = self.block_labels[stub.target as usize];
            self.asm.b(target);
        }
        for code in 0..self.trap_labels.len() {
            if let Some(label) = self.trap_labels[code] {
                self.asm.bind(label);
                self.asm.mov_imm(Width::W32, Gpr(0), code as u64);
                self.asm.mov_imm(Width::W64, SCRATCH, self.runtime.trap_exit);
                self.asm.br(SCRATCH);
            }
        }
        if let Some(label) = self.dynamic_trap {
            // NB: The code is in x0 already.
            self.asm.bind(label);
            self.asm.mov_imm(Width::W64, SCRATCH, self.runtime.trap_exit);
            self.asm.br(SCRATCH);
        }
    }

    // Addresses.

    /// An operand for an access of `1 << size_log2` bytes at `base` + `offset`, through `ADDRESS` where the offset does
    /// not fit.
    pub(crate) fn operand(&mut self, base: Gpr, offset: i64, size_log2: u32) -> MemOperand {
        if fits_offset(offset, size_log2) {
            return MemOperand::Offset(base, offset as i32);
        }
        self.asm.add_any(ADDRESS, base, offset, ADDRESS);
        MemOperand::Offset(ADDRESS, 0)
    }

    /// The sp offset of the frame slot at fp offset `slot`.
    fn frame_offset(&self, slot: i32) -> i64 {
        i64::from(self.frame_size) + i64::from(slot)
    }

    fn location_operand(&mut self, location: Location, size_log2: u32) -> MemOperand {
        let offset = match location {
            Location::Slot(slot) => self.frame_offset(slot),
            Location::Outgoing(index) => 8 * i64::from(index),
            _ => unreachable!("{location:?} is no frame location"),
        };
        self.operand(SP, offset, size_log2)
    }

    // Moves.

    /// Loads (if `load`) or stores the register `register` at `operand`: 8 bytes, or 16 for a vector.
    fn transfer(&mut self, register: Register, operand: MemOperand, vector: bool, load: bool) {
        let op = if vector {
            fpr_access(4, load)
        } else if convention::is_fpr(register) {
            fpr_access(3, load)
        } else {
            gpr_access(3, load)
        };
        let encoding = if convention::is_fpr(register) {
            fpr(register).0
        } else {
            gpr(register).0
        };
        self.asm.load_store(op, encoding, operand);
    }

    fn store_to_slot(&mut self, register: Register, slot: i32, vector: bool) {
        let operand = self.location_operand(Location::Slot(slot), if vector { 4 } else { 3 });
        self.transfer(register, operand, vector, false);
    }

    /// The bits of a constant of any type.
    fn constant_bits(&self, value: Value) -> u128 {
        let node = self.func.node(value);
        match node.op {
            Op::Const128 => self.func.constants[node.args[0] as usize],
            _ => u128::from(node.constant().expect("only constants have bits")),
        }
    }

    /// Puts the constant `bits` into the vector register `register`: its low 64 bits, or all 128 if `vector`.
    pub(crate) fn load_vector_constant(&mut self, bits: u128, register: Fpr, vector: bool) {
        let (low, high) = (bits as u64, (bits >> 64) as u64);
        if low == 0 {
            self.asm.fmov_from_gpr(register, XZR);
        } else {
            self.asm.mov_imm(Width::W64, SCRATCH, low);
            self.asm.fmov_from_gpr(register, SCRATCH);
        }
        if vector && high != 0 {
            let source = if high == low {
                SCRATCH
            } else {
                self.asm.mov_imm(Width::W64, SCRATCH, high);
                SCRATCH
            };
            self.asm.insert_lane_from_gpr(register, 3, 1, source);
        }
    }

    /// Materializes the constant `value` into `register`.
    fn materialize(&mut self, value: Value, register: Register) {
        let ty = self.func.node(value).ty;
        let bits = self.constant_bits(value);
        if convention::is_fpr(register) {
            self.load_vector_constant(bits, fpr(register), ty == Type::V128);
        } else {
            let bits = if ty.is_64() {
                bits as u64
            } else {
                bits as u64 & 0xffff_ffff
            };
            self.asm.mov_imm(Width::W64, gpr(register), bits);
        }
    }

    /// Stores the constant `value` at `operand` (which does not use x16), as 8 bytes, or 16 for a vector.
    fn store_constant(&mut self, value: Value, location: Location) {
        let node = self.func.node(value);
        if node.op == Op::Const128 {
            let bits = self.constant_bits(value);
            for (half, bits) in [(0, bits as u64), (1, (bits >> 64) as u64)] {
                let source = if bits == 0 {
                    XZR
                } else {
                    self.asm.mov_imm(Width::W64, SCRATCH, bits);
                    SCRATCH
                };
                let operand = self.half_operand(location, half);
                self.asm.str(Width::W64, source, operand);
            }
            return;
        }
        let bits = node.constant().expect("only constants are stored as constants");
        let bits = if node.ty.is_64() { bits } else { bits & 0xffff_ffff };
        let source = if bits == 0 {
            XZR
        } else {
            self.asm.mov_imm(Width::W64, SCRATCH, bits);
            SCRATCH
        };
        let operand = self.location_operand(location, 3);
        self.asm.str(Width::W64, source, operand);
    }

    /// The operand of the 8-byte half `half` of the frame location `location`.
    fn half_operand(&mut self, location: Location, half: i32) -> MemOperand {
        let location = match location {
            Location::Slot(slot) => Location::Slot(slot + 8 * half),
            Location::Outgoing(index) => Location::Outgoing(index + half as u32),
            _ => unreachable!("{location:?} is no frame location"),
        };
        self.location_operand(location, 3)
    }

    /// Copies a vector register.
    pub(crate) fn move_vector(&mut self, to: Fpr, from: Fpr) {
        if to != from {
            self.asm.mov_vector(to, from);
        }
    }

    pub(crate) fn emit_move(&mut self, item: &Move) {
        match (item.from, item.to) {
            (Location::Register(from), Location::Register(to)) => {
                if from == to {
                    return;
                }
                match (convention::is_fpr(from), convention::is_fpr(to)) {
                    (true, true) => self.move_vector(fpr(to), fpr(from)),
                    (false, false) => self.asm.mov(Width::W64, gpr(to), gpr(from)),
                    (true, false) => self.asm.fmov_to_gpr(gpr(to), fpr(from)),
                    (false, true) => self.asm.fmov_from_gpr(fpr(to), gpr(from)),
                }
            }
            (Location::Constant(value), Location::Register(to)) => self.materialize(value, to),
            (Location::Register(from), to @ (Location::Slot(_) | Location::Outgoing(_))) => {
                let operand = self.location_operand(to, if item.vector { 4 } else { 3 });
                self.transfer(from, operand, item.vector, false);
            }
            (from @ (Location::Slot(_) | Location::Outgoing(_)), Location::Register(to)) => {
                let operand = self.location_operand(from, if item.vector { 4 } else { 3 });
                self.transfer(to, operand, item.vector, true);
            }
            (Location::Constant(value), to @ (Location::Slot(_) | Location::Outgoing(_))) => {
                self.store_constant(value, to);
            }
            (from @ Location::Slot(_), to @ (Location::Slot(_) | Location::Outgoing(_))) => {
                let size_log2 = if item.vector { 4 } else { 3 };
                let operand = self.location_operand(from, size_log2);
                let register = if item.vector { SCRATCH_FPR.0 } else { SCRATCH.0 };
                let (load, store) = if item.vector {
                    (fpr_access(4, true), fpr_access(4, false))
                } else {
                    (gpr_access(3, true), gpr_access(3, false))
                };
                self.asm.load_store(load, register, operand);
                let operand = self.location_operand(to, size_log2);
                self.asm.load_store(store, register, operand);
            }
            (from, Location::ResultArea(index)) => {
                // The value goes through a vector register, or x16; the area's address takes x17.
                let (register, op) = match from {
                    Location::Register(register) if convention::is_fpr(register) => (
                        fpr(register).0,
                        if item.vector {
                            fpr_access(4, false)
                        } else {
                            fpr_access(3, false)
                        },
                    ),
                    Location::Register(register) => (gpr(register).0, gpr_access(3, false)),
                    Location::Slot(slot) => {
                        let size_log2 = if item.vector { 4 } else { 3 };
                        let operand = self.location_operand(Location::Slot(slot), size_log2);
                        self.asm.load_store(fpr_access(size_log2, true), SCRATCH_FPR.0, operand);
                        (SCRATCH_FPR.0, fpr_access(size_log2, false))
                    }
                    Location::Constant(value) => {
                        let bits = self.constant_bits(value);
                        self.load_vector_constant(bits, SCRATCH_FPR, item.vector);
                        let size_log2 = if item.vector { 4 } else { 3 };
                        (SCRATCH_FPR.0, fpr_access(size_log2, false))
                    }
                    _ => unreachable!("results do not come from {from:?}"),
                };
                let pointer = convention::home_slot_offset(convention::result_area_pointer(&self.func.params));
                let operand = self.location_operand(Location::Slot(pointer), 3);
                self.asm.ldr(Width::W64, ADDRESS, operand);
                let operand = MemOperand::Offset(ADDRESS, 8 * index as i32);
                let operand = if fits_offset(8 * i64::from(index), op.size_log2()) {
                    operand
                } else {
                    self.asm.add_any(ADDRESS, ADDRESS, 8 * i64::from(index), ADDRESS);
                    MemOperand::Offset(ADDRESS, 0)
                };
                self.asm.load_store(op, register, operand);
            }
            (from, to) => unreachable!("no move from {from:?} to {to:?}"),
        }
    }

    // Control.

    /// The label an edge jumps to: its target's, or a stub's that moves values into place first.
    fn edge_label(&mut self, slot: u32, target: BlockId) -> Label {
        if self.allocation.edge(slot).is_empty() {
            return self.block_labels[target as usize];
        }
        let label = self.asm.new_label();
        self.edge_stubs.push(EdgeStub { label, slot, target });
        label
    }

    /// Ends the block on its edge `index` without a condition.
    fn jump(&mut self, index: usize) {
        let block = self.block;
        let slot = self.func.blocks[block as usize].successors_start + index as u32;
        let target = self.func.successors(block)[index];
        for item in self.allocation.edge(slot) {
            self.emit_move(item);
        }
        if target != block + 1 {
            let label = self.block_labels[target as usize];
            self.asm.b(label);
        }
    }

    /// Ends the block with a two-way branch on `cond`, which holds for the first successor; inverted where the taken
    /// successor comes next and needs no moves, or is a loop header the edge needs moves into, so that those moves are
    /// inline rather than in a stub that branches again.
    fn branch_on(&mut self, cond: Cond) {
        let block = self.block;
        let data = self.func.blocks[block as usize];
        let successors = self.func.successors(block);
        let (taken, fallthrough) = (successors[0], successors[1]);
        if taken == block + 1 && self.allocation.edge(data.successors_start).is_empty() {
            let label = self.edge_label(data.successors_start + 1, fallthrough);
            self.asm.b_cond(cond.invert(), label);
            return;
        }
        if self.func.is_loop_header(taken) && !self.allocation.edge(data.successors_start).is_empty() {
            let label = self.edge_label(data.successors_start + 1, fallthrough);
            self.asm.b_cond(cond.invert(), label);
            self.jump(0);
            return;
        }
        let taken_label = self.edge_label(data.successors_start, taken);
        self.asm.b_cond(cond, taken_label);
        for item in self.allocation.edge(data.successors_start + 1) {
            self.emit_move(item);
        }
        if fallthrough != block + 1 {
            let label = self.block_labels[fallthrough as usize];
            self.asm.b(label);
        }
    }

    fn lower_terminator(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        match node.op {
            Op::Jump => self.jump(0),
            Op::Branch => {
                self.asm.cmp_imm(Width::W32, gpr(record.inputs[0]), 0, false);
                self.branch_on(Cond::Ne);
            }
            Op::BranchCompare => {
                let ty = func.node(node.args[0]).ty;
                self.compare(value, ty, record.inputs[0], record.inputs[1], 1);
                self.branch_on(condition(IntCondition::from_flags(node.flags)));
            }
            Op::BranchFloatCompare => {
                let ty = func.node(node.args[0]).ty;
                self.asm
                    .float_compare(float_size(ty), fpr(record.inputs[0]), fpr(record.inputs[1]));
                self.branch_on(float_condition(FloatCondition::from_flags(node.flags)));
            }
            Op::BrTable => self.branch_table(value),
            Op::Return => self.epilogue(),
            Op::TailCall => self.lower_tail_call(value),
            Op::Trap => {
                let label = self.trap_label(node.args[0]);
                self.asm.b(label);
            }
            Op::Throw => self.throw(gpr(record.inputs[0])),
            _ => unreachable!("{} is no terminator", node.op.name()),
        }
    }

    fn branch_table(&mut self, value: Value) {
        let func = self.func;
        let block = self.block;
        let data = func.blocks[block as usize];
        let record = *self.allocation.node(value);
        let index = gpr(record.inputs[0]);
        let successors = func.successors(block);
        let mut labels = Vec::with_capacity(successors.len());
        for (position, &successor) in successors.iter().enumerate() {
            let label = self.edge_label(data.successors_start + position as u32, successor);
            labels.push(label);
        }
        let targets = func.table_targets(value);
        let (cases, default) = targets.split_at(targets.len() - 1);
        let default = labels[default[0] as usize];
        self.compare_immediate(Width::W32, index, cases.len() as u64);
        self.asm.b_cond(Cond::Hs, default);
        // The table is a branch per case, right after the indirect branch into it.
        let table = self.asm.new_label();
        self.asm.reserve_room(4 * (cases.len() + 4));
        self.asm.adr(SCRATCH, table);
        self.asm
            .add_extended(Width::W64, SCRATCH, SCRATCH, index, Extend::Uxtw, 2);
        self.asm.br(SCRATCH);
        self.asm.bind(table);
        for case in cases {
            self.asm.b(labels[*case as usize]);
        }
    }

    /// `cmp register, #immediate`, through x16 where the immediate does not fit.
    fn compare_immediate(&mut self, width: Width, register: Gpr, immediate: u64) {
        let immediate = if width == Width::W32 {
            immediate & 0xffff_ffff
        } else {
            immediate
        };
        let negated = match width {
            Width::W32 => (immediate as u32).wrapping_neg() as u64,
            Width::W64 => immediate.wrapping_neg(),
        };
        if immediate < 4096 {
            self.asm.cmp_imm(width, register, immediate as u32, false);
        } else if immediate & 0xfff == 0 && immediate < (1 << 24) {
            self.asm.cmp_imm(width, register, (immediate >> 12) as u32, true);
        } else if negated < 4096 {
            self.asm.cmn_imm(width, register, negated as u32, false);
        } else {
            self.asm.mov_imm(Width::W64, SCRATCH, immediate);
            self.asm.cmp(width, register, SCRATCH);
        }
    }

    /// `cmp a, b` of type `ty`, where `b` is input `index` of `value`: an immediate if it is a constant.
    fn compare(&mut self, value: Value, ty: Type, a: Register, b: Register, index: usize) {
        if b == crate::regalloc::CONSTANT_INPUT {
            let bits = self.immediate(value, index);
            self.compare_immediate(width(ty), gpr(a), bits);
        } else {
            self.asm.cmp(width(ty), gpr(a), gpr(b));
        }
    }

    fn lower(&mut self, value: Value) {
        let node = self.func.node(value);
        match node.op {
            op if op.is_terminator() => self.lower_terminator(value),
            Op::Load
            | Op::Store
            | Op::GlobalGet
            | Op::GlobalSet
            | Op::InstanceField
            | Op::LoadPointer
            | Op::FrameLoad
            | Op::VectorLoad => self.lower_memory(value),
            Op::Call | Op::CallResult => self.lower_call(value),
            Op::Offset
            | Op::ReadField
            | Op::WriteField
            | Op::TableElement
            | Op::TableStore
            | Op::CalleeInstance
            | Op::CheckNull
            | Op::CheckFunction
            | Op::Helper => self.lower_runtime(value),
            Op::CheckedAddress
            | Op::AtomicRmw
            | Op::AtomicCompareExchange
            | Op::AtomicFence
            | Op::Rtt
            | Op::I31New
            | Op::I31Get
            | Op::ElementAddress
            | Op::RefTest => self.lower_gc(value),
            Op::VectorUnary
            | Op::VectorBinary
            | Op::VectorTernary
            | Op::ExtractLane
            | Op::ReplaceLane
            | Op::Shuffle => self.lower_vector(value),
            _ => self.lower_numeric(value),
        }
    }

    /// The constant of input `index` of `value`, which code generation uses as an immediate.
    fn immediate(&self, value: Value, index: usize) -> u64 {
        let input = self.func.inputs(value)[index];
        self.func.node(input).constant().expect("immediates are constants")
    }

    /// The register of an input that may be a constant: its own, or x16 with the constant in it.
    fn input_or_scratch(&mut self, value: Value, index: usize, register: Register, ty: Type) -> Gpr {
        if register != crate::regalloc::CONSTANT_INPUT {
            return gpr(register);
        }
        let bits = self.immediate(value, index);
        let bits = if ty.is_64() { bits } else { bits & 0xffff_ffff };
        if bits == 0 {
            return XZR;
        }
        self.asm.mov_imm(Width::W64, SCRATCH, bits);
        SCRATCH
    }
}

/// Generates the code of `func`.
pub fn generate(
    func: &Func,
    allocation: &Allocation,
    runtime: &RuntimeInfo,
) -> Result<Generated, crate::CompileFailure> {
    let _ = (NONE, Shift::Lsl);
    CodeGenerator::new(func, allocation, runtime).generate()
}
