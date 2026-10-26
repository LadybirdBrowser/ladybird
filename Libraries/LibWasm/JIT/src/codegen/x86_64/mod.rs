/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! x86-64 code generation: the blocks in linear order, each node lowered with the registers the allocator gave it
//! (`numeric`, `memory`, `calls`), then the out-of-line code: stubs for edges that need moves, trap stubs, jump
//! tables and the constant pool. The code is position independent: it reaches the runtime through absolute
//! addresses and its own data through rip-relative operands.

mod calls;
mod gc;
mod memory;
mod numeric;
mod tables;
mod vector;

pub(crate) use vector::{is_vector_shift, vector_constant_inputs, vector_temps};

use crate::RuntimeInfo;
use crate::asm::x86_64::{Assembler, BitwiseOp, Cond, Precision, VexOperand};
use crate::asm::{Address, Fpr, Gpr, Label, Width};
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
    debug_assert!(convention::is_fpr(register), "r{register} is no xmm register");
    Fpr(register - convention::FPR_BASE)
}

pub(crate) fn width(ty: Type) -> Width {
    if ty.is_64() { Width::W64 } else { Width::W32 }
}

pub(crate) fn precision(ty: Type) -> Precision {
    if ty == Type::F64 {
        Precision::Double
    } else {
        Precision::Single
    }
}

const SCRATCH: Gpr = Gpr(convention::SCRATCH_GPR);
const SCRATCH_FPR: Fpr = Fpr(convention::SCRATCH_FPR - convention::FPR_BASE);
const FP: Gpr = Gpr(convention::RBP);
const SP: Gpr = Gpr(convention::RSP);

