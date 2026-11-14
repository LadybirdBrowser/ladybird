/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Instructions built as calls of their slow path on SSA values (see
//! `Op::CallSlowPath`): the slow path gets the values of the operands it
//! reads as the node's inputs, and its outputs become SSA values, so the
//! compiled function's frame neither holds nor receives them.

use super::Flow;
use super::GenericInfo;
use super::GraphBuilder;
use crate::CompileFailure;
use crate::bytecode::FrameLayout;
use crate::bytecode::Instruction;
use crate::bytecode::Operand;
use crate::bytecode::OperandRole;
use crate::bytecode::slow_path_layout;
use crate::code::Repr;
use crate::codegen::slow_path_call;
use crate::ir::Op;
use crate::ir::value;

/// The operands of a slow path, as raw operands of its instruction.
pub(crate) struct SlowPathOperands {
    /// The operands it reads, in the order of its layout (see
    /// `bytecode::slow_path_layout()`): the fields that are not outputs,
    /// then the array's elements. `Operand::INVALID` for absent ones.
    pub(crate) inputs: Vec<u32>,
    /// The operands it writes, with the index of their layout field, in
    /// layout order.
    pub(crate) outputs: Vec<(u32, u32)>,
}

/// Whether `operand`, an output of a slow path in a frame of `layout`, becomes an SSA value: any slot but a
/// constant, which instructions may write too, and which lives in frame memory.
pub(crate) fn is_ssa_slow_path_output(layout: &FrameLayout, operand: Operand) -> bool {
    layout.tracked_index(operand).is_some()
}

/// The operands of a JS call instruction, whose slow path is the runtime's
/// call helper (`SlowPathCall::JitCall`): the operands it reads, in the order
/// the instruction lists them, and its destination.
pub(crate) fn js_call_operands(instruction: &Instruction) -> SlowPathOperands {
    let mut inputs = Vec::new();
    let mut outputs = Vec::new();
    instruction.for_each_operand(|operand, role| {
        if role.is_read() {
            inputs.push(operand.raw());
        }
        if role.is_written() {
            outputs.push((0, operand.raw()));
        }
    });
    SlowPathOperands { inputs, outputs }
}

impl GraphBuilder<'_> {
    /// The operands of the slow path of the instruction being built.
    pub(super) fn slow_path_operands(&self) -> Result<SlowPathOperands, CompileFailure> {
        let opcode = self.function.instructions[self.function.instruction_index]
            .instruction
            .opcode();
        let layout = slow_path_layout(opcode);
        let bytes_at = |offset: u32, size: usize| -> Result<&[u8], CompileFailure> {
            let at = (self.function.pc + offset) as usize;
            self.function
                .executable
                .bytecode
                .get(at..at + size)
                .ok_or(CompileFailure::InvalidBytecode {
                    pc: self.function.pc,
                    reason: "operand field outside the bytecode",
                })
        };
        let read_operand =
            |offset: u32| -> Result<u32, CompileFailure> { Ok(Operand::raw_from_byte(bytes_at(offset, 1)?[0])) };
        let mut inputs = Vec::new();
        let mut outputs = Vec::new();
        for (index, field) in layout.fields.iter().enumerate() {
            let operand = read_operand(field.instruction_offset)?;
            if field.role != OperandRole::Out {
                inputs.push(operand);
            }
            if field.role != OperandRole::In && operand != Operand::INVALID {
                outputs.push((u32::try_from(index).expect("layouts have few fields"), operand));
            }
        }
        if let Some(array) = layout.array {
            for element in 0..u32::from(bytes_at(array.count_offset, 1)?[0]) {
                inputs.push(read_operand(array.instruction_offset + element)?);
            }
        }
        Ok(SlowPathOperands { inputs, outputs })
    }

    /// Builds an instruction as a call of its slow path on SSA values (see
    /// `Op::CallSlowPath`), unless its slow path reads more of the frame than
    /// its operands: then returns `None`, and the instruction becomes a
    /// `Generic` node.
    pub(super) fn try_build_slow_path_call(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
    ) -> Result<Option<Flow>, CompileFailure> {
        let opcode = instruction.opcode();
        if info.reads_whole_frame || slow_path_call(opcode).is_none() {
            return Ok(None);
        }
        // NB: An arguments object the code never created only exists once
        //     something reads it from the frame (see `arguments`).
        let mut reads_frame = false;
        instruction.for_each_operand(|operand, role| {
            if role.is_read() && self.holds_virtual_arguments(operand) {
                reads_frame = true;
            }
        });
        if reads_frame {
            return Ok(None);
        }
        let operands = if info.is_js_call {
            js_call_operands(instruction)
        } else {
            self.slow_path_operands()?
        };
        let inputs = operands
            .inputs
            .iter()
            .map(|operand| match *operand {
                Operand::INVALID => Ok(self.constant(value::EMPTY)),
                raw => self.read(Operand::from_raw(raw)),
            })
            .collect::<Result<Vec<_>, _>>()?;

        let mut destination = None;
        instruction.for_each_operand(|operand, role| {
            if role.is_written() {
                destination.get_or_insert(operand);
            }
        });

        let mut targets = Vec::new();
        instruction.for_each_jump_target(|label| targets.push(label));
        let repr = if !targets.is_empty() {
            Some(Repr::Int32)
        } else if operands
            .outputs
            .first()
            .is_some_and(|(_, operand)| is_ssa_slow_path_output(&self.function.layout, Operand::from_raw(*operand)))
        {
            Some(Repr::Tagged)
        } else {
            None
        };
        let node = self.emit(
            Op::CallSlowPath {
                opcode,
                executable: self.function.index,
                pc: self.function.pc,
                saves_registers: false,
            },
            inputs,
            repr,
        );

        // NB: Where the slow path does not continue with the next
        //     instruction, it threw or runs a callee in the interpreter. The
        //     frame then gets the frame state's values, with the old values
        //     of the outputs the slow path did not write, except that it
        //     writes back the outputs it reads (as the interpreter does on
        //     exceptions): the frame state lists those as held by the frame.
        let input_outputs = operands
            .outputs
            .iter()
            .filter(|(field, _)| {
                !info.is_js_call && slow_path_layout(opcode).fields[*field as usize].role == OperandRole::InOut
            })
            .filter_map(|(_, operand)| self.function.layout.tracked_index(Operand::from_raw(*operand)))
            .collect::<Vec<_>>();
        let frame_state = self.resume_after_frame_state_with_outputs_in_frame(
            destination.map_or(Operand::INVALID, Operand::raw),
            &input_outputs,
        );
        self.graph.nodes[node.index()].frame_state = Some(frame_state);

        for (index, (_, operand)) in operands.outputs.iter().enumerate() {
            let operand = Operand::from_raw(*operand);
            if !is_ssa_slow_path_output(&self.function.layout, operand) {
                continue;
            }
            let value = if index == 0 {
                node
            } else {
                let index = u8::try_from(index).expect("slow paths have few outputs");
                self.emit(Op::SlowPathOutput { index }, vec![node], Some(Repr::Tagged))
            };
            self.write(operand, value)?;
        }
        self.end_generic_instruction(instruction, node).map(Some)
    }
}
