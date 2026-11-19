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
use crate::bytecode::SlowPathLayout;
use crate::bytecode::slow_path_layout;
use crate::code::Repr;
use crate::codegen::slow_path_call;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use ak::ZoneVec;

/// The operands of a slow path, as raw operands of its instruction.
#[derive(Clone, Copy)]
pub(crate) enum SlowPathOperands<'a> {
    /// The operands of the instruction at `at` of `bytecode` in the layout of its slow path (see
    /// `bytecode::slow_path_layout()`), all of whose operand fields lie within the bytecode.
    Layout {
        bytecode: &'a [u8],
        at: usize,
        layout: SlowPathLayout,
        array_length: u32,
    },
    /// The operands of a JS call instruction (see `js_call_operands()`).
    JsCall(Instruction<'a>),
}

impl SlowPathOperands<'_> {
    /// Calls `visit` with the operands the slow path reads, in the order of its layout (see
    /// `bytecode::slow_path_layout()`): the fields that are not outputs, then the array's elements.
    /// `Operand::INVALID` for absent ones.
    pub(crate) fn for_each_input(&self, mut visit: impl FnMut(u32)) {
        match self {
            SlowPathOperands::Layout {
                bytecode,
                at,
                layout,
                array_length,
            } => {
                let read_operand = |offset: u32| Operand::raw_from_byte(bytecode[at + offset as usize]);
                for field in layout.fields {
                    if field.role != OperandRole::Out {
                        visit(read_operand(field.instruction_offset));
                    }
                }
                if let Some(array) = layout.array {
                    for element in 0..*array_length {
                        visit(read_operand(array.instruction_offset + element));
                    }
                }
            }
            SlowPathOperands::JsCall(instruction) => instruction.for_each_operand(|operand, role| {
                if role.is_read() {
                    visit(operand.raw());
                }
            }),
        }
    }

    /// Calls `visit` with the operands the slow path writes, with the index of their layout field, in layout order.
    pub(crate) fn for_each_output(&self, mut visit: impl FnMut(u32, u32)) {
        match self {
            SlowPathOperands::Layout {
                bytecode, at, layout, ..
            } => {
                for (index, field) in layout.fields.iter().enumerate() {
                    let operand = Operand::raw_from_byte(bytecode[at + field.instruction_offset as usize]);
                    if field.role != OperandRole::In && operand != Operand::INVALID {
                        visit(u32::try_from(index).expect("layouts have few fields"), operand);
                    }
                }
            }
            SlowPathOperands::JsCall(instruction) => instruction.for_each_operand(|operand, role| {
                if role.is_written() {
                    visit(0, operand.raw());
                }
            }),
        }
    }

    /// How many operands the slow path writes.
    pub(crate) fn output_count(&self) -> usize {
        let mut count = 0;
        self.for_each_output(|_, _| count += 1);
        count
    }

    /// The first operand the slow path writes, if it writes one.
    fn first_output(&self) -> Option<u32> {
        let mut first = None;
        self.for_each_output(|_, operand| {
            first.get_or_insert(operand);
        });
        first
    }
}

/// Whether `operand`, an output of a slow path in a frame of `layout`, becomes an SSA value: any slot but a
/// constant, which instructions may write too, and which lives in frame memory.
pub(crate) fn is_ssa_slow_path_output(layout: &FrameLayout, operand: Operand) -> bool {
    layout.tracked_index(operand).is_some()
}

