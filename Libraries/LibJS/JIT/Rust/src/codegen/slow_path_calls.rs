/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Calls of slow paths on values (`Op::CallSlowPath`), the publication of
//! the frames of inlined calls they and calls run in, and the out of line
//! code of nodes.

use super::Codegen;
use crate::CompileFailure;
use crate::asm::Address;
use crate::asm::Gpr;
use crate::asm::GprSet;
use crate::asm::PortableMacroAssembler;
use crate::bytecode::OpCode;
use crate::code::Repr;
use crate::code::SiteKind;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use crate::regalloc::Location;
use crate::regalloc::NodeAllocation;

/// The slow path call of one instruction, of a `CallSlowPath` node.
#[derive(Debug, Clone, Copy)]
pub(super) struct SlowPath {
    pub(super) node: NodeId,
    pub(super) opcode: OpCode,
    pub(super) executable: u32,
}

/// The registers a slow path call clobbers that may hold values.
pub(super) fn slow_path_saved_registers<M: PortableMacroAssembler>() -> GprSet {
    M::CALLER_SAVED_GPRS.intersection(M::ALLOCATABLE_GPRS)
}

/// The registers a slow path call of the node `allocation` is of saves, in
/// the order of their words in the saves area: those that may hold values,
/// except its output register, which is last if an input is there too, since
/// passing the other inputs to the slow path may clobber it.
pub(super) fn slow_path_saved_registers_of<M: PortableMacroAssembler>(allocation: &NodeAllocation) -> Vec<Gpr> {
    let output = match allocation.output {
        Some(Location::Register(register)) => Some(Gpr(register)),
        _ => None,
    };
    let mut saved = slow_path_saved_registers::<M>();
    if let Some(output) = output {
        saved = saved.without(output);
    }
    let mut saved = saved.iter().collect::<Vec<_>>();
    if let Some(output) = output
        && allocation.inputs.contains(&Location::Register(output.0))
    {
        saved.push(output);
    }
    saved
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// The out of line code of nodes, after the last block.
    pub(super) fn emit_deferred_code(&mut self) -> Result<(), CompileFailure> {
        self.emit_deferred_cache_probes()?;
        self.emit_deferred_allocations();
        self.emit_deferred_storage_growths();
        Ok(())
    }

    /// Publishes the frames of the inlined calls a slow path call `node` is
    /// in, for its slow path to run in the innermost one: the runtime pushes
    /// them with their headers (see `SiteKind::Publish`). Clobbers every
    /// register a call clobbers. Returns how many frames were pushed, which
    /// compiled code pops again if the slow path continues in it.
    pub(super) fn emit_publish_frames(
        &mut self,
        node: NodeId,
        saved: &[Gpr],
    ) -> Result<(usize, super::InputRegisters), CompileFailure> {
        if !self.publishes_frames(node) {
            return Ok((0, super::InputRegisters::Saved(saved.to_vec())));
        }
        let frame_state = self
            .graph
            .node(node)
            .frame_state
            .expect("publishing nodes have a frame state");
        let chain = self.graph.frame_state_chain(frame_state);
        let pushed = chain.len() - 1;
        // NB: Compiled code writes the headers itself, unless a value they
        //     need is one it cannot write (a virtual object).
        let compiled_code_writes_headers = saved.len() >= 5
            && chain[..pushed].iter().all(|id| {
                let frame_state = self.graph.frame_state(*id);
                let arguments_base = self.executables[frame_state.executable as usize]
                    .layout
                    .arguments_base();
                frame_state.values.iter().all(|(slot, value)| {
                    let is_header = *slot == crate::bytecode::THIS_VALUE_REGISTER
                        || *slot == crate::ir::CALLEE_SLOT
                        || (*slot >= arguments_base && *slot != crate::ir::VIRTUAL_OBJECT_PROPERTY);
                    !is_header
                        || !(matches!(self.graph.node(*value).op, Op::VirtualObject { .. })
                            || self.graph.node(*value).repr == Some(Repr::Pointer))
                })
            });
        if compiled_code_writes_headers {
            self.emit_published_frames(node, saved)?;
            return Ok((pushed, super::InputRegisters::Saved(saved.to_vec())));
        }
        let index = u32::try_from(self.sites.len()).expect("site count fits in u32");
        self.sites.push((node, SiteKind::Publish));
        let exit_index = self.local_address(self.locals.exit_index);
        self.masm.store_imm32(&exit_index, index);
        self.emit_exit_runtime_call();
        let frame = self.pinned.frame;
        self.masm.load64(
            frame,
            &Address::new(self.pinned.vm, self.runtime.offsets.vm_running_execution_context as i32),
        );
        Ok((pushed, super::InputRegisters::Dumped))
    }

    /// Whether `node` runs in the published frames of the inlined calls it is
    /// in (see `emit_publish_frames()`): a slow path call or a call in an
    /// inlined callee that does not run without those frames.
    pub(super) fn publishes_frames(&self, node: NodeId) -> bool {
        let node = self.graph.node(node);
        let in_inlined_callee = node
            .frame_state
            .is_some_and(|frame_state| self.graph.frame_state(frame_state).parent.is_some());
        in_inlined_callee
            && matches!(
                node.op,
                Op::CallSlowPath { .. }
                    | Op::CallDirect {
                        inlined_frame_bytes: 0,
                        ..
                    }
                    | Op::CallNative {
                        inlined_frame_bytes: 0,
                        ..
                    }
            )
    }

    /// The address of word `index` of the saves area.
    pub(super) fn save_address(&self, index: usize) -> Address {
        self.local_address(self.locals.saves + 8 * index as u32)
    }

    /// Stores `registers` into the saves area, in order.
    pub(super) fn emit_save_registers(&mut self, registers: &[Gpr]) {
        for (index, register) in registers.iter().enumerate() {
            let address = self.save_address(index);
            self.masm.store64(&address, *register);
        }
    }

    /// Loads `registers` back from the saves area, but `except`, which holds
    /// a result.
    pub(super) fn emit_restore_registers(&mut self, registers: &[Gpr], except: Option<Gpr>) {
        for (index, register) in registers.iter().enumerate() {
            if Some(*register) != except {
                let address = self.save_address(index);
                self.masm.load64(*register, &address);
            }
        }
    }

    /// Whether the code of `node` uses the saves area: slow paths and the
    /// runtime calls that save registers like them, calls that publish
    /// frames, and stores that may grow named property storage.
    pub(super) fn uses_saves_area(&self, node: NodeId) -> bool {
        match self.graph.node(node).op {
            Op::CallSlowPath { .. }
            | Op::ProbePropertyCache { .. }
            | Op::ProbeHasProperty { .. }
            | Op::ProbePropertyStore { .. }
            | Op::ProbeKeyedStore { .. }
            | Op::CallArrayPush
            | Op::AllocateObject { .. }
            | Op::AllocateArray { .. }
            | Op::AllocateFunction { .. }
            | Op::AllocateEnvironment { .. } => true,
            Op::CallDirect { .. } | Op::CallNative { .. } => self.publishes_frames(node),
            _ => false,
        }
    }

    /// A `SlowPathOutput` node: output `index` of the `CallSlowPath` node
    /// right before it, from the record of operands its slow path wrote.
    pub(super) fn emit_slow_path_output(&mut self, node: NodeId, index: u8) -> Result<(), CompileFailure> {
        let call = self.graph.node(node).inputs[0];
        let Op::CallSlowPath {
            opcode, executable, pc, ..
        } = self.graph.node(call).op
        else {
            unreachable!("slow path outputs are of slow path calls");
        };
        let layout = crate::bytecode::slow_path_layout(opcode);
        debug_assert_ne!(
            layout.abi,
            crate::bytecode::SlowPathAbi::Scalar,
            "slow paths with several outputs return them in the record"
        );
        let mut outputs = Vec::new();
        for (field_index, field) in layout.fields.iter().enumerate() {
            if field.role == crate::bytecode::OperandRole::In {
                continue;
            }
            if self.read_u32(executable, pc, field.instruction_offset)? != crate::bytecode::Operand::INVALID {
                outputs.push(field_index as u32);
            }
        }
        let field_index = outputs[usize::from(index)];
        let record = self.local_address(self.locals.record + 8 * field_index);
        self.masm.load64(self.output(node), &record);
        Ok(())
    }

    /// A `CallSlowPath` node: runs the slow path of its instruction with
    /// the node's inputs as operands, keeping every value in its register,
    /// and puts the slow path's output into the node's output register.
    pub(super) fn emit_call_slow_path(&mut self, slow_path: SlowPath) -> Result<(), CompileFailure> {
        let node = slow_path.node;
        let inlined = slow_path.executable != 0;
        if !inlined && self.graph.slow_paths_initializing_frame.contains(&node) {
            let initialized = self.masm.new_label();
            self.branch_if_frame_initialized(initialized);
            self.emit_frame_initialization(self.pinned.scratch, None)?;
            self.masm.bind(initialized);
        }
        let output = match self.allocation.node(node).output {
            Some(Location::Register(register)) => Some(Gpr(register)),
            _ => None,
        };
        let saved = slow_path_saved_registers_of::<M>(self.allocation.node(node));
        self.emit_save_registers(&saved);
        self.leave_frame = self.leave_frame_of(node).map(|node| super::LeaveFrame {
            node,
            restore: saved
                .iter()
                .enumerate()
                .map(|(index, register)| (*register, self.locals.saves + 8 * index as u32))
                .collect(),
        });
        // NB: In an inlined callee, the slow path runs in the published
        //     frames of the inlined calls.
        let (pushed, registers) = self.emit_publish_frames(node, &saved)?;
        let outputs = self.emit_slow_path_call_with_inputs(node, &registers)?;
        if let Some(output) = output {
            match (slow_path.opcode, outputs.as_slice()) {
                // NB: The slow path writes the this value register.
                (OpCode::ResolveThisBinding, []) => {
                    let this_value = self.slot_address(crate::bytecode::THIS_VALUE_REGISTER)?;
                    self.masm.load64(output, &this_value);
                }
                // NB: Outputs after the first stay in the record, for `SlowPathOutput` nodes.
                (_, [super::OutputSource::Register(register), ..]) => self.masm.move64(output, *register),
                (_, [super::OutputSource::Record(offset), ..]) => {
                    let record = self.local_address(*offset);
                    self.masm.load64(output, &record);
                }
                // NB: The value of a conditional jump is the control word.
                (_, []) => self.masm.move64(output, M::RETURN_GPRS[0]),
            }
        }
        for _ in 0..pushed {
            self.emit_pop_inline_frame();
        }
        self.emit_restore_registers(&saved, output);
        Ok(())
    }

    /// Pushes the frame of each inlined call the frame state of `node` is
    /// in, from the outside in, uninitialized, with its headers: the `this`
    /// value and the arguments (Header translation, see `SiteKind::Publish`).
    /// The `saved` registers, stored in the saves area, are free to clobber;
    /// the first five serve as temps.
    fn emit_published_frames(&mut self, node: NodeId, saved: &[Gpr]) -> Result<(), CompileFailure> {
        let scratch = self.pinned.scratch;

        let frame_state = self
            .graph
            .node(node)
            .frame_state
            .expect("fast paths have a frame state");
        // NB: Frame state values are listed innermost frame first.
        let chain = self.graph.frame_state_chain(frame_state);
        let mut locations = self.allocation.node(node).exit_values.clone().into_iter();
        let mut frames = Vec::new();
        for id in &chain {
            let frame = self.graph.frame_state(*id).clone();
            let values = frame
                .values
                .iter()
                .map(|(slot, value)| {
                    let (_, location) = locations.next().expect("every frame state value has a location");
                    let repr = self.graph.node(*value).repr.unwrap_or(Repr::Tagged);
                    (*slot, location, repr)
                })
                .collect::<Vec<_>>();
            frames.push((frame, values));
        }
        let mut clobbered = false;
        for level in (0..frames.len()).rev() {
            if level + 1 < frames.len() {
                let (caller, _) = &frames[level + 1];
                let (callee, values) = &frames[level];
                let call = self.decoded(caller.executable, caller.pc)?;
                let (argument_count, construct) = match call.instruction {
                    crate::bytecode::Instruction::Call { argument_count, .. } => (argument_count, false),
                    crate::bytecode::Instruction::CallConstruct { argument_count, .. } => (argument_count, true),
                    // NB: Property gets and sets call inlined getters and
                    //     setters.
                    crate::bytecode::Instruction::GetById { .. } => (0, false),
                    crate::bytecode::Instruction::PutById { .. } => (1, false),
                    _ => {
                        return Err(CompileFailure::InvalidBytecode {
                            pc: caller.pc,
                            reason: "inlined call is no Call",
                        });
                    }
                };
                let crate::code::ResumeMode::ResumeAfter { dst } = caller.mode else {
                    unreachable!("callers of inlined calls resume after them");
                };
                let this = saved[3];
                // NB: Writing `closure` and `this` clobbers saved registers,
                //     which frame state values may live in, so these two read
                //     from the saves area, which holds all of them.
                // NB: A frame of an inlined closure runs the closure that was
                //     called.
                let closure = match values.iter().find(|(slot, _, _)| *slot == crate::ir::CALLEE_SLOT) {
                    Some((_, location, repr)) => {
                        let closure = saved[4];
                        self.emit_frame_value(closure, *location, *repr, saved, true);
                        self.emit_unbox_cell(closure, closure);
                        Some(closure)
                    }
                    None => None,
                };
                match values
                    .iter()
                    .find(|(slot, _, _)| *slot == crate::bytecode::THIS_VALUE_REGISTER)
                {
                    Some((_, location, repr)) => self.emit_frame_value(this, *location, *repr, saved, true),
                    None => self.masm.move_imm64(this, value::EMPTY),
                }
                self.emit_push_inline_frame(
                    callee.executable,
                    caller.pc,
                    call.next_pc(),
                    dst,
                    caller.passed_argument_count.unwrap_or(argument_count),
                    construct,
                    this,
                    closure,
                    [saved[0], saved[1], saved[2]],
                )?;
                clobbered = true;
            }
            if level + 1 == frames.len() {
                continue;
            }
            let arguments_base = self.executables[frames[level].0.executable as usize]
                .layout
                .arguments_base();
            let values = frames[level].1.clone();
            for (slot, location, repr) in values {
                if slot == crate::ir::CALLEE_SLOT {
                    continue;
                }
                if slot != crate::bytecode::THIS_VALUE_REGISTER && slot < arguments_base {
                    continue;
                }
                // NB: The arguments object the code never created is only
                //     created if the slow path leaves the code (see
                //     `Codegen::leave_arguments`).
                if let Location::Constant(bits) = location
                    && repr == Repr::Tagged
                    && value::virtual_arguments_kind(bits).is_some()
                {
                    continue;
                }
                let address = self.slot_address(slot)?;
                if let Location::Constant(bits) = location {
                    self.masm.store_imm64(&address, value::boxed_constant(bits, repr));
                    continue;
                }
                self.emit_frame_value(scratch, location, repr, saved, clobbered);
                self.masm.store64(&address, scratch);
            }
        }
        Ok(())
    }

    /// Loads the boxed value of a frame state value at `location` into
    /// `dst`. Once `clobbered`, the `saved` registers hold what they held at
    /// the node in the saves area.
    fn emit_frame_value(&mut self, dst: Gpr, location: Location, repr: Repr, saved: &[Gpr], clobbered: bool) {
        match location {
            Location::Register(register) => match saved.iter().position(|saved| saved.0 == register) {
                Some(index) if clobbered => {
                    let address = self.save_address(index);
                    self.masm.load64(dst, &address);
                    self.emit_box(dst, dst, repr);
                }
                _ => self.emit_box_register(dst, register, repr),
            },
            Location::Stack(spill) => {
                let spill = self.spill_address(spill);
                self.masm.load64(dst, &spill);
                self.emit_box(dst, dst, repr);
            }
            Location::Constant(bits) => self.masm.move_imm64(dst, value::boxed_constant(bits, repr)),
        }
    }
}
