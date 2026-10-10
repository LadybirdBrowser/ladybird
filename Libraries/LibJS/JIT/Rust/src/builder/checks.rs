/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Checks and conversions built as IR: a tag test on the operand handles
//! the common case inline, and a cold block calls the instruction's slow
//! path for everything else (see `Op::CallSlowPath`).
//!
//! The throwing checks only take their slow path where they throw, so their
//! cold blocks end there, and the code after them knows the check passed.
//! Conversions rejoin with the slow path's result.

use super::Flow;
use super::GenericInfo;
use super::GraphBuilder;
use super::SlotState;
use super::intrinsics::StringCharacter;
use crate::CompileFailure;
use crate::bytecode::Instruction;
use crate::bytecode::OpCode;
use crate::bytecode::Operand;
use crate::bytecode::THIS_VALUE_REGISTER;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Float64UnaryOp;
use crate::ir::FrameStateId;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;

impl GraphBuilder<'_> {
    /// Builds an instruction whose handling is `Handling::Expanded`.
    pub(super) fn build_expanded(
        &mut self,
        instruction: &Instruction,
        info: GenericInfo,
    ) -> Result<Flow, CompileFailure> {
        match *instruction {
            Instruction::GetBinding { .. }
            | Instruction::GetInitializedBinding { .. }
            | Instruction::GetCalleeAndThisFromEnvironment { .. }
            | Instruction::SetLexicalBinding { .. }
            | Instruction::SetVariableBinding { .. }
            | Instruction::InitializeLexicalBinding { .. }
            | Instruction::InitializeVariableBinding { .. }
            | Instruction::CreateVariable { .. } => match self.try_build_binding(instruction)? {
                Some(flow) => Ok(flow),
                None => self.build_generic(instruction, info),
            },
            Instruction::ThrowIfTDZ { src } => {
                let value = self.read(src)?;
                let empty = self.constant(value::EMPTY);
                self.build_throwing_check(BranchCondition::TaggedEquals { equal: true }, vec![value, empty], true)
            }
            Instruction::ThrowIfNotObject { src } => {
                let value = self.read(src)?;
                self.build_throwing_check(BranchCondition::Object, vec![value], false)
            }
            Instruction::ThrowIfNullish { src } => {
                let value = self.read(src)?;
                self.build_throwing_check(BranchCondition::Nullish, vec![value], true)
            }
            Instruction::ToObject { dst, value } => self.build_conversion(BranchCondition::Object, dst, value),
            Instruction::ToLength { dst, value } => {
                self.build_conversion(BranchCondition::NonNegativeInt32, dst, value)
            }
            Instruction::ResolveThisBinding => self.build_resolve_this_binding(),
            Instruction::CallBuiltinMathAbs {
                dst, callee, argument, ..
            } => self.build_math_function(Float64UnaryOp::Abs, dst, callee, argument),
            Instruction::CallBuiltinMathFloor {
                dst, callee, argument, ..
            } => self.build_math_function(Float64UnaryOp::Floor, dst, callee, argument),
            Instruction::CallBuiltinMathCeil {
                dst, callee, argument, ..
            } => self.build_math_function(Float64UnaryOp::Ceil, dst, callee, argument),
            Instruction::CallBuiltinMathRound {
                dst, callee, argument, ..
            } => self.build_math_function(Float64UnaryOp::Round, dst, callee, argument),
            Instruction::CallBuiltinMathSqrt {
                dst, callee, argument, ..
            } => self.build_math_function(Float64UnaryOp::Sqrt, dst, callee, argument),
            Instruction::ObjectPropertyIteratorNext {
                dst_value,
                dst_done,
                receiver,
                keys,
                cursor,
            } => self.build_object_property_iterator_next(dst_value, dst_done, receiver, keys, cursor),
            Instruction::CallBuiltinStringPrototypeCharCodeAt {
                dst,
                callee,
                this_value,
                argument,
                ..
            } => self.build_string_character(StringCharacter::CodeUnit, dst, callee, this_value, argument),
            Instruction::CallBuiltinStringPrototypeCharAt {
                dst,
                callee,
                this_value,
                argument,
                ..
            } => self.build_string_character(StringCharacter::String, dst, callee, this_value, argument),
            Instruction::GetByValue { .. } => self.build_get_by_value(instruction, info),
            Instruction::PutByValue { .. } => self.build_put_by_value(instruction, info),
            Instruction::GetLength { .. } => self.build_get_length(instruction),
            Instruction::GetById { .. } => self.build_get_by_id(instruction),
            Instruction::PutById { .. } => self.build_put_by_id(instruction),
            Instruction::GetGlobal { .. } => self.build_get_global(instruction),
            Instruction::SetGlobal { .. } => self.build_set_global(instruction),
            _ => match super::speculation::Operation::of(instruction) {
                Some(operation) => self.build_operation(instruction, info, operation),
                None => unreachable!("{} is not built expanded", instruction.opcode().name()),
            },
        }
    }

    /// Ends the current block with a branch on `condition` of `inputs` to a
    /// cold block that runs the instruction's slow path, which throws, where
    /// the condition is `throws_when`, and continues in a new block otherwise.
    pub(super) fn build_throwing_check(
        &mut self,
        condition: BranchCondition,
        inputs: Vec<NodeId>,
        throws_when: bool,
    ) -> Result<Flow, CompileFailure> {
        let frame_state = self.eager_frame_state();
        let slow_path_inputs = self.slow_path_inputs()?;
        let (next, cold) = self.branch_with_cold_side(condition, inputs, !throws_when);
        self.block = cold;
        self.emit_call_slow_path(slow_path_inputs, frame_state, None);
        self.set_control(Op::Unreachable, Vec::new());
        self.block = next;
        Ok(Flow::Continue)
    }

    /// Writes `src` to `dst` where `condition` holds of it, and the result
    /// of the instruction's slow path, in a cold block, otherwise.
    fn build_conversion(
        &mut self,
        condition: BranchCondition,
        dst: Operand,
        src: Operand,
    ) -> Result<Flow, CompileFailure> {
        let frame_state = self.eager_frame_state();
        let value = self.read(src)?;
        let slow_path_inputs = self.slow_path_inputs()?;
        let (hot, cold) = self.branch_with_cold_side(condition, vec![value], true);
        self.block = cold;
        let converted = self.emit_call_slow_path(slow_path_inputs, frame_state, Some(Repr::Tagged));
        self.join_blocks(vec![hot, cold]);
        let result = self.add_phi(vec![value, converted]);
        self.write(dst, result)?;
        Ok(Flow::Continue)
    }

    /// `ResolveThisBinding`: the this value register holds the this value
    /// once it is resolved, and the empty value before.
    fn build_resolve_this_binding(&mut self) -> Result<Flow, CompileFailure> {
        let this_register = Operand::from_raw(THIS_VALUE_REGISTER);
        let slot = self
            .function
            .layout
            .tracked_index(this_register)
            .expect("the this value register is tracked");
        let known = match self.frame.slots[slot] {
            SlotState::Value { node, .. } => Some(self.graph.constant_value(node)),
            SlotState::InMemory => None,
        };
        // NB: An inlined callee's `this` is bound by its caller, or empty if
        //     the callee never reads it.
        if self.function.is_virtual() && known.is_some_and(|bits| bits != Some(value::EMPTY)) {
            return Ok(Flow::Continue);
        }
        if known.flatten().is_some_and(|bits| bits != value::EMPTY) {
            return Ok(Flow::Continue);
        }
        let frame_state = self.eager_frame_state();
        let this_value = self.read(this_register)?;
        let in_sync = matches!(self.frame.slots[slot], SlotState::Value { in_sync: true, .. });
        let empty = self.constant(value::EMPTY);
        let (hot, cold) = self.branch_with_cold_side(
            BranchCondition::TaggedEquals { equal: true },
            vec![this_value, empty],
            false,
        );
        self.block = cold;
        // NB: The slow path's value is the this value it resolved.
        let resolved = self.emit_call_slow_path(Vec::new(), frame_state, Some(Repr::Tagged));
        self.join_blocks(vec![hot, cold]);
        let result = self.add_phi(vec![this_value, resolved]);
        self.frame.slots[slot] = SlotState::Value { node: result, in_sync };
        Ok(Flow::Continue)
    }

    /// A new block after `source`, which branches to it.
    pub(super) fn add_block_after(&mut self, source: BlockId) -> BlockId {
        let block = self.start_new_block(vec![source]);
        self.block = source;
        block
    }

    /// A new cold block after `source`, which branches to it.
    pub(super) fn add_cold_block(&mut self, source: BlockId) -> BlockId {
        let block = self.add_block_after(source);
        self.graph.blocks[block.index()].is_cold = true;
        block
    }

    /// Ends `predecessors` with jumps to a new block, in which building
    /// continues. Phis of the new block take their inputs in the order of
    /// `predecessors`.
    pub(super) fn join_blocks(&mut self, predecessors: Vec<BlockId>) -> BlockId {
        let join = self.start_new_block(predecessors.clone());
        for block in predecessors {
            self.block = block;
            self.set_control(Op::Jump { target: join }, Vec::new());
        }
        self.block = join;
        join
    }

    /// The values of the operands the slow path of the instruction being
    /// built reads, in the order of its layout (see
    /// `bytecode::slow_path_layout()`), with the empty value for absent
    /// ones: the inputs of its `CallSlowPath` node. Read before the
    /// instruction branches, so that the values are in the abstract frame
    /// on every path.
    pub(super) fn slow_path_inputs(&mut self) -> Result<Vec<NodeId>, CompileFailure> {
        self.slow_path_operands()?
            .inputs
            .into_iter()
            .map(|operand| match operand {
                Operand::INVALID => Ok(self.constant(value::EMPTY)),
                raw => self.read(Operand::from_raw(raw)),
            })
            .collect()
    }

    fn opcode(&self) -> OpCode {
        self.function.instructions[self.function.instruction_index]
            .instruction
            .opcode()
    }

    /// The `CallSlowPath` node of the instruction being built, with the
    /// instruction's eager frame state.
    pub(super) fn emit_call_slow_path(
        &mut self,
        inputs: Vec<NodeId>,
        frame_state: FrameStateId,
        repr: Option<Repr>,
    ) -> NodeId {
        let node = self.emit(
            Op::CallSlowPath {
                opcode: self.opcode(),
                executable: self.function.index,
                pc: self.function.pc,
                saves_registers: true,
            },
            inputs,
            repr,
        );
        self.graph.nodes[node.index()].frame_state = Some(frame_state);
        node
    }

    /// Starts building the instruction being built as IR that branches to
    /// one cold block running its slow path, which produces a value of
    /// `repr` if the instruction writes one: the frame state and the
    /// operands of the slow path are the instruction's, before any of it
    /// ran.
    pub(super) fn start_slow_paths(&mut self, repr: Option<Repr>) -> Result<SlowPaths, CompileFailure> {
        let frame_state = self.eager_frame_state();
        let inputs = self.slow_path_inputs()?;
        // NB: The value of a conditional jump's slow path is the pc it
        //     jumps to.
        let output_count = if repr.is_some() {
            self.slow_path_operands()?.outputs.len().max(1)
        } else {
            0
        };
        Ok(SlowPaths {
            frame_state,
            inputs,
            repr,
            output_count,
            hot: Vec::new(),
            cold: Vec::new(),
            next_case: Vec::new(),
        })
    }

    /// Ends the current block with a branch on `condition` of `inputs`,
    /// which continues in a new block where the condition is
    /// `continue_when`, and takes a cold edge of `slow_paths` to the
    /// instruction's slow path otherwise. Returns the refinement of the first
    /// input in the new block, for the nodes that rely on the condition.
    pub(super) fn branch_to_slow_path(
        &mut self,
        slow_paths: &mut SlowPaths,
        condition: BranchCondition,
        inputs: Vec<NodeId>,
        continue_when: bool,
    ) -> NodeId {
        let (hot, cold) = self.branch_with_cold_side(condition, inputs.clone(), continue_when);
        slow_paths.cold.push(cold);
        self.block = hot;
        self.refine(condition, continue_when, inputs)
    }

    /// Like `branch_to_slow_path()`, where the values for which `condition`
    /// is not `continue_when` go on to the next case (see
    /// `start_next_case()`) instead, or to the slow path if there is none.
    pub(super) fn branch_to_next_case(
        &mut self,
        slow_paths: &mut SlowPaths,
        condition: BranchCondition,
        inputs: Vec<NodeId>,
        continue_when: bool,
    ) -> NodeId {
        let (if_true, if_false) = self.branch_both_ways(condition, inputs.clone());
        let (hot, edge) = if continue_when {
            (if_true, if_false)
        } else {
            (if_false, if_true)
        };
        slow_paths.next_case.push(edge);
        self.block = hot;
        self.refine(condition, continue_when, inputs)
    }

    /// Starts a block where the values that `branch_to_next_case()` sent on
    /// continue, in which building continues. Returns false, and starts
    /// nothing, if none did.
    pub(super) fn start_next_case(&mut self, slow_paths: &mut SlowPaths) -> bool {
        let edges = std::mem::take(&mut slow_paths.next_case);
        match edges.as_slice() {
            [] => return false,
            // NB: A single edge is the case's block itself.
            [edge] => {
                self.block = *edge;
                return true;
            }
            _ => {}
        }
        self.join_blocks(edges);
        true
    }

    /// The refinement of the first of `inputs`, at the head of the current
    /// block, which only the edge of a branch on `condition` of `inputs`
    /// where it is `holds` reaches (see `Op::Refine`).
    pub(super) fn refine(&mut self, condition: BranchCondition, holds: bool, inputs: Vec<NodeId>) -> NodeId {
        let repr = self.graph.node(inputs[0]).repr;
        self.emit(Op::Refine { condition, holds }, inputs, repr)
    }

    /// Ends the current block as one of several paths that completed the
    /// instruction, with `values` like `join_paths()`
    /// takes them, to be joined with the others there. Building continues
    /// in another block.
    pub(super) fn end_hot_path(&mut self, slow_paths: &mut SlowPaths, values: Vec<NodeId>) {
        slow_paths.hot.push((self.block, values));
    }

    /// Ends the current block with a branch on `condition` of `inputs` to a
    /// new block where it is `continue_when` and a new cold block where it is
    /// not, and returns them.
    pub(super) fn branch_with_cold_side(
        &mut self,
        condition: BranchCondition,
        inputs: Vec<NodeId>,
        continue_when: bool,
    ) -> (BlockId, BlockId) {
        let source = self.block;
        let hot = self.add_block_after(source);
        let cold = self.add_cold_block(source);
        let (if_true, if_false) = if continue_when { (hot, cold) } else { (cold, hot) };
        self.set_control(
            Op::Branch {
                condition,
                if_true,
                if_false,
            },
            inputs,
        );
        (hot, cold)
    }

    /// Ends the current block with a branch on `condition` of `inputs` to
    /// two new blocks, and returns them: where it holds, and where not.
    pub(super) fn branch_both_ways(&mut self, condition: BranchCondition, inputs: Vec<NodeId>) -> (BlockId, BlockId) {
        let source = self.block;
        let if_true = self.add_block_after(source);
        let if_false = self.add_block_after(source);
        self.set_control(
            Op::Branch {
                condition,
                if_true,
                if_false,
            },
            inputs,
        );
        (if_true, if_false)
    }

    /// Branches on `conditions` of `value`, one after the other, and builds
    /// what applies where each holds with `build` (with the index of the
    /// condition, and the value refined by it) in a block of its own. Values
    /// for which none holds continue in the current block if `others`, and
    /// take a slow path otherwise.
    pub(super) fn branch_on_cases(
        &mut self,
        slow_paths: &mut SlowPaths,
        value: NodeId,
        conditions: &[BranchCondition],
        others: bool,
        mut build: impl FnMut(&mut Self, &mut SlowPaths, usize, NodeId),
    ) {
        for (index, condition) in conditions.iter().enumerate() {
            if index + 1 == conditions.len() && !others {
                let refined = self.branch_to_slow_path(slow_paths, *condition, vec![value], true);
                build(self, slow_paths, index, refined);
                return;
            }
            let (matched, other) = self.branch_both_ways(*condition, vec![value]);
            self.block = matched;
            let refined = self.refine(*condition, true, vec![value]);
            build(self, slow_paths, index, refined);
            self.block = other;
        }
    }

    /// `value`, refined to not be the empty value, exiting with `kind` in a
    /// cold block where it is.
    pub(super) fn exit_if_empty(&mut self, value: NodeId, kind: ExitKind) -> NodeId {
        let empty = self.constant(value::EMPTY);
        let condition = BranchCondition::TaggedEquals { equal: true };
        let frame_state = self.eager_frame_state();
        let (hot, cold) = self.branch_with_cold_side(condition, vec![value, empty], false);
        self.block = cold;
        let exit = self.set_control(Op::Exit { kind }, Vec::new());
        self.graph.nodes[exit.index()].frame_state = Some(frame_state);
        self.block = hot;
        self.refine(condition, false, vec![value, empty])
    }

    /// `value`, refined to not be the empty value (a hole, or nothing found
    /// by a cache probe), taking a slow path where it is.
    pub(super) fn branch_if_empty(&mut self, slow_paths: &mut SlowPaths, value: NodeId) -> NodeId {
        let empty = self.constant(value::EMPTY);
        self.branch_to_slow_path(
            slow_paths,
            BranchCondition::TaggedEquals { equal: true },
            vec![value, empty],
            false,
        )
    }

    /// Joins the paths that `end_hot_path()` ended with the slow paths, and
    /// returns the instruction's value, if it has one.
    pub(super) fn join_hot_paths(&mut self, slow_paths: SlowPaths) -> Option<NodeId> {
        self.join_hot_paths_with(slow_paths, |_, outputs| outputs)
            .into_iter()
            .next()
    }

    /// Like `join_hot_paths()`, for instructions that write several
    /// operands, where `values` makes their values of the outputs of the
    /// slow path, in the slow path's block.
    pub(super) fn join_hot_paths_with(
        &mut self,
        mut slow_paths: SlowPaths,
        values: impl FnOnce(&mut Self, Vec<NodeId>) -> Vec<NodeId>,
    ) -> Vec<NodeId> {
        let (block, hot_values) = slow_paths.take_last_hot_path();
        self.block = block;
        self.join_paths(slow_paths, hot_values, values)
    }

    /// Ends the current block, the blocks of `end_hot_path()` and the cold
    /// blocks of `slow_paths` with jumps to a new block, in which building
    /// continues. Returns the
    /// instruction's value there: the phi of `value`, its value in the
    /// current block, and the slow paths' values.
    pub(super) fn join_slow_paths(&mut self, slow_paths: SlowPaths, value: Option<NodeId>) -> Option<NodeId> {
        self.join_paths(slow_paths, value.into_iter().collect(), |_, outputs| outputs)
            .into_iter()
            .next()
    }

    /// Like `join_slow_paths()`, for an instruction that writes several
    /// operands: `values` are their values in the current block, in the
    /// order of the slow path's outputs, `slow_values` makes them of the
    /// outputs of the slow path, in its block, and this returns their phis.
    pub(super) fn join_paths(
        &mut self,
        mut slow_paths: SlowPaths,
        values: Vec<NodeId>,
        slow_values: impl FnOnce(&mut Self, Vec<NodeId>) -> Vec<NodeId>,
    ) -> Vec<NodeId> {
        let mut predecessors = slow_paths.hot.iter().map(|(block, _)| *block).collect::<Vec<_>>();
        predecessors.push(self.block);
        // NB: Values that no case took take the slow path.
        for edge in std::mem::take(&mut slow_paths.next_case) {
            self.graph.blocks[edge.index()].is_cold = true;
            slow_paths.cold.push(edge);
        }
        let slow = self.build_slow_path(&slow_paths).map(|(_, outputs)| {
            let values = slow_values(self, outputs);
            (self.block, values)
        });
        predecessors.extend(slow.as_ref().map(|(block, _)| *block));
        self.join_blocks(predecessors);
        debug_assert!(
            values.is_empty() || values.len() == slow_paths.output_count,
            "the slow path writes the same operands"
        );
        values
            .into_iter()
            .enumerate()
            .map(|(index, value)| {
                let mut inputs = slow_paths
                    .hot
                    .iter()
                    .map(|(_, values)| values[index])
                    .collect::<Vec<_>>();
                inputs.push(value);
                inputs.extend(slow.as_ref().map(|(_, outputs)| outputs[index]));
                if inputs.iter().all(|input| *input == inputs[0]) {
                    return inputs[0];
                }
                let repr = self.graph.node(value).repr;
                let phi = self.add_phi(inputs);
                self.graph.nodes[phi.index()].repr = repr;
                phi
            })
            .collect()
    }

    /// The block running the slow path of `slow_paths`, which every cold
    /// edge they took jumps to, with the operands the slow path wrote, if
    /// any edge was taken.
    fn build_slow_path(&mut self, slow_paths: &SlowPaths) -> Option<(BlockId, Vec<NodeId>)> {
        let slow = match slow_paths.cold.as_slice() {
            [] => return None,
            [edge] => *edge,
            edges => {
                let slow = self.join_blocks(edges.to_vec());
                self.graph.blocks[slow.index()].is_cold = true;
                slow
            }
        };
        self.block = slow;
        let call = self.emit_call_slow_path(slow_paths.inputs.clone(), slow_paths.frame_state, slow_paths.repr);
        let mut outputs = vec![call];
        for index in 1..slow_paths.output_count {
            let index = u8::try_from(index).expect("slow paths have few outputs");
            outputs.push(self.emit(Op::SlowPathOutput { index }, vec![call], Some(Repr::Tagged)));
        }
        Some((slow, outputs))
    }
}

/// The paths of an instruction built as IR so far: the cold edges to its
/// slow path, the other paths that completed it, and what the slow path
/// takes (see `GraphBuilder::start_slow_paths()`).
pub(in crate::builder) struct SlowPaths {
    frame_state: FrameStateId,
    inputs: Vec<NodeId>,
    repr: Option<Repr>,
    /// How many operands the slow path writes.
    output_count: usize,
    /// The other blocks that completed the instruction (see
    /// `GraphBuilder::end_hot_path()`), with their values.
    hot: Vec<(BlockId, Vec<NodeId>)>,
    /// The cold edges taken so far, which all jump to one block running the
    /// slow path (see `GraphBuilder::build_slow_path()`).
    cold: Vec<BlockId>,
    /// The edges that `GraphBuilder::branch_to_next_case()` took since the
    /// last case started.
    next_case: Vec<BlockId>,
}

impl SlowPaths {
    /// The last path that `GraphBuilder::end_hot_path()` ended, which no
    /// longer is one of them.
    pub(in crate::builder) fn take_last_hot_path(&mut self) -> (BlockId, Vec<NodeId>) {
        self.hot.pop().expect("a path completed the instruction")
    }
}