pub(crate) fn condition(condition: IntCondition) -> Cond {
    match condition {
        IntCondition::Eq => Cond::Equal,
        IntCondition::Ne => Cond::NotEqual,
        IntCondition::LtS => Cond::Less,
        IntCondition::LtU => Cond::Below,
        IntCondition::GtS => Cond::Greater,
        IntCondition::GtU => Cond::Above,
        IntCondition::LeS => Cond::LessOrEqual,
        IntCondition::LeU => Cond::BelowOrEqual,
        IntCondition::GeS => Cond::GreaterOrEqual,
        IntCondition::GeU => Cond::AboveOrEqual,
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
    /// The trap with the code in eax, for the runtime's functions that return one.
    dynamic_trap: Option<Label>,
    subtype_checks: Vec<tables::SubtypeCheck>,
    edge_stubs: Vec<EdgeStub>,
    /// Jump tables: their label, and the label of each target.
    tables: Vec<(Label, Vec<Label>)>,
    constants: Vec<(u128, Label)>,
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
            asm: Assembler::for_host(),
            block_labels: Vec::new(),
            trap_labels: vec![None; 32],
            dynamic_trap: None,
            subtype_checks: Vec::new(),
            edge_stubs: Vec::new(),
            tables: Vec::new(),
            constants: Vec::new(),
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
        self.asm.push(FP);
        self.asm.lea(Width::W64, FP, &Address::new(SP, 16));
        // NB: The instance goes in the frame before the stack check, which traps through the frame. It is in the
        //     red zone until rsp moves below it.
        self.asm.mov_mr(
            Width::W64,
            &Address::new(FP, convention::FRAME_INSTANCE),
            gpr(convention::INSTANCE_REGISTER),
        );
        // NB: rax carries nothing into a function, so it is free here.
        let context = Gpr(convention::RAX);
        self.asm.mov_rm(
            Width::W64,
            context,
            &Address::new(
                gpr(convention::INSTANCE_REGISTER),
                offset_of!(InstanceData, context) as i32,
            ),
        );
        let below = self.frame_size as i32 - 16;
        self.asm.lea(Width::W64, SCRATCH, &Address::new(SP, -below));
        self.asm.alu_rm(
            crate::asm::x86_64::AluOp::Cmp,
            Width::W64,
            SCRATCH,
            &Address::new(context, offset_of!(ExecutionContext, stack_limit) as i32),
        );
        let overflow = self.trap_label(libwasm_module::abi::TRAP_STACK_EXHAUSTED);
        self.asm.jcc(Cond::Below, overflow);
        self.asm.mov_rr(Width::W64, SP, SCRATCH);
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
            .mov_rm(Width::W64, context, &Address::new(FP, convention::FRAME_INSTANCE));
        self.asm.mov_rm(
            Width::W64,
            context,
            &Address::new(context, offset_of!(InstanceData, context) as i32),
        );
        let arguments = offset_of!(ExecutionContext, arguments) as i32;
        self.asm
            .mov_mr(Width::W64, &Address::new(context, arguments), exception);
        self.asm.mov_mr(Width::W64, &Address::new(context, arguments + 8), FP);
        self.asm.mov_rr(Width::W64, Gpr(convention::RDI), context);
        let target = Gpr(convention::RAX);
        self.asm.mov_ri(target, self.runtime.throw);
        self.asm.call_r(target);
        crate::stubs::resume(&mut self.asm, target);
    }

    fn epilogue(&mut self) {
        self.asm
            .lea(Width::W64, SP, &Address::new(FP, convention::FRAME_SAVED_FP));
        self.asm.pop(FP);
        self.asm.ret();
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

    fn constant_label(&mut self, bits: u128) -> Label {
        if let Some((_, label)) = self.constants.iter().find(|(constant, _)| *constant == bits) {
            return *label;
        }
        let label = self.asm.new_label();
        self.constants.push((bits, label));
        label
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
            self.asm.jmp(target);
        }
        for code in 0..self.trap_labels.len() {
            if let Some(label) = self.trap_labels[code] {
                self.asm.bind(label);
                self.asm.mov_ri32(Gpr(convention::RDI), code as u32);
                self.asm.mov_ri(SCRATCH, self.runtime.trap_exit);
                self.asm.jmp_r(SCRATCH);
            }
        }
        if let Some(label) = self.dynamic_trap {
            self.asm.bind(label);
            self.asm.mov_rr(Width::W32, Gpr(convention::RDI), Gpr(convention::RAX));
            self.asm.mov_ri(SCRATCH, self.runtime.trap_exit);
            self.asm.jmp_r(SCRATCH);
        }
        let tables = std::mem::take(&mut self.tables);
        for (table, targets) in &tables {
            self.asm.align_data(4);
            self.asm.bind(*table);
            let base = self.asm.label_offset(*table).expect("the table was just bound");
            for target in targets {
                let offset = self
                    .asm
                    .label_offset(*target)
                    .expect("jump targets are bound before the tables");
                self.asm.data_u32((offset as i64 - base as i64) as i32 as u32);
            }
        }
        if !self.constants.is_empty() {
            self.asm.align_data(16);
            let constants = std::mem::take(&mut self.constants);
            for (bits, label) in constants {
                self.asm.bind(label);
                self.asm.data_u64(bits as u64);
                self.asm.data_u64((bits >> 64) as u64);
            }
        }
    }

    // Moves.

    fn store_to_slot(&mut self, register: Register, slot: i32, vector: bool) {
        let address = Address::new(FP, slot);
        if vector {
            self.store_vector(register, &address);
        } else if convention::is_fpr(register) {
            self.asm.vmovs_mr(Precision::Double, &address, fpr(register));
        } else {
            self.asm.mov_mr(Width::W64, &address, gpr(register));
        }
    }

    fn address(&self, location: Location) -> Address {
        match location {
            Location::Slot(slot) => Address::new(FP, slot),
            Location::Outgoing(index) => Address::new(SP, 8 * index as i32),
            _ => unreachable!("{location:?} is no frame location"),
        }
    }

    /// The bits of a constant of any type.
    fn constant_bits(&self, value: Value) -> u128 {
        let node = self.func.node(value);
        match node.op {
            Op::Const128 => self.func.constants[node.args[0] as usize],
            _ => u128::from(node.constant().expect("only constants have bits")),
        }
    }

    /// Loads the constant `bits` into the xmm register `register`, as 4, 8 or 16 bytes as `ty` says.
    fn load_constant(&mut self, ty: Type, bits: u128, register: Register) {
        let label = self.constant_label(bits);
        if ty == Type::V128 {
            self.asm
                .vex(vector::VMOVUPS_LOAD, fpr(register).0, 0, VexOperand::Label(label), None);
        } else {
            let p = if ty.is_64() {
                Precision::Double
            } else {
                Precision::Single
            };
            self.asm.vmovs_r_label(p, fpr(register), label);
        }
    }

    /// Materializes the constant `value` into `register`.
    fn materialize(&mut self, value: Value, register: Register) {
        let ty = self.func.node(value).ty;
        let bits = self.constant_bits(value);
        if convention::is_fpr(register) {
            if bits == 0 {
                let register = fpr(register);
                self.asm.vbitwise_rrr(BitwiseOp::Xor, register, register, register);
            } else {
                self.load_constant(ty, bits, register);
            }
        } else if bits == 0 {
            self.asm.xor_rr(Width::W32, gpr(register), gpr(register));
        } else {
            self.asm.mov_ri(gpr(register), bits as u64);
        }
    }

    /// Stores the constant `value` at `address`, as 8 bytes, or 16 for a vector.
    fn store_constant(&mut self, value: Value, address: &Address) {
        let node = self.func.node(value);
        if node.op == Op::Const128 {
            self.materialize(value, convention::SCRATCH_FPR);
            self.store_vector(convention::SCRATCH_FPR, address);
            return;
        }
        let bits = node.constant().expect("only constants are stored as constants");
        let bits = if node.ty.is_64() { bits } else { bits & 0xffff_ffff };
        if i32::try_from(bits as i64).is_ok() {
            self.asm.mov_mi(Width::W64, address, bits as i64 as i32);
        } else {
            self.asm.mov_ri(SCRATCH, bits);
            self.asm.mov_mr(Width::W64, address, SCRATCH);
        }
    }

    pub(crate) fn emit_move(&mut self, item: &Move) {
        match (item.from, item.to) {
            (Location::Register(from), Location::Register(to)) => {
                if from == to {
                    return;
                }
                match (convention::is_fpr(from), convention::is_fpr(to)) {
                    (true, true) => self.asm.vmovaps_rr(fpr(to), fpr(from)),
                    (false, false) => self.asm.mov_rr(Width::W64, gpr(to), gpr(from)),
                    (true, false) => self.asm.vmov_rx(Width::W64, gpr(to), fpr(from)),
                    (false, true) => self.asm.vmov_xr(Width::W64, fpr(to), gpr(from)),
                }
            }
            (Location::Constant(value), Location::Register(to)) => self.materialize(value, to),
            (Location::Register(from), to @ (Location::Slot(_) | Location::Outgoing(_))) => {
                let address = self.address(to);
                if item.vector {
                    self.store_vector(from, &address);
                } else if convention::is_fpr(from) {
                    self.asm.vmovs_mr(Precision::Double, &address, fpr(from));
                } else {
                    self.asm.mov_mr(Width::W64, &address, gpr(from));
                }
            }
            (from @ (Location::Slot(_) | Location::Outgoing(_)), Location::Register(to)) => {
                let address = self.address(from);
                if item.vector {
                    self.load_vector(fpr(to).0, &address);
                } else if convention::is_fpr(to) {
                    self.asm.vmovs_rm(Precision::Double, fpr(to), &address);
                } else {
                    self.asm.mov_rm(Width::W64, gpr(to), &address);
                }
            }
            (Location::Constant(value), to @ (Location::Slot(_) | Location::Outgoing(_))) => {
                let address = self.address(to);
                self.store_constant(value, &address);
            }
            (from @ Location::Slot(_), to @ (Location::Slot(_) | Location::Outgoing(_))) => {
                let (from, to) = (self.address(from), self.address(to));
                if item.vector {
                    self.load_vector(SCRATCH_FPR.0, &from);
                    self.store_vector(convention::SCRATCH_FPR, &to);
                } else {
                    self.asm.mov_rm(Width::W64, SCRATCH, &from);
                    self.asm.mov_mr(Width::W64, &to, SCRATCH);
                }
            }
            (from, Location::ResultArea(index)) => {
                // The value goes through the scratch xmm register, since the area's address takes the scratch one.
                let source = match from {
                    Location::Register(register) if convention::is_fpr(register) => fpr(register),
                    Location::Register(register) => {
                        self.asm.vmov_xr(Width::W64, SCRATCH_FPR, gpr(register));
                        SCRATCH_FPR
                    }
                    Location::Slot(slot) => {
                        self.load_vector(SCRATCH_FPR.0, &Address::new(FP, slot));
                        SCRATCH_FPR
                    }
                    Location::Constant(value) => {
                        self.materialize(value, convention::SCRATCH_FPR);
                        SCRATCH_FPR
                    }
                    _ => unreachable!("results do not come from {from:?}"),
                };
                let pointer = convention::home_slot_offset(convention::result_area_pointer(&self.func.params));
                self.asm.mov_rm(Width::W64, SCRATCH, &Address::new(FP, pointer));
                let destination = Address::new(SCRATCH, 8 * index as i32);
                if item.vector {
                    self.asm.vex(
                        vector::VMOVUPS_STORE,
                        source.0,
                        0,
                        VexOperand::Memory(destination),
                        None,
                    );
                } else {
                    self.asm.vmovs_mr(Precision::Double, &destination, source);
                }
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
            self.asm.jmp(label);
        }
    }

    /// Ends the block with a two-way branch whose flags `emit` sets up and whose condition is true for the first
    /// successor. `emit_jumps(generator, taken, not_taken)` jumps to `taken` where the condition holds and may jump
    /// to `not_taken`, which is bound right after, where it does not.
    fn branch(&mut self, emit_jumps: impl Fn(&mut Self, Label, Label)) {
        let block = self.block;
        let data = self.func.blocks[block as usize];
        let successors = self.func.successors(block);
        let (taken, fallthrough) = (successors[0], successors[1]);
        let taken_slot = data.successors_start;
        let fallthrough_slot = taken_slot + 1;
        let taken_label = self.edge_label(taken_slot, taken);
        let not_taken = self.asm.new_label();
        emit_jumps(self, taken_label, not_taken);
        self.asm.bind(not_taken);
        for item in self.allocation.edge(fallthrough_slot) {
            self.emit_move(item);
        }
        if fallthrough != block + 1 {
            let label = self.block_labels[fallthrough as usize];
            self.asm.jmp(label);
        }
    }

    /// A branch on an integer condition, inverted where the taken successor comes next and needs no moves.
    fn branch_on(&mut self, cond: Cond) {
        let block = self.block;
        let data = self.func.blocks[block as usize];
        let successors = self.func.successors(block);
        let (taken, fallthrough) = (successors[0], successors[1]);
        if taken == block + 1 && self.allocation.edge(data.successors_start).is_empty() {
            let label = self.edge_label(data.successors_start + 1, fallthrough);
            self.asm.jcc(cond.invert(), label);
            return;
        }
        self.branch(|generator, taken, _| generator.asm.jcc(cond, taken));
    }

    fn lower_terminator(&mut self, value: Value) {
        let func = self.func;
        let node = *func.node(value);
        let record = *self.allocation.node(value);
        match node.op {
            Op::Jump => self.jump(0),
            Op::Branch => {
                let register = gpr(record.inputs[0]);
                self.asm.test_rr(Width::W32, register, register);
                self.branch_on(Cond::NotEqual);
            }
            Op::BranchCompare => {
                let ty = func.node(node.args[0]).ty;
                self.compare(ty, record.inputs[0], record.inputs[1], node.args[1]);
                self.branch_on(condition(IntCondition::from_flags(node.flags)));
            }
            Op::BranchFloatCompare => {
                let ty = func.node(node.args[0]).ty;
                let (a, b) = (fpr(record.inputs[0]), fpr(record.inputs[1]));
                match FloatCondition::from_flags(node.flags) {
                    FloatCondition::Gt => {
                        self.asm.vucomis(precision(ty), a, b);
                        self.branch_on(Cond::Above);
                    }
                    FloatCondition::Ge => {
                        self.asm.vucomis(precision(ty), a, b);
                        self.branch_on(Cond::AboveOrEqual);
                    }
                    FloatCondition::Lt => {
                        self.asm.vucomis(precision(ty), b, a);
                        self.branch_on(Cond::Above);
                    }
                    FloatCondition::Le => {
                        self.asm.vucomis(precision(ty), b, a);
                        self.branch_on(Cond::AboveOrEqual);
                    }
                    FloatCondition::Eq => {
                        self.asm.vucomis(precision(ty), a, b);
                        self.branch(|generator, taken, not_taken| {
                            generator.asm.jcc(Cond::Parity, not_taken);
                            generator.asm.jcc(Cond::Equal, taken);
                        });
                    }
                    FloatCondition::Ne => {
                        self.asm.vucomis(precision(ty), a, b);
                        self.branch(|generator, taken, _| {
                            generator.asm.jcc(Cond::Parity, taken);
                            generator.asm.jcc(Cond::NotEqual, taken);
                        });
                    }
                }
            }
            Op::BrTable => self.branch_table(value),
            Op::Return => self.epilogue(),
            Op::TailCall => self.lower_tail_call(value),
            Op::Trap => {
                let label = self.trap_label(node.args[0]);
                self.asm.jmp(label);
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
        let temp = gpr(record.temps[0]);
        let successors = func.successors(block);
        let mut labels = Vec::with_capacity(successors.len());
        for (position, &successor) in successors.iter().enumerate() {
            let label = self.edge_label(data.successors_start + position as u32, successor);
            labels.push(label);
        }
        let targets = func.table_targets(value);
        let (cases, default) = targets.split_at(targets.len() - 1);
        let default = labels[default[0] as usize];
        self.asm.cmp_ri(Width::W32, index, cases.len() as i32);
        self.asm.jcc(Cond::AboveOrEqual, default);
        let table = self.asm.new_label();
        self.asm.lea_label(SCRATCH, table);
        self.asm
            .movsxd_rm(temp, &Address::indexed(SCRATCH, index, crate::asm::Scale::Four, 0));
        self.asm.add_rr(Width::W64, temp, SCRATCH);
        self.asm.jmp_r(temp);
        let case_labels = cases.iter().map(|case| labels[*case as usize]).collect();
        self.tables.push((table, case_labels));
    }

    /// `cmp a, b` of type `ty`, `b` an immediate when it is the constant `b_value`.
    /// The slot of `input`, an input the allocator left there for the node to read (`SLOT_INPUT`).
    fn input_slot(&self, input: Value) -> Address {
        let slot = self.allocation.slots[input as usize];
        debug_assert!(slot != NO_SLOT, "v{input} has no slot");
        Address::new(FP, slot)
    }

    fn compare(&mut self, ty: Type, a: Register, b: Register, b_value: Value) {
        if b == crate::regalloc::SLOT_INPUT {
            let address = self.input_slot(b_value);
            self.asm
                .alu_rm(crate::asm::x86_64::AluOp::Cmp, width(ty), gpr(a), &address);
        } else if b == crate::regalloc::CONSTANT_INPUT {
            let bits = self.func.node(b_value).constant().expect("a constant");
            self.asm.cmp_ri(width(ty), gpr(a), bits as i32);
        } else {
            self.asm.cmp_rr(width(ty), gpr(a), gpr(b));
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
            | Op::FrameLoad => {
                self.lower_memory(value);
            }
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
            Op::VectorLoad => self.lower_memory(value),
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
}

/// Generates the code of `func`.
pub fn generate(
    func: &Func,
    allocation: &Allocation,
    runtime: &RuntimeInfo,
) -> Result<Generated, crate::CompileFailure> {
    let _ = NONE;
    CodeGenerator::new(func, allocation, runtime).generate()
}