/// The operands of a JS call instruction, whose slow path is the runtime's
/// call helper (`SlowPathCall::JitCall`): the operands it reads, in the order
/// the instruction lists them, and its destination.
pub(crate) fn js_call_operands(instruction: Instruction<'_>) -> SlowPathOperands<'_> {
    SlowPathOperands::JsCall(instruction)
}

impl<'a> GraphBuilder<'a, '_> {
    /// The operands of the slow path of the instruction being built.
    pub(super) fn slow_path_operands(&self) -> Result<SlowPathOperands<'a>, CompileFailure> {
        let opcode = self.function.instructions[self.function.instruction_index]
            .instruction
            .opcode();
        let layout = slow_path_layout(opcode);
        let bytecode = &self.snapshot[self.function.executable.bytecode];
        let at = self.function.pc as usize;
        let byte_at = |offset: u32| -> Result<u8, CompileFailure> {
            bytecode
                .get(at + offset as usize)
                .copied()
                .ok_or(CompileFailure::InvalidBytecode {
                    pc: self.function.pc,
                    reason: "operand field outside the bytecode",
                })
        };
        for field in layout.fields {
            byte_at(field.instruction_offset)?;
        }
        let mut array_length = 0;
        if let Some(array) = layout.array {
            array_length = u32::from(byte_at(array.count_offset)?);
            for element in 0..array_length {
                byte_at(array.instruction_offset + element)?;
            }
        }
        Ok(SlowPathOperands::Layout {
            bytecode,
            at,
            layout,
            array_length,
        })
    }

    /// The values of the operands that `operands` reads, with the empty value for absent ones, in a list to give back
    /// with `release_node_list()`.
    pub(super) fn read_slow_path_inputs(
        &mut self,
        operands: SlowPathOperands<'_>,
    ) -> Result<ZoneVec<'a, NodeId>, CompileFailure> {
        let mut inputs = self.node_list();
        let mut result = Ok(());
        operands.for_each_input(|operand| {
            if result.is_err() {
                return;
            }
            match operand {
                Operand::INVALID => inputs.push(self.constant(value::EMPTY)),
                raw => match self.read(Operand::from_raw(raw)) {
                    Ok(value) => inputs.push(value),
                    Err(error) => result = Err(error),
                },
            }
        });
        result?;
        Ok(inputs)
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
            js_call_operands(*instruction)
        } else {
            self.slow_path_operands()?
        };
        let inputs = self.read_slow_path_inputs(operands)?;

        let mut destination = None;
        instruction.for_each_operand(|operand, role| {
            if role.is_written() {
                destination.get_or_insert(operand);
            }
        });

        let mut jumps = false;
        instruction.for_each_jump_target(|_| jumps = true);
        let repr = if jumps {
            Some(Repr::Int32)
        } else if operands
            .first_output()
            .is_some_and(|operand| is_ssa_slow_path_output(&self.function.layout, Operand::from_raw(operand)))
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
            &inputs,
            repr,
        );
        self.release_node_list(inputs);

        // NB: Where the slow path does not continue with the next
        //     instruction, it threw or runs a callee in the interpreter. The
        //     frame then gets the frame state's values, with the old values
        //     of the outputs the slow path did not write, except that it
        //     writes back the outputs it reads (as the interpreter does on
        //     exceptions): the frame state lists those as held by the frame.
        let mut input_outputs = self.slot_list();
        operands.for_each_output(|field, operand| {
            if !info.is_js_call
                && slow_path_layout(opcode).fields[field as usize].role == OperandRole::InOut
                && let Some(slot) = self.function.layout.tracked_index(Operand::from_raw(operand))
            {
                input_outputs.push(slot);
            }
        });
        let frame_state = self.resume_after_frame_state_with_outputs_in_frame(
            destination.map_or(Operand::INVALID, Operand::raw),
            &input_outputs,
        );
        self.release_slot_list(input_outputs);
        self.graph.nodes[node.index()].frame_state = Some(frame_state);

        let mut index = 0;
        let mut result = Ok(());
        operands.for_each_output(|_, operand| {
            let output = index;
            index += 1;
            let operand = Operand::from_raw(operand);
            if result.is_err() || !is_ssa_slow_path_output(&self.function.layout, operand) {
                return;
            }
            let value = if output == 0 {
                node
            } else {
                let output = u8::try_from(output).expect("slow paths have few outputs");
                self.emit(Op::SlowPathOutput { index: output }, &[node], Some(Repr::Tagged))
            };
            result = self.write(operand, value);
        });
        result?;
        self.end_generic_instruction(instruction, node).map(Some)
    }
}
