/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lower target-independent SSA into LowIR.

use super::{
    AddressDisplacement, AddressRegister, AddressScale, Handler, Instruction, Label, MemoryAddress, Operand,
    Relocation, VirtualRegister, emit_instructions as emit,
};
use crate::bytecode::{BytecodeFieldId, HandlerLayout as BytecodeHandlerLayout};
use crate::frontend::layout::{KnownLayoutConstant, LayoutConstantCategory, LayoutConstants};
use crate::hash::{HashMap, HashSet};
use crate::intrinsic::{
    AddressOperation, AggregateOperation, AssertionOperation, BranchOperation, BytecodeOperation, CallOperation,
    CheckedIntegerOperation, ClassificationOperation, ComparisonDomain, ComparisonRelation, ControlOperation,
    FieldWidth, FloatConversion, FloatingPointOperation, IntegerBinaryOperation, IntegerComparisonOperation,
    IntegerSignedness, Intrinsic, LowLevelOperation, MemoryOperation, OperandLoad, OperandOperation, OperandStore,
    ValueOperation,
};
use crate::ssa::{
    BlockId, BlockLayout, Constant, Edge, Function, InstructionId, Operation, Terminator, ValueDefinition, ValueId,
};
use crate::target::description::{
    BinaryOperation, EqualityCondition, IntegerWidth, MemoryWidth, Operation as MachineOperation, OrderedCondition,
    OverflowOperation, PairWidth, SignCondition, TestCondition, ZeroCondition,
};
use crate::types::{InterpreterRegister, RegisterClass, Type};
use crate::{CompileError, CompileStage};
use std::ops::Range;

pub(crate) fn lower_handler_with_constants(
    function: &Function,
    is_cold: bool,
    constants: &LayoutConstants,
    id: crate::identity::HandlerId,
) -> Result<Handler, CompileError> {
    let function = &FunctionUses::new(function, id);
    let mut handler = lower_handler_internal(function, is_cold, constants, id)
        .map_err(|message| CompileError::new(CompileStage::LowIr, Some(&function.name), message))?;
    materialize_layout_constants(&mut handler);
    Ok(handler)
}

pub(crate) fn materialize_bytecode_layout(
    handler: &mut Handler,
    layout: &BytecodeHandlerLayout,
) -> Result<(), CompileError> {
    if handler.size.is_none() {
        handler.size = layout
            .size
            .map(|size| {
                size.try_into().map_err(|_| {
                    CompileError::new(
                        CompileStage::LowIr,
                        Some(&handler.name),
                        format!("bytecode instruction size {size} does not fit in u32"),
                    )
                })
            })
            .transpose()?;
    }

    for instruction in &mut handler.instructions {
        for operand in &mut instruction.operands {
            materialize_bytecode_field(operand, layout, &handler.name)?;
        }
    }
    Ok(())
}

fn materialize_bytecode_field(
    operand: &mut Operand,
    layout: &BytecodeHandlerLayout,
    handler: &str,
) -> Result<(), CompileError> {
    let resolve = |field: BytecodeFieldId| {
        layout
            .field_offset(field)
            .ok_or_else(|| {
                CompileError::new(
                    CompileStage::LowIr,
                    Some(handler),
                    format!("unknown bytecode field '{}'", layout.field_name(field)),
                )
            })?
            .try_into()
            .map_err(|_| {
                CompileError::new(
                    CompileStage::LowIr,
                    Some(handler),
                    format!(
                        "bytecode field '{}' offset does not fit in i64",
                        layout.field_name(field)
                    ),
                )
            })
    };

    match operand {
        Operand::BytecodeField(field) => {
            *operand = Operand::Immediate(resolve(*field)?);
        }
        Operand::Address(address) => {
            if let Some(AddressDisplacement::BytecodeField(field)) = &address.displacement {
                address.displacement = Some(AddressDisplacement::Immediate(resolve(*field)?));
            }
        }
        _ => {}
    }
    Ok(())
}

pub(crate) fn materialize_layout_constants(handler: &mut Handler) {
    for instruction in &mut handler.instructions {
        for operand in &mut instruction.operands {
            resolve_layout_constant(operand);
        }
    }
}

fn resolve_layout_constant(operand: &mut Operand) {
    match operand {
        Operand::LayoutConstant(constant) => {
            *operand = if constant.category() == LayoutConstantCategory::Value {
                Operand::ValueConstant(constant.value())
            } else {
                Operand::Immediate(constant.value())
            };
        }
        Operand::Address(address) => {
            if let Some(AddressScale::LayoutConstant(constant)) = address.scale {
                address.scale = Some(AddressScale::Immediate(constant.value()));
            }
            if let Some(AddressDisplacement::LayoutConstant(constant)) = &address.displacement {
                address.displacement = Some(AddressDisplacement::Immediate(constant.value()));
            }
        }
        _ => {}
    }
}

fn lower_handler_internal(
    function: &FunctionUses<'_>,
    is_cold: bool,
    constants: &LayoutConstants,
    id: crate::identity::HandlerId,
) -> Result<Handler, String> {
    let mut folded_instructions = FoldedInstructions::new(function.instructions.len());
    for block in &function.blocks {
        let Some(Terminator::Branch { condition, .. }) = &block.terminator else {
            continue;
        };
        if let Some(branch) = selected_branch(function, *condition) {
            folded_instructions.insert(branch.instruction);
            folded_instructions.extend(branch.folded_inputs);
        } else if let Some((instruction, _, _)) = selected_int32_pair(function, *condition) {
            folded_instructions.insert(instruction);
        }
    }
    for instruction in &function.instructions {
        if instruction.operation.guard_failure().is_some() {
            let condition = instruction.inputs[0];
            if let Some(branch) = selected_branch(function, condition) {
                folded_instructions.insert(branch.instruction);
                folded_instructions.extend(branch.folded_inputs);
            } else if let Some((instruction, _, _)) = selected_int32_pair(function, condition) {
                folded_instructions.insert(instruction);
            }
        }
        if let Some((instruction, _)) = folded_bitwise_not_source(function, instruction) {
            folded_instructions.insert(instruction);
        }
        if let Some((instruction, _)) = folded_u32_unbox_source(function, instruction) {
            folded_instructions.insert(instruction);
        }
        if let Some((instruction, _)) = folded_i32_bool_source(function, instruction) {
            folded_instructions.insert(instruction);
        }
        if instruction.operation == Operation::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::DivideModulo)) {
            for input in &instruction.inputs {
                if let Some((instructions, _)) = folded_32bit_operation_source(function, *input) {
                    folded_instructions.extend(instructions);
                }
            }
        }
        if matches!(instruction.operation, Operation::Intrinsic(Intrinsic::IntegerBinary(operation)) if operation.supports_narrow_result())
            && instruction
                .results
                .first()
                .is_some_and(|result| integer_result_can_stay_narrow(function, *result))
        {
            for input in &instruction.inputs {
                if let Some((instructions, _)) = folded_32bit_operation_source(function, *input) {
                    folded_instructions.extend(instructions);
                }
            }
            if matches!(instruction.operation, Operation::Intrinsic(Intrinsic::IntegerBinary(operation)) if operation.is_shift())
                && let Some((instructions, source)) = folded_shift_count(function, instruction.inputs[1])
            {
                folded_instructions.extend(instructions);
                if let Some((instructions, _)) = folded_32bit_operation_source(function, source) {
                    folded_instructions.extend(instructions);
                }
            }
        }
    }
    for (block_index, block) in function.blocks.iter().enumerate() {
        let block_id = BlockId(block_index);
        let Some(Terminator::CheckedOperation { operation, inputs, .. }) = &block.terminator else {
            continue;
        };
        if matches!(
            operation,
            Operation::Intrinsic(Intrinsic::CheckedInteger(
                CheckedIntegerOperation::Add
                    | CheckedIntegerOperation::Subtract
                    | CheckedIntegerOperation::Multiply
                    | CheckedIntegerOperation::Negate
            ))
        ) {
            folded_instructions.extend(
                inputs.iter().filter_map(|input| {
                    folded_checked_i32_source(function, *input).map(|(instruction, _)| instruction)
                }),
            );
            let is_binary = matches!(
                operation,
                Operation::Intrinsic(Intrinsic::CheckedInteger(
                    CheckedIntegerOperation::Add
                        | CheckedIntegerOperation::Subtract
                        | CheckedIntegerOperation::Multiply
                ))
            );
            if is_binary {
                let rhs_is_bytecode_field = matches!(
                    operation,
                    Operation::Intrinsic(Intrinsic::CheckedInteger(
                        CheckedIntegerOperation::Add | CheckedIntegerOperation::Subtract
                    ))
                )
                .then(|| folded_checked_i32_bytecode_field(function, inputs[1]))
                .flatten();
                let lhs_unbox = recoverable_checked_i32_unbox_source(function, inputs[0], block_id);
                let rhs_unbox = recoverable_checked_i32_unbox_source(function, inputs[1], block_id);
                if let Some((instruction, _)) = rhs_is_bytecode_field {
                    folded_instructions.insert(instruction);
                }
                if lhs_unbox.is_some() && (rhs_is_bytecode_field.is_some() || rhs_unbox.is_some()) {
                    folded_instructions.extend(
                        lhs_unbox
                            .into_iter()
                            .chain(rhs_unbox)
                            .map(|(instruction, _)| instruction),
                    );
                }
            }
        } else if let Operation::Intrinsic(Intrinsic::Branch(operation)) = operation
            && typed_narrow_signed_branch_operation(function, *operation, inputs).is_some()
        {
            for input in inputs {
                if let Some((instructions, _)) = folded_32bit_operation_source(function, *input) {
                    folded_instructions.extend(instructions);
                }
            }
        }
    }
    folded_instructions.extend(
        function
            .instructions
            .iter()
            .enumerate()
            .filter_map(|(index, instruction)| {
                (instruction.operation == Operation::Intrinsic(Intrinsic::Value(ValueOperation::Reuse)))
                    .then_some(InstructionId(index))
            }),
    );
    let preferred_order = schedule_blocks(function);
    let alternate_order = schedule_blocks_with_successor_order(function, false);
    let profile_order = schedule_blocks_by_profile(function);
    let mut candidate_orders = vec![preferred_order.clone()];
    if alternate_order != preferred_order {
        candidate_orders.push(alternate_order);
    }
    if !candidate_orders.contains(&profile_order) {
        candidate_orders.push(profile_order);
    }
    let lowered = lower_blocks(function, &folded_instructions, constants, preferred_order.clone())?;
    let lowered_blocks = LoweredBlockFragments::new(&lowered, &preferred_order)?;
    let mut selected = None;
    for order in candidate_orders {
        let instructions = lowered_blocks.layout_instructions(&lowered, &order);
        let mut graph = super::cfg::ControlFlowGraph::from_instructions(instructions)?;
        graph.simplify();
        let cost = graph.linearized_instruction_count()?;
        let profile_score = profile_layout_score(function, &order);
        if selected.as_ref().is_none_or(|(selected_profile, selected_cost, _)| {
            profile_score > *selected_profile || profile_score == *selected_profile && cost < *selected_cost
        }) {
            selected = Some((profile_score, cost, order));
        }
    }
    let selected_order = selected.expect("there is always a preferred block order").2;
    let mut instructions = if selected_order == preferred_order {
        lowered
    } else {
        lowered_blocks.reorder(&lowered, &selected_order)
    };
    // Preserve the semantic exec_ctx value for targets that derive it from a
    // different pinned base. The dedicated instruction also exposes any
    // scratch register needed for that derivation to register allocation.
    for instruction in &mut instructions {
        let stores_execution_context = match instruction.operands.get(1) {
            Some(Operand::InterpreterRegister(register)) => *register == InterpreterRegister::ExecutionContext,
            _ => false,
        };
        if instruction.opcode.operation() == MachineOperation::store(MemoryWidth::DoubleWord)
            && stores_execution_context
        {
            instruction.opcode = MachineOperation::StoreExecutionContext;
            instruction.operands.pop();
        }
    }
    Ok(Handler {
        id,
        name: function.name.clone(),
        size: None,
        is_cold,
        instructions,
    })
}

fn profile_layout_score(function: &Function, blocks: &[BlockId]) -> usize {
    blocks
        .windows(2)
        .filter(|pair| function.blocks[pair[1].0].layout == BlockLayout::Preferred)
        .filter(|pair| {
            function.blocks[pair[0].0]
                .terminator
                .as_ref()
                .is_some_and(|terminator| terminator.successors().any(|successor| successor.block == pair[1]))
        })
        .count()
}

/// The instructions a later one has absorbed, so lowering must not emit them.
struct FoldedInstructions(Vec<bool>);

impl FoldedInstructions {
    fn new(instructions: usize) -> Self {
        Self(vec![false; instructions])
    }

    fn insert(&mut self, instruction: InstructionId) {
        self.0[instruction.0] = true;
    }

    fn extend(&mut self, instructions: impl IntoIterator<Item = InstructionId>) {
        for instruction in instructions {
            self.insert(instruction);
        }
    }

    fn contains(&self, instruction: InstructionId) -> bool {
        self.0[instruction.0]
    }
}

fn lower_blocks(
    function: &FunctionUses<'_>,
    folded_instructions: &FoldedInstructions,
    constants: &LayoutConstants,
    block_order: Vec<BlockId>,
) -> Result<Vec<Instruction>, String> {
    let mut body = Vec::new();
    let mut edge_temporary = 0usize;
    let mut deferred_switch_tails = HashMap::<BlockId, Vec<Instruction>>::default();
    let entry_is_branch_target = (0..function.blocks.len()).any(|index| {
        function.blocks[index]
            .terminator
            .as_ref()
            .is_some_and(|terminator| terminator.successors().any(|edge| edge.block == function.entry))
            || function
                .guard_exits(BlockId(index))
                .any(|(_, failure)| failure == function.entry)
    });
    for (position, &block_id) in block_order.iter().enumerate() {
        let block = &function.blocks[block_id.0];
        if block_id != function.entry || entry_is_branch_target {
            let label = block_label(block_id);
            if block.layout == BlockLayout::Cold {
                emit!(body; MachineOperation::Cold => [Operand::Label(label.clone())];);
            }
            emit!(body; MachineOperation::Label => [Operand::Label(label)];);
        }
        let block_body_start = body.len();
        let instructions = block
            .instructions
            .iter()
            .copied()
            .filter(|instruction| !folded_instructions.contains(*instruction))
            .collect::<Vec<_>>();
        let mut instruction_index = 0;
        while instruction_index < instructions.len() {
            let instruction_id = instructions[instruction_index];
            let next_instruction = instructions[instruction_index + 1..]
                .iter()
                .position(|instruction| {
                    !matches!(
                        function.instructions[instruction.0].operation,
                        Operation::Address | Operation::BlockReference(_)
                    )
                })
                .map(|offset| instruction_index + 1 + offset);
            if let Some(failure) = function.instructions[instruction_id.0].operation.guard_failure() {
                let name = format!("guard_{}", instruction_id.0);
                let continue_label = Label::new(format!(".ssa_{name}_pass"));
                emit_conditional_branch(
                    function,
                    constants,
                    &name,
                    block_body_start,
                    function.instructions[instruction_id.0].inputs[0],
                    &continue_label,
                    &mut body,
                )?;
                emit!(body; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(failure))];);
                emit!(body; MachineOperation::Label => [Operand::Label(continue_label)];);
                instruction_index += 1;
                continue;
            }
            if let Some(next_index) = next_instruction
                && lower_field_access_pair(function, instruction_id, instructions[next_index], &mut body)?
            {
                instruction_index = next_index + 1;
                continue;
            }
            lower_instruction(
                function,
                instruction_id,
                &function.instructions[instruction_id.0],
                &mut body,
            )?;
            instruction_index += 1;
        }
        match block
            .terminator
            .as_ref()
            .ok_or_else(|| format!("block {block_id:?} has no terminator"))?
        {
            Terminator::Jump(edge) => {
                lower_edge_copies(function, edge, &mut body, &mut edge_temporary)?;
                emit!(body; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(edge.block))];);
            }
            Terminator::Branch {
                condition,
                then_edge,
                else_edge,
            } => {
                let then_label = Label::new(format!(".ssa_edge_{}_then", block_id.0));
                emit_conditional_branch(
                    function,
                    constants,
                    &format!("{}", block_id.0),
                    block_body_start,
                    *condition,
                    &then_label,
                    &mut body,
                )?;
                lower_edge_copies(function, else_edge, &mut body, &mut edge_temporary)?;
                emit!(body; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(else_edge.block))];);
                emit!(body; MachineOperation::Label => [Operand::Label(then_label)];);
                lower_edge_copies(function, then_edge, &mut body, &mut edge_temporary)?;
                emit!(body; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(then_edge.block))];);
            }
            Terminator::Switch { value, cases, default } => {
                let preferred_tail_after = preferred_switch_tail_after(&block_order, position, cases, default);
                if let Some((target, tail)) = lower_switch(
                    function,
                    block_id,
                    block_order.get(position + 1).copied(),
                    preferred_tail_after,
                    *value,
                    cases,
                    default,
                    &mut body,
                    &mut edge_temporary,
                )? {
                    deferred_switch_tails.entry(target).or_default().extend(tail);
                }
            }
            Terminator::CheckedOperation {
                operation,
                inputs,
                results,
                success,
                failure,
                ..
            } => {
                lower_checked_operation(
                    function,
                    block_id,
                    operation,
                    inputs,
                    results,
                    success,
                    failure,
                    &mut body,
                    &mut edge_temporary,
                )?;
            }
            Terminator::Return(values) if values.is_empty() => {}
            Terminator::Unreachable => {}
            terminator => {
                return Err(format!(
                    "direct machine lowering for '{}' does not support {terminator:?} yet",
                    function.name
                ));
            }
        }
        if let Some(tail) = deferred_switch_tails.remove(&block_id) {
            body.extend(tail);
        }
        if block.layout == BlockLayout::Cold {
            let internal_labels = body[block_body_start..]
                .iter()
                .filter_map(|instruction| {
                    if instruction.opcode.operation() != (MachineOperation::Label) {
                        return None;
                    }
                    let [Operand::Label(label)] = instruction.operands.as_slice() else {
                        return None;
                    };
                    Some(label.clone())
                })
                .collect::<Vec<_>>();
            body.extend(
                internal_labels
                    .into_iter()
                    .map(|label| machine_instruction(MachineOperation::Cold, vec![Operand::Label(label)])),
            );
        }
    }
    if !deferred_switch_tails.is_empty() {
        return Err("preferred switch tail was not placed in the scheduled block order".to_string());
    }
    super::optimize::propagate_single_assignment_copies(&mut body);
    super::optimize::fuse_scaled_addresses(&mut body);
    super::optimize::select_copy_add_immediates(&mut body);
    super::optimize::fuse_indexed_offset_stores(&mut body);
    super::optimize::fuse_adjacent_sequence_stores(&mut body);
    super::optimize::select_zero_moves(&mut body);
    super::optimize::fuse_or32_sign_branches(&mut body);
    Ok(body)
}

struct LoweredBlockFragments {
    ranges: Vec<Range<usize>>,
}

impl LoweredBlockFragments {
    fn new(instructions: &[Instruction], original_order: &[BlockId]) -> Result<Self, String> {
        let labels = original_order
            .iter()
            .map(|block| (block_label(*block), *block))
            .collect::<HashMap<_, _>>();
        let mut starts = Vec::with_capacity(original_order.len());
        for (index, instruction) in instructions.iter().enumerate() {
            if instruction.opcode.operation() != MachineOperation::Label {
                continue;
            }
            let [Operand::Label(label)] = instruction.operands.as_slice() else {
                continue;
            };
            let Some(block) = labels.get(label).copied() else {
                continue;
            };
            let start = if index > 0
                && instructions[index - 1].opcode.operation() == MachineOperation::Cold
                && instructions[index - 1].operands == instruction.operands
            {
                index - 1
            } else {
                index
            };
            starts.push((block, start));
        }
        if starts.first().is_none_or(|(_, start)| *start != 0) {
            starts.insert(0, (original_order[0], 0));
        }
        let mut ranges = vec![0..0; original_order.len()];
        let mut found = vec![false; original_order.len()];
        for (position, (block, start)) in starts.iter().enumerate() {
            let end = starts.get(position + 1).map_or(instructions.len(), |(_, start)| *start);
            if block.0 >= ranges.len() || std::mem::replace(&mut found[block.0], true) {
                return Err("lowered block fragments do not cover the scheduled SSA blocks".to_string());
            }
            ranges[block.0] = *start..end;
        }
        if found.contains(&false) {
            return Err("lowered block fragments do not cover the scheduled SSA blocks".to_string());
        }
        Ok(Self { ranges })
    }

    fn reorder(&self, instructions: &[Instruction], new_order: &[BlockId]) -> Vec<Instruction> {
        let mut reordered = Vec::with_capacity(instructions.len());
        for block in new_order {
            reordered.extend_from_slice(&instructions[self.ranges[block.0].clone()]);
        }
        reordered
    }

    fn layout_instructions(
        &self,
        instructions: &[Instruction],
        new_order: &[BlockId],
    ) -> Vec<Instruction<LayoutOperand>> {
        let mut reordered = Vec::with_capacity(instructions.len());
        for block in new_order {
            reordered.extend(instructions[self.ranges[block.0].clone()].iter().map(|instruction| {
                Instruction {
                    opcode: instruction.opcode,
                    operands: instruction
                        .operands
                        .iter()
                        .map(|operand| match operand {
                            Operand::Label(label) => LayoutOperand::Label(label.clone()),
                            _ => LayoutOperand::Other,
                        })
                        .collect(),
                }
            }));
        }
        reordered
    }
}

#[derive(Clone, Debug)]
enum LayoutOperand {
    Label(Label),
    Other,
}

impl super::ControlFlowOperand for LayoutOperand {
    fn label(&self) -> Option<&Label> {
        match self {
            Self::Label(label) => Some(label),
            Self::Other => None,
        }
    }

    fn label_mut(&mut self) -> Option<&mut Label> {
        match self {
            Self::Label(label) => Some(label),
            Self::Other => None,
        }
    }

    fn from_label(label: Label) -> Self {
        Self::Label(label)
    }
}

fn schedule_blocks(function: &Function) -> Vec<BlockId> {
    schedule_blocks_with_successor_order(function, true)
}

fn schedule_blocks_with_successor_order(function: &Function, reverse_successors: bool) -> Vec<BlockId> {
    // Walk the graph with an explicit stack to handle deeply nested control
    // flow without consuming the call stack.
    let mut visited = vec![false; function.blocks.len()];
    let mut reverse_postorder = Vec::with_capacity(function.blocks.len());
    let mut stack = vec![(function.entry, 0usize)];
    visited[function.entry.0] = true;
    while let Some((block, next)) = stack.last_mut() {
        let block = *block;
        let successors = function.blocks[block.0]
            .terminator
            .as_ref()
            .map_or(0, Terminator::successor_count);
        if *next == successors {
            reverse_postorder.push(block);
            stack.pop();
            continue;
        }
        let index = if reverse_successors {
            successors - 1 - *next
        } else {
            *next
        };
        *next += 1;
        let successor = function.blocks[block.0]
            .terminator
            .as_ref()
            .expect("a block with successors has a terminator")
            .successor(index)
            .block;
        if !std::mem::replace(&mut visited[successor.0], true) {
            stack.push((successor, 0));
        }
    }
    reverse_postorder.reverse();
    for (index, was_visited) in visited.iter_mut().enumerate() {
        if !std::mem::replace(was_visited, true) {
            reverse_postorder.push(BlockId(index));
        }
    }
    let mut scheduled = reverse_postorder
        .iter()
        .copied()
        .filter(|block| function.blocks[block.0].layout != BlockLayout::Cold)
        .collect::<Vec<_>>();
    scheduled.extend(
        reverse_postorder
            .into_iter()
            .filter(|block| function.blocks[block.0].layout == BlockLayout::Cold),
    );
    scheduled
}

fn schedule_blocks_by_profile(function: &Function) -> Vec<BlockId> {
    let base_order = schedule_blocks(function);
    let ranks = base_order
        .iter()
        .enumerate()
        .map(|(rank, block)| (*block, rank))
        .collect::<HashMap<_, _>>();
    let mut chains = (0..function.blocks.len())
        .map(|index| vec![BlockId(index)])
        .collect::<Vec<_>>();
    let mut owners = (0..function.blocks.len()).collect::<Vec<_>>();
    let mut edges = function
        .blocks
        .iter()
        .enumerate()
        .flat_map(|(source, block)| {
            block
                .terminator
                .as_ref()
                .into_iter()
                .flat_map(Terminator::successors)
                .enumerate()
                .map(move |(successor_index, edge)| {
                    let target_layout = function.blocks[edge.block.0].layout;
                    let profile_weight = match target_layout {
                        BlockLayout::Preferred => 2,
                        BlockLayout::Hot => 1,
                        BlockLayout::Cold => 0,
                    };
                    (profile_weight, source, edge.block.0, successor_index)
                })
        })
        .collect::<Vec<_>>();
    edges.sort_by(|lhs, rhs| {
        rhs.0
            .cmp(&lhs.0)
            .then_with(|| ranks[&BlockId(lhs.1)].cmp(&ranks[&BlockId(rhs.1)]))
            .then_with(|| lhs.3.cmp(&rhs.3))
            .then_with(|| lhs.2.cmp(&rhs.2))
    });

    for (_, source, target, _) in edges {
        let source = BlockId(source);
        let target = BlockId(target);
        if target == function.entry
            || (function.blocks[source.0].layout == BlockLayout::Cold)
                != (function.blocks[target.0].layout == BlockLayout::Cold)
        {
            continue;
        }
        let source_chain = owners[source.0];
        let target_chain = owners[target.0];
        if source_chain == target_chain
            || chains[source_chain].last() != Some(&source)
            || chains[target_chain].first() != Some(&target)
        {
            continue;
        }
        let appended = std::mem::take(&mut chains[target_chain]);
        chains[source_chain].extend(&appended);
        for block in appended {
            owners[block.0] = source_chain;
        }
    }

    let entry_chain = owners[function.entry.0];
    let mut chain_order = chains
        .iter()
        .enumerate()
        .filter(|(_, chain)| !chain.is_empty())
        .map(|(index, chain)| {
            let cold = function.blocks[chain[0].0].layout == BlockLayout::Cold;
            let rank = chain.iter().map(|block| ranks[block]).min().unwrap();
            (index, cold, rank)
        })
        .collect::<Vec<_>>();
    chain_order.sort_by_key(|(index, cold, rank)| (*index != entry_chain, *cold, *rank));
    chain_order
        .into_iter()
        .flat_map(|(index, _, _)| chains[index].iter().copied())
        .collect()
}

#[allow(clippy::too_many_arguments)]
fn lower_switch(
    function: &FunctionUses<'_>,
    block: BlockId,
    next_block: Option<BlockId>,
    preferred_tail_after: Option<BlockId>,
    value: ValueId,
    cases: &[(ValueId, Edge)],
    default: &Edge,
    output: &mut Vec<Instruction>,
    temporary_index: &mut usize,
) -> Result<Option<(BlockId, Vec<Instruction>)>, String> {
    let value = value_operand(function, value)?;
    if let Some((group_length, preferred_edge)) = first_preferred_switch_group(function, cases, next_block)
        && let Some(preferred_tail_after) = preferred_tail_after
    {
        let tail_label = Label::new(format!(".ssa_switch_{}_after_preferred", block.0));
        if emit_inverted_switch_group_test(function, &value, &cases[..group_length], &tail_label, output)? {
            lower_edge_copies(function, preferred_edge, output, temporary_index)?;
            emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(preferred_edge.block))];);
            let mut tail = vec![machine_instruction(
                MachineOperation::Label,
                vec![Operand::Label(tail_label)],
            )];
            lower_switch_cases(
                function,
                block,
                &value,
                cases,
                group_length,
                default,
                &mut tail,
                temporary_index,
            )?;
            return Ok(Some((preferred_tail_after, tail)));
        }
    }
    lower_switch_cases(function, block, &value, cases, 0, default, output, temporary_index)?;
    Ok(None)
}

fn preferred_switch_tail_after(
    block_order: &[BlockId],
    position: usize,
    cases: &[(ValueId, Edge)],
    default: &Edge,
) -> Option<BlockId> {
    let (_, preferred) = cases.first()?;
    if block_order.get(position + 1).copied() != Some(preferred.block) {
        return None;
    }
    let competing_targets = cases
        .iter()
        .map(|(_, edge)| edge.block)
        .filter(|target| *target != preferred.block)
        .chain([default.block])
        .collect::<HashSet<_>>();
    let competing_position = block_order[position + 1..]
        .iter()
        .position(|block| competing_targets.contains(block))?
        + position
        + 1;
    competing_position.checked_sub(1).map(|position| block_order[position])
}

fn first_preferred_switch_group<'a>(
    function: &Function,
    cases: &'a [(ValueId, Edge)],
    next_block: Option<BlockId>,
) -> Option<(usize, &'a Edge)> {
    let (_, first_edge) = cases.first()?;
    if Some(first_edge.block) != next_block || function.blocks[first_edge.block.0].layout != BlockLayout::Preferred {
        return None;
    }
    let length = cases.iter().take_while(|(_, edge)| edge == first_edge).count();
    Some((length, first_edge))
}

fn emit_inverted_switch_group_test(
    function: &FunctionUses<'_>,
    value: &Operand,
    cases: &[(ValueId, Edge)],
    failure: &Label,
    output: &mut Vec<Instruction>,
) -> Result<bool, String> {
    let patterns = cases.iter().map(|(pattern, _)| *pattern).collect::<Vec<_>>();
    if patterns.len() == 1 {
        emit!(output; MachineOperation::branch_equality(IntegerWidth::U64, EqualityCondition::NotEqual) => [value.clone(), value_operand(function, patterns[0])?, Operand::Label(failure.clone())];);
        return Ok(true);
    }
    let values = patterns
        .iter()
        .map(|pattern| integer_constant(function, *pattern))
        .collect::<Option<Vec<_>>>();
    let Some(values) = values.filter(|values| {
        values.last().unwrap().checked_add(1).is_some()
            && values.windows(2).all(|pair| pair[0].checked_add(1) == Some(pair[1]))
    }) else {
        return Ok(false);
    };
    let start = values[0];
    let end = values.last().unwrap().checked_add(1).unwrap();
    if start != 0 {
        emit!(output; MachineOperation::branch_ordered(IntegerWidth::U64, OrderedCondition::Less, IntegerSignedness::Unsigned) => [value.clone(), Operand::Immediate(start), Operand::Label(failure.clone())];);
    }
    emit!(output; MachineOperation::branch_ordered(IntegerWidth::U64, OrderedCondition::GreaterOrEqual, IntegerSignedness::Unsigned) => [value.clone(), Operand::Immediate(end), Operand::Label(failure.clone())];);
    Ok(true)
}

#[allow(clippy::too_many_arguments)]
fn lower_switch_cases(
    function: &FunctionUses<'_>,
    block: BlockId,
    value: &Operand,
    cases: &[(ValueId, Edge)],
    start_index: usize,
    default: &Edge,
    output: &mut Vec<Instruction>,
    temporary_index: &mut usize,
) -> Result<(), String> {
    let mut case_groups = Vec::new();
    let mut index = start_index;
    while index < cases.len() {
        let (pattern, edge) = &cases[index];
        let target = Operand::Label(Label::new(format!(".ssa_switch_{}_case_{index}", block.0)));
        let mut patterns = vec![*pattern];
        while matches!(cases.get(index + patterns.len()), Some((_, next_edge)) if next_edge == edge) {
            patterns.push(cases[index + patterns.len()].0);
        }
        let consecutive_integers = patterns
            .iter()
            .map(|pattern| integer_constant(function, *pattern))
            .collect::<Option<Vec<_>>>()
            .filter(|values| {
                values.len() > 1
                    && values.last().unwrap().checked_add(1).is_some()
                    && values.windows(2).all(|pair| pair[0].checked_add(1) == Some(pair[1]))
            });
        if let Some(values) = consecutive_integers {
            let start = values[0];
            let end = values.last().unwrap().checked_add(1).unwrap();
            if start == 0 {
                emit!(output; MachineOperation::branch_ordered(IntegerWidth::U64, OrderedCondition::Less, IntegerSignedness::Unsigned) => [value.clone(), Operand::Immediate(end), target];);
            } else {
                let next = Label::new(format!(".ssa_switch_{}_after_{index}", block.0));
                emit!(output; MachineOperation::branch_ordered(IntegerWidth::U64, OrderedCondition::Less, IntegerSignedness::Unsigned) => [value.clone(), Operand::Immediate(start), Operand::Label(next.clone())];);
                emit!(output; MachineOperation::branch_ordered(IntegerWidth::U64, OrderedCondition::Less, IntegerSignedness::Unsigned) => [value.clone(), Operand::Immediate(end), target];);
                emit!(output; MachineOperation::Label => [Operand::Label(next)];);
            }
            case_groups.push((index, edge));
            index += patterns.len();
            continue;
        }
        let operation = if patterns.len() == 1 {
            MachineOperation::branch_equality(IntegerWidth::U64, EqualityCondition::Equal)
        } else {
            MachineOperation::Branch(BranchOperation::AnyEqual)
        };
        let operands = std::iter::once(value.clone())
            .chain(
                patterns
                    .iter()
                    .map(|pattern| value_operand(function, *pattern))
                    .collect::<Result<Vec<_>, _>>()?,
            )
            .chain([target])
            .collect();
        output.push(machine_instruction(operation, operands));
        case_groups.push((index, edge));
        index += patterns.len();
    }
    lower_edge_copies(function, default, output, temporary_index)?;
    emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(default.block))];);
    for (index, edge) in case_groups {
        emit!(output; MachineOperation::Label => [Operand::Label(Label::new(format!(".ssa_switch_{}_case_{index}", block.0)))];);
        lower_edge_copies(function, edge, output, temporary_index)?;
        emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(edge.block))];);
    }
    Ok(())
}

#[allow(clippy::too_many_arguments)]
fn lower_checked_operation(
    function: &FunctionUses<'_>,
    block: BlockId,
    operation: &Operation,
    inputs: &[ValueId],
    results: &[ValueId],
    success: &Edge,
    failure: &Edge,
    output: &mut Vec<Instruction>,
    temporary_index: &mut usize,
) -> Result<(), String> {
    let failure_label = Label::new(format!(".ssa_checked_{}_failure", block.0));
    let mut success = success.clone();
    let mut failure = failure.clone();
    let mut failure_inout_update = None;
    let mut failure_recovery = None;
    if let Operation::Intrinsic(Intrinsic::Branch(operation)) = operation {
        if let Some(machine_operation) = typed_narrow_signed_branch_operation(function, *operation, inputs) {
            let [lhs, rhs] = require_inputs(inputs, operation.name())?;
            let lhs = folded_32bit_operation_source(function, *lhs)
                .map(|(_, source)| source)
                .unwrap_or(*lhs);
            let rhs = folded_32bit_operation_source(function, *rhs)
                .map(|(_, source)| source)
                .unwrap_or(*rhs);
            emit!(output; machine_operation => [value_operand(function, lhs)?, value_operand(function, rhs)?, Operand::Label(failure_label.clone())];);
        } else {
            let operands = results
                .iter()
                .chain(inputs)
                .map(|value| value_operand(function, *value))
                .collect::<Result<Vec<_>, _>>()?
                .into_iter()
                .chain([Operand::Label(failure_label.clone())])
                .collect();
            output.push(machine_instruction(MachineOperation::Branch(*operation), operands));
        }
    } else if let Operation::Intrinsic(Intrinsic::FloatingPoint(operation)) = operation {
        if !operation.is_fallible() {
            return Err(format!(
                "non-fallible floating-point operation '{}' used as a checked operation",
                operation.name()
            ));
        }
        let operands = results
            .iter()
            .chain(inputs)
            .map(|value| value_operand(function, *value))
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .chain([Operand::Label(failure_label.clone())])
            .collect();
        output.push(machine_instruction(MachineOperation::Float(*operation), operands));
    } else if let Operation::Intrinsic(Intrinsic::CheckedInteger(operation)) = operation {
        if *operation != CheckedIntegerOperation::Negate {
            let [result] = require_results(results, operation.name())?;
            let [lhs, rhs] = require_inputs(inputs, operation.name())?;
            let result_id = *result;
            let rhs_bytecode_field = folded_checked_i32_bytecode_field(function, *rhs);
            let lhs_unbox = recoverable_checked_i32_unbox_source(function, *lhs, block);
            let rhs_unbox = recoverable_checked_i32_unbox_source(function, *rhs, block);
            let can_fold_inputs = lhs_unbox.is_some() && (rhs_bytecode_field.is_some() || rhs_unbox.is_some());
            let can_recover_inputs = can_fold_inputs && *operation != CheckedIntegerOperation::Multiply;
            let lhs = if can_fold_inputs {
                lhs_unbox
            } else {
                folded_checked_i32_source(function, *lhs)
            }
            .map(|(_, source)| source)
            .unwrap_or(*lhs);
            let result = if can_recover_inputs {
                for argument in &mut success.arguments {
                    if *argument == result_id {
                        *argument = lhs;
                    }
                }
                value_operand(function, lhs)?
            } else {
                value_operand(function, result_id)?
            };
            let rhs_value = if can_fold_inputs && rhs_bytecode_field.is_none() {
                rhs_unbox
            } else {
                folded_checked_i32_source(function, *rhs)
            }
            .map(|(_, source)| source)
            .unwrap_or(*rhs);
            let rhs_operand = rhs_bytecode_field
                .map(|(_, field)| bytecode_field_memory(&field))
                .map_or_else(|| value_operand(function, rhs_value), Ok)?;
            let checked_operation = match operation {
                CheckedIntegerOperation::Add => OverflowOperation::AddWithOverflow,
                CheckedIntegerOperation::Subtract => OverflowOperation::SubtractWithOverflow,
                CheckedIntegerOperation::Multiply => OverflowOperation::MultiplyWithOverflow,
                CheckedIntegerOperation::Negate => unreachable!(),
            };
            if checked_operation == OverflowOperation::MultiplyWithOverflow {
                emit!(output; MachineOperation::Overflow(OverflowOperation::MultiplyCopy) => [result, value_operand(function, lhs)?, value_operand(function, rhs_value)?, Operand::Label(failure_label.clone())];);
            } else {
                if !can_recover_inputs {
                    emit!(output; MachineOperation::Move(IntegerWidth::U64) => [result.clone(), value_operand(function, lhs)?];);
                }
                if checked_operation == OverflowOperation::AddWithOverflow
                    && integer_constant(function, *rhs) == Some(1)
                {
                    emit!(output; MachineOperation::Overflow(OverflowOperation::Increment) => [result, Operand::Label(failure_label.clone())];);
                } else if checked_operation == OverflowOperation::SubtractWithOverflow
                    && integer_constant(function, *rhs) == Some(1)
                {
                    emit!(output; MachineOperation::Overflow(OverflowOperation::Decrement) => [result, Operand::Label(failure_label.clone())];);
                } else {
                    emit!(output; MachineOperation::Overflow(checked_operation) => [result.clone(), rhs_operand.clone(), Operand::Label(failure_label.clone())];);
                    if can_recover_inputs {
                        failure_recovery = Some((
                            match checked_operation {
                                OverflowOperation::AddWithOverflow => OverflowOperation::RecoverAddLhs,
                                OverflowOperation::SubtractWithOverflow => OverflowOperation::RecoverSubtractLhs,
                                _ => unreachable!(),
                            },
                            result.clone(),
                            rhs_operand.clone(),
                        ));
                    }
                }
            }
        } else {
            let [result] = require_results(results, "neg32_overflow")?;
            let [input] = require_inputs(inputs, "neg32_overflow")?;
            let result_id = *result;
            let input_id = *input;
            let input = folded_checked_i32_source(function, input_id)
                .map(|(_, source)| source)
                .unwrap_or(input_id);
            let input_operand = value_operand(function, input)?;
            let result = value_operand(function, *result)?;
            emit!(output; MachineOperation::Move(IntegerWidth::U64) => [result.clone(), input_operand.clone()];);
            emit!(output; MachineOperation::Overflow(OverflowOperation::Negate) => [result.clone(), Operand::Label(failure_label.clone())];);
            // A checked inout operation exposes its destructive result to both
            // continuations. Updating the captured input lets allocation coalesce
            // the copies while preserving that value on the failure edge.
            failure_inout_update = Some((input_operand.clone(), result));
            for argument in &mut failure.arguments {
                if *argument == input_id || value_operand(function, *argument)? == input_operand {
                    *argument = result_id;
                }
            }
        }
    } else {
        let (operation_name, machine_operation) = match operation {
            Operation::Parameter(parameter) => {
                return Err(format!(
                    "unresolved operation parameter {} reached LowIR lowering",
                    parameter.0
                ));
            }
            Operation::Intrinsic(Intrinsic::LowLevel(operation)) => {
                (operation.name(), Some(low_level_machine_operation(*operation)))
            }
            Operation::Intrinsic(Intrinsic::Control(ControlOperation::JumpLabel)) => (
                ControlOperation::JumpLabel.name(),
                Some(MachineOperation::Control(ControlOperation::JumpLabel)),
            ),
            _ => return Err("checked machine-state assignments are unsupported".to_string()),
        };
        let operands = results
            .iter()
            .chain(inputs)
            .map(|value| value_operand(function, *value))
            .collect::<Result<Vec<_>, _>>()?
            .into_iter()
            .chain([Operand::Label(failure_label.clone())])
            .collect();
        output.push(machine_instruction(
            machine_operation.ok_or_else(|| format!("checked operation '{operation_name}' has no LowIR operation"))?,
            operands,
        ));
    }
    lower_edge_copies(function, &success, output, temporary_index)?;
    emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(success.block))];);
    if function.blocks[failure.block.0].layout == BlockLayout::Cold {
        emit!(output; MachineOperation::Cold => [Operand::Label(failure_label.clone())];);
    }
    emit!(output; MachineOperation::Label => [Operand::Label(failure_label)];);
    if let Some((operation, result, rhs)) = failure_recovery {
        emit!(output; MachineOperation::Overflow(operation) => [result, rhs];);
    }
    if let Some((input, result)) = failure_inout_update {
        emit!(output; MachineOperation::Move(IntegerWidth::U64) => [input, result];);
    }
    lower_edge_copies(function, &failure, output, temporary_index)?;
    emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [Operand::Label(block_label(failure.block))];);
    Ok(())
}

struct SelectedBranch {
    instruction: InstructionId,
    folded_inputs: Vec<InstructionId>,
    early_branches: Vec<SelectedEarlyBranch>,
    and_mask: Option<(ValueId, ValueId, KnownLayoutConstant)>,
    operation: MachineOperation,
    inputs: Vec<SelectedBranchInput>,
    direct_tag_source: Option<DirectTagSource>,
}

impl SelectedBranch {
    fn new(instruction: InstructionId, operation: MachineOperation, inputs: Vec<SelectedBranchInput>) -> Self {
        Self {
            instruction,
            operation,
            inputs,
            folded_inputs: Vec::new(),
            early_branches: Vec::new(),
            and_mask: None,
            direct_tag_source: None,
        }
    }
}

#[derive(Clone)]
struct DirectTagSource {
    tag: ValueId,
    value: ValueId,
    extract_instruction: InstructionId,
}

struct SelectedEarlyBranch {
    operation: MachineOperation,
    inputs: Vec<SelectedBranchInput>,
}

#[derive(Clone)]
enum SelectedBranchInput {
    Value(ValueId),
    Operand(Operand),
    Immediate(i64),
    Layout(KnownLayoutConstant),
}

fn selected_branch(function: &FunctionUses<'_>, condition: ValueId) -> Option<SelectedBranch> {
    let mut branch = select_branch(function, condition)?;
    fold_load_into_branch(function, &mut branch);
    Some(branch)
}

fn select_branch(function: &FunctionUses<'_>, condition: ValueId) -> Option<SelectedBranch> {
    let (instruction, operation) = single_use_instruction(function, condition)?;
    let (comparison, classification) = match &operation.operation {
        Operation::Intrinsic(Intrinsic::IntegerComparison(comparison)) => (Some(*comparison), None),
        Operation::Intrinsic(Intrinsic::Classification(classification)) => (None, Some(*classification)),
        _ => return None,
    };
    if matches!(
        comparison,
        Some(IntegerComparisonOperation::Equal | IntegerComparisonOperation::NotEqual)
    ) {
        let [lhs, rhs] = operation.inputs.as_slice() else {
            return None;
        };
        let nonzero_value = if is_integer_zero(function, *rhs) {
            Some(*lhs)
        } else if is_integer_zero(function, *lhs) {
            Some(*rhs)
        } else {
            None
        };
        if let Some(value) = nonzero_value {
            let mut branch = SelectedBranch::new(
                instruction,
                MachineOperation::branch_zero(
                    if matches!(function.values[value.0].ty, Type::I32 | Type::U32) {
                        IntegerWidth::U32
                    } else {
                        IntegerWidth::U64
                    },
                    match comparison.unwrap() {
                        IntegerComparisonOperation::Equal => ZeroCondition::Zero,
                        IntegerComparisonOperation::NotEqual => ZeroCondition::NonZero,
                        _ => unreachable!(),
                    },
                ),
                vec![SelectedBranchInput::Value(value)],
            );
            if function.values[value.0].ty == Type::I32
                && let Some((instructions, source)) = folded_32bit_operation_source(function, value)
            {
                branch.folded_inputs = instructions;
                branch.inputs[0] = SelectedBranchInput::Value(source);
            }
            return Some(branch);
        }
    }
    if matches!(
        comparison,
        Some(IntegerComparisonOperation::Relational {
            relation: ComparisonRelation::LessOrEqual | ComparisonRelation::Greater,
            domain: ComparisonDomain::UnsignedInteger,
        })
    ) {
        let [lhs, rhs] = operation.inputs.as_slice() else {
            return None;
        };
        if function.values[lhs.0].ty == Type::U32 && is_i32_max(function, *rhs) {
            return Some(SelectedBranch::new(
                instruction,
                if matches!(
                    comparison,
                    Some(IntegerComparisonOperation::Relational {
                        relation: ComparisonRelation::LessOrEqual,
                        ..
                    })
                ) {
                    MachineOperation::branch_bit(TestCondition::Clear)
                } else {
                    MachineOperation::branch_bit(TestCondition::Set)
                },
                vec![SelectedBranchInput::Value(*lhs), SelectedBranchInput::Immediate(31)],
            ));
        }
    }
    if matches!(
        comparison,
        Some(IntegerComparisonOperation::Relational {
            relation: ComparisonRelation::Less | ComparisonRelation::GreaterOrEqual,
            domain: ComparisonDomain::SignedInteger,
        })
    ) {
        let [lhs, rhs] = operation.inputs.as_slice() else {
            return None;
        };
        if is_integer_zero(function, *rhs) {
            let width = if function.values[lhs.0].ty == Type::I32 {
                IntegerWidth::U32
            } else {
                IntegerWidth::U64
            };
            let (lhs, folded_inputs) = folded_32bit_operation_source(function, *lhs)
                .or_else(|| {
                    single_use_reused_source(function, *lhs).map(|(instruction, source)| (vec![instruction], source))
                })
                .map(|(instructions, source)| (source, instructions))
                .unwrap_or((*lhs, Vec::new()));
            let mut branch = SelectedBranch::new(
                instruction,
                if matches!(
                    comparison,
                    Some(IntegerComparisonOperation::Relational {
                        relation: ComparisonRelation::Less,
                        ..
                    })
                ) {
                    MachineOperation::branch_sign(width, SignCondition::Negative)
                } else {
                    MachineOperation::branch_sign(width, SignCondition::NotNegative)
                },
                vec![SelectedBranchInput::Value(lhs)],
            );
            branch.folded_inputs = folded_inputs;
            return Some(branch);
        }
    }
    if matches!(
        classification,
        Some(ClassificationOperation::Float64Tag | ClassificationOperation::NumberTag)
    ) {
        let [tag] = operation.inputs.as_slice() else {
            return None;
        };
        let destination = if value_use_count(function, *tag) == 1 {
            *tag
        } else {
            *operation.results.first()?
        };
        let direct_tag_source = (destination == *tag)
            .then(|| direct_tag_source(function, *tag))
            .flatten();
        let mut branch = SelectedBranch::new(
            instruction,
            MachineOperation::branch_equality(IntegerWidth::U16, EqualityCondition::NotEqual),
            vec![
                SelectedBranchInput::Value(destination),
                SelectedBranchInput::Layout(KnownLayoutConstant::NanBaseTag),
            ],
        );
        branch.folded_inputs = direct_tag_source
            .as_ref()
            .map(|source| vec![source.extract_instruction])
            .unwrap_or_default();
        branch.early_branches = (classification == Some(ClassificationOperation::NumberTag))
            .then(|| SelectedEarlyBranch {
                operation: MachineOperation::branch_equality(IntegerWidth::U16, EqualityCondition::Equal),
                inputs: vec![
                    SelectedBranchInput::Value(*tag),
                    SelectedBranchInput::Layout(KnownLayoutConstant::Int32Tag),
                ],
            })
            .into_iter()
            .collect();
        branch.and_mask = Some((*tag, destination, KnownLayoutConstant::NanBaseTag));
        branch.direct_tag_source = direct_tag_source;
        return Some(branch);
    }
    let compares_u32 = operation
        .inputs
        .iter()
        .all(|input| function.values[input.0].ty == Type::U32);
    let compares_i32 = operation
        .inputs
        .iter()
        .all(|input| function.values[input.0].ty == Type::I32);
    let compares_value_tags = operation
        .inputs
        .iter()
        .all(|input| function.values[input.0].ty == Type::ValueTag);
    let comparison = comparison?;
    let branch_operation = match comparison {
        IntegerComparisonOperation::Equal if compares_value_tags => {
            MachineOperation::branch_equality(IntegerWidth::U16, EqualityCondition::Equal)
        }
        IntegerComparisonOperation::NotEqual if compares_value_tags => {
            MachineOperation::branch_equality(IntegerWidth::U16, EqualityCondition::NotEqual)
        }
        IntegerComparisonOperation::Equal if compares_u32 => {
            MachineOperation::branch_equality(IntegerWidth::U32, EqualityCondition::Equal)
        }
        IntegerComparisonOperation::NotEqual if compares_u32 => {
            MachineOperation::branch_equality(IntegerWidth::U32, EqualityCondition::NotEqual)
        }
        IntegerComparisonOperation::Equal => {
            MachineOperation::branch_equality(IntegerWidth::U64, EqualityCondition::Equal)
        }
        IntegerComparisonOperation::NotEqual => {
            MachineOperation::branch_equality(IntegerWidth::U64, EqualityCondition::NotEqual)
        }
        IntegerComparisonOperation::Relational { relation, domain } => MachineOperation::branch_ordered(
            if compares_i32 {
                IntegerWidth::U32
            } else {
                IntegerWidth::U64
            },
            relation,
            match domain {
                ComparisonDomain::SignedInteger => IntegerSignedness::Signed,
                ComparisonDomain::UnsignedInteger => IntegerSignedness::Unsigned,
                ComparisonDomain::Value => return None,
            },
        ),
        IntegerComparisonOperation::BitsSet => {
            MachineOperation::branch_bits(MemoryWidth::DoubleWord, TestCondition::Set)
        }
        IntegerComparisonOperation::BitsClear => {
            MachineOperation::branch_bits(MemoryWidth::DoubleWord, TestCondition::Clear)
        }
    };
    let direct_tag_source = operation
        .inputs
        .iter()
        .find_map(|input| direct_tag_source(function, *input));
    let mut folded_inputs = direct_tag_source
        .as_ref()
        .map(|source| vec![source.extract_instruction])
        .unwrap_or_default();
    let inputs = operation
        .inputs
        .iter()
        .copied()
        .map(|input| {
            if matches!(
                branch_operation,
                MachineOperation::Branch(BranchOperation::Equality {
                    width: IntegerWidth::U32,
                    ..
                }) | MachineOperation::Branch(BranchOperation::Ordered {
                    width: IntegerWidth::U32,
                    ..
                })
            ) && let Some((instructions, source)) = folded_32bit_operation_source(function, input)
            {
                folded_inputs.extend(instructions);
                source
            } else {
                input
            }
        })
        .map(SelectedBranchInput::Value)
        .collect();
    let mut branch = SelectedBranch::new(instruction, branch_operation, inputs);
    branch.folded_inputs = folded_inputs;
    branch.direct_tag_source = direct_tag_source;
    Some(branch)
}

fn branch_input_operand(
    function: &FunctionUses<'_>,
    constants: &LayoutConstants,
    input: &SelectedBranchInput,
) -> Result<Operand, String> {
    match input {
        SelectedBranchInput::Value(value) => value_operand(function, *value),
        SelectedBranchInput::Operand(operand) => Ok(operand.clone()),
        SelectedBranchInput::Immediate(value) => Ok(Operand::Immediate(*value)),
        SelectedBranchInput::Layout(value) => layout_operand(constants, *value),
    }
}

fn fold_load_into_branch(function: &FunctionUses<'_>, branch: &mut SelectedBranch) {
    for load_index in 0..branch.inputs.len().min(2) {
        let SelectedBranchInput::Value(value) = branch.inputs[load_index] else {
            continue;
        };
        let Some(load) = lowered_single_use_load(function, value, branch) else {
            continue;
        };
        let other_index = usize::from(load_index == 0);
        let other = branch.inputs.get(other_index).cloned();
        let byte_immediate = other
            .as_ref()
            .and_then(|input| branch_input_integer(function, input))
            .filter(|value| (0..=u8::MAX as i64).contains(value));
        let (operation, other) = match (load.width, branch.operation) {
            (
                MemoryWidth::Byte,
                MachineOperation::Branch(BranchOperation::Equality {
                    width: IntegerWidth::U64,
                    condition,
                }),
            ) if byte_immediate.is_some() => (
                MachineOperation::branch_equality(IntegerWidth::U8, condition),
                other.unwrap(),
            ),
            (
                MemoryWidth::Byte,
                MachineOperation::Branch(BranchOperation::Bits {
                    width: MemoryWidth::DoubleWord,
                    condition,
                }),
            ) if byte_immediate.is_some() => (
                MachineOperation::branch_bits(MemoryWidth::Byte, condition),
                other.unwrap(),
            ),
            (
                MemoryWidth::Byte,
                MachineOperation::Branch(BranchOperation::Zero {
                    width: IntegerWidth::U64,
                    condition,
                }),
            ) => (
                MachineOperation::branch_equality(
                    IntegerWidth::U8,
                    condition.select(EqualityCondition::Equal, EqualityCondition::NotEqual),
                ),
                SelectedBranchInput::Immediate(0),
            ),
            (
                width @ (MemoryWidth::Word | MemoryWidth::DoubleWord),
                MachineOperation::Branch(BranchOperation::Equality {
                    width: branch_width,
                    condition,
                }),
            ) if other
                .as_ref()
                .is_some_and(|other| branch_register_is_compatible(function, other, width, branch_width)) =>
            {
                (
                    MachineOperation::branch_memory(
                        match width {
                            MemoryWidth::Word => PairWidth::Word,
                            MemoryWidth::DoubleWord => PairWidth::DoubleWord,
                            _ => unreachable!(),
                        },
                        condition,
                    ),
                    other.unwrap(),
                )
            }
            _ => continue,
        };
        branch.operation = operation;
        branch.inputs = vec![SelectedBranchInput::Operand(load.address), other];
        branch.folded_inputs.extend(load.instructions);
        return;
    }
}

fn branch_register_is_compatible(
    function: &FunctionUses<'_>,
    input: &SelectedBranchInput,
    load_width: MemoryWidth,
    branch_width: IntegerWidth,
) -> bool {
    let SelectedBranchInput::Value(value) = input else {
        return false;
    };
    if !matches!(
        (load_width, branch_width),
        (MemoryWidth::Word, IntegerWidth::U32 | IntegerWidth::U64) | (MemoryWidth::DoubleWord, IntegerWidth::U64)
    ) {
        return false;
    }
    matches!(value_operand(function, *value), Ok(Operand::VirtualRegister(_)))
        && (load_width == MemoryWidth::DoubleWord
            || branch_width == IntegerWidth::U32
            || matches!(
                &function.values[resolve_reuse_value(function, *value).0].ty,
                Type::I8 | Type::I16 | Type::I32 | Type::U8 | Type::U16 | Type::U32
            ))
}

struct LoweredLoad {
    instructions: Vec<InstructionId>,
    address: Operand,
    width: MemoryWidth,
}

fn lowered_single_use_load(
    function: &FunctionUses<'_>,
    value: ValueId,
    branch: &SelectedBranch,
) -> Option<LoweredLoad> {
    let mut value = value;
    let mut instructions = Vec::new();
    loop {
        let (instruction, source) = single_use_instruction(function, value)?;
        if source.operation != Operation::Intrinsic(Intrinsic::Value(ValueOperation::Reuse)) {
            instructions.push(instruction);
            break;
        }
        instructions.push(instruction);
        let [source] = source.inputs.as_slice() else {
            return None;
        };
        value = *source;
    }
    let load_instruction = *instructions.last()?;
    let omitted = instructions
        .iter()
        .chain(&branch.folded_inputs)
        .copied()
        .collect::<HashSet<_>>();
    let (load_block, first) = function.position(load_instruction)?;
    let (branch_block, last) = function.position(branch.instruction)?;
    if load_block != branch_block || first >= last {
        return None;
    }
    let between = &function.blocks[load_block as usize].instructions[first as usize + 1..last as usize];
    if !between.iter().all(|id| omitted.contains(id)) {
        return None;
    }
    let mut lowered = Vec::new();
    lower_instruction(
        function,
        load_instruction,
        &function.instructions[load_instruction.0],
        &mut lowered,
    )
    .ok()?;
    let [load] = lowered.as_slice() else {
        return None;
    };
    let MachineOperation::Memory(MemoryOperation::Load { width, signed: false }) = load.opcode.operation() else {
        return None;
    };
    let [destination, address] = load.operands.as_slice() else {
        return None;
    };
    if destination != &value_operand(function, value).ok()? || !matches!(address, Operand::Address(_)) {
        return None;
    }
    Some(LoweredLoad {
        instructions,
        address: address.clone(),
        width,
    })
}

fn branch_input_integer(function: &Function, input: &SelectedBranchInput) -> Option<i64> {
    match input {
        SelectedBranchInput::Value(value) => match function.values[value.0].definition {
            ValueDefinition::Constant(Constant::Integer(value)) => Some(value),
            ValueDefinition::Constant(Constant::Layout(value)) => Some(value.value()),
            _ => None,
        },
        SelectedBranchInput::Immediate(value) => Some(*value),
        SelectedBranchInput::Operand(Operand::Immediate(value)) => Some(*value),
        _ => None,
    }
}

fn direct_tag_source(function: &FunctionUses<'_>, tag: ValueId) -> Option<DirectTagSource> {
    let (extract_instruction, extract) = single_use_instruction(function, tag)?;
    if extract.operation != Operation::Intrinsic(Intrinsic::Value(ValueOperation::ExtractTag { rematerialized: false }))
    {
        return None;
    }
    let [value] = extract.inputs.as_slice() else {
        return None;
    };
    Some(DirectTagSource {
        tag,
        value: *value,
        extract_instruction,
    })
}

fn selected_int32_pair(function: &FunctionUses<'_>, condition: ValueId) -> Option<(InstructionId, ValueId, ValueId)> {
    let (instruction, operation) = single_use_instruction(function, condition)?;
    if operation.operation != Operation::Intrinsic(Intrinsic::Classification(ClassificationOperation::Int32Pair)) {
        return None;
    }
    let [lhs, rhs] = operation.inputs.as_slice() else {
        return None;
    };
    Some((instruction, *lhs, *rhs))
}

fn single_use_reused_source(function: &FunctionUses<'_>, value: ValueId) -> Option<(InstructionId, ValueId)> {
    let (instruction, reuse) = single_use_instruction(function, value)?;
    if reuse.operation != Operation::Intrinsic(Intrinsic::Value(ValueOperation::Reuse)) {
        return None;
    }
    let [source] = reuse.inputs.as_slice() else {
        return None;
    };
    Some((instruction, *source))
}

fn single_use_instruction<'a>(
    function: &'a FunctionUses<'a>,
    value: ValueId,
) -> Option<(InstructionId, &'a super::ir::Instruction)> {
    if value_use_count(function, value) != 1 {
        return None;
    }
    defining_instruction(function, value)
}

fn defining_instruction(function: &Function, value: ValueId) -> Option<(InstructionId, &super::ir::Instruction)> {
    let ValueDefinition::InstructionResult { instruction, .. } = function.values[value.0].definition else {
        return None;
    };
    Some((instruction, &function.instructions[instruction.0]))
}

/// A function together with how many times each of its values is used.
///
/// Lowering asks that question constantly, and answering it by scanning the
/// whole function each time makes compilation quadratic in the function size.
pub(crate) struct FunctionUses<'a> {
    function: &'a Function,
    counts: Vec<u32>,
    user_offsets: Vec<u32>,
    users: Vec<ValueUser>,
    registers: Vec<Option<VirtualRegister>>,
    positions: Vec<Option<(u32, u32)>>,
}

/// One place a value is read.
#[derive(Clone, Copy)]
enum ValueUser {
    Instruction(InstructionId),
    Terminator(BlockId),
}

impl<'a> FunctionUses<'a> {
    fn new(function: &'a Function, handler: crate::identity::HandlerId) -> Self {
        let mut counts = vec![0u32; function.values.len()];
        let mut terminator_inputs = Vec::new();
        // Where each instruction sits. Selection asks whether two instructions
        // are adjacent in a block, and searching the whole function for them is
        // quadratic on a large program.
        let mut positions = vec![None; function.instructions.len()];
        for (block_index, block) in function.blocks.iter().enumerate() {
            for (position, instruction) in block.instructions.iter().enumerate() {
                positions[instruction.0] = Some((block_index as u32, position as u32));
            }
        }
        for instruction in &function.instructions {
            for input in &instruction.inputs {
                counts[input.0] += 1;
            }
        }
        for block in &function.blocks {
            let Some(terminator) = block.terminator.as_ref() else {
                continue;
            };
            for input in terminator.inputs() {
                counts[input.0] += 1;
            }
        }
        // The users of each value live in one flat array, indexed by a per-value
        // offset. Asking who reads a value is the question the narrowing
        // analysis asks about nearly every result it lowers.
        let mut user_offsets = Vec::with_capacity(counts.len() + 1);
        let mut total = 0;
        for count in &counts {
            user_offsets.push(total);
            total += count;
        }
        user_offsets.push(total);
        let mut filled = user_offsets.clone();
        let mut users = vec![ValueUser::Instruction(InstructionId(0)); total as usize];
        for (index, instruction) in function.instructions.iter().enumerate() {
            for input in &instruction.inputs {
                users[filled[input.0] as usize] = ValueUser::Instruction(InstructionId(index));
                filled[input.0] += 1;
            }
        }
        for (index, block) in function.blocks.iter().enumerate() {
            let Some(terminator) = block.terminator.as_ref() else {
                continue;
            };
            terminator_inputs.clear();
            terminator_inputs.extend(terminator.inputs());
            for input in &terminator_inputs {
                users[filled[input.0] as usize] = ValueUser::Terminator(BlockId(index));
                filled[input.0] += 1;
            }
        }
        let mut register_index = 0u32;
        let registers = function
            .values
            .iter()
            .enumerate()
            .map(|(value_index, value)| {
                if !matches!(
                    value.definition,
                    ValueDefinition::InstructionResult { .. }
                        | ValueDefinition::BlockParameter { .. }
                        | ValueDefinition::TerminatorResult { .. }
                ) {
                    return None;
                }
                let class = value.ty.register_class()?;
                let register = VirtualRegister::ssa(handler, register_index, value_index, class);
                register_index = register_index
                    .checked_add(1)
                    .expect("one handler cannot contain more than u32::MAX virtual registers");
                Some(register)
            })
            .collect();
        Self {
            function,
            counts,
            user_offsets,
            users,
            registers,
            positions,
        }
    }

    /// The block an instruction belongs to and where it sits in it.
    fn position(&self, instruction: InstructionId) -> Option<(u32, u32)> {
        self.positions[instruction.0]
    }

    /// The virtual register an SSA value is named by, if it lives in one.
    fn register(&self, value: ValueId) -> Option<&VirtualRegister> {
        self.registers[value.0].as_ref()
    }

    fn users_of(&self, value: ValueId) -> &[ValueUser] {
        &self.users[self.user_offsets[value.0] as usize..self.user_offsets[value.0 + 1] as usize]
    }
}

impl std::ops::Deref for FunctionUses<'_> {
    type Target = Function;

    fn deref(&self) -> &Function {
        self.function
    }
}

fn value_use_count(function: &FunctionUses<'_>, value: ValueId) -> usize {
    function.counts[value.0] as usize
}

fn is_integer_zero(function: &Function, value: ValueId) -> bool {
    matches!(
        function.values[value.0].definition,
        ValueDefinition::Constant(Constant::Integer(0))
    )
}

fn is_i32_max(function: &Function, value: ValueId) -> bool {
    match &function.values[value.0].definition {
        ValueDefinition::Constant(Constant::Integer(value)) => *value == i32::MAX as i64,
        _ => false,
    }
}

fn direct_tag_constant(function: &Function, value: ValueId) -> Option<Operand> {
    match &function.values[value.0].definition {
        ValueDefinition::Constant(Constant::Layout(constant)) if constant.category() == LayoutConstantCategory::Tag => {
            Some(Operand::LayoutConstant(*constant))
        }
        ValueDefinition::Constant(Constant::Integer(tag)) if u16::try_from(*tag).is_ok() => {
            Some(Operand::Immediate(*tag))
        }
        _ => None,
    }
}

fn singleton_value_branch(function: &FunctionUses<'_>, branch: &SelectedBranch) -> Option<(ValueId, Operand)> {
    if !branch.early_branches.is_empty()
        || branch.and_mask.is_some()
        || !matches!(
            branch.operation,
            MachineOperation::Branch(BranchOperation::Equality {
                width: IntegerWidth::U64,
                ..
            })
        )
    {
        return None;
    }
    let [SelectedBranchInput::Value(lhs), SelectedBranchInput::Value(rhs)] = branch.inputs.as_slice() else {
        return None;
    };
    let candidates = [(*lhs, *rhs), (*rhs, *lhs)];
    candidates.into_iter().find_map(|(value, singleton)| {
        // FIXME: Reuse materialized singleton constants on AArch64 so multi-use
        //        constants can use direct x86-64 tag checks without growing AArch64.
        if value_use_count(function, singleton) != 1 {
            return None;
        }
        let ValueDefinition::Constant(Constant::Layout(singleton)) = &function.values[singleton.0].definition else {
            return None;
        };
        if ![
            KnownLayoutConstant::EmptyValue,
            KnownLayoutConstant::UndefinedValue,
            KnownLayoutConstant::NullValue,
        ]
        .into_iter()
        .any(|known| singleton.is(known))
        {
            return None;
        }
        Some((value, Operand::LayoutConstant(*singleton)))
    })
}

fn integer_constant(function: &Function, value: ValueId) -> Option<i64> {
    match function.values[value.0].definition {
        ValueDefinition::Constant(Constant::Integer(value)) => Some(value),
        _ => None,
    }
}

fn lower_edge_copies(
    function: &FunctionUses<'_>,
    edge: &Edge,
    output: &mut Vec<Instruction>,
    temporary_index: &mut usize,
) -> Result<(), String> {
    let parameters = &function.blocks[edge.block.0].parameters;
    if parameters.len() != edge.arguments.len() {
        return Err(format!("edge to {:?} has the wrong argument count", edge.block));
    }
    let mut copies = parameters
        .iter()
        .zip(&edge.arguments)
        .filter(|(destination, source)| destination != source)
        .map(|(destination, source)| {
            Ok((
                value_operand(function, *destination)?,
                value_operand(function, *source)?,
                function.values[destination.0].ty.clone(),
            ))
        })
        .collect::<Result<Vec<_>, String>>()?;
    copies.retain(|(destination, source, _)| destination != source);
    while !copies.is_empty() {
        if let Some(index) = copies
            .iter()
            .position(|(destination, _, _)| !copies.iter().any(|(_, source, _)| source == destination))
        {
            let (destination, source, ty) = copies.remove(index);
            output.push(copy_instruction(destination, source, &ty));
            continue;
        }
        let destination = copies[0].0.clone();
        let ty = copies[0].2.clone();
        let temporary = Operand::VirtualRegister(VirtualRegister::with_class(
            format!("ssa_edge_temporary_{}", *temporary_index),
            if is_fpr(&ty) {
                RegisterClass::FloatingPoint
            } else {
                RegisterClass::GeneralPurpose
            },
        ));
        *temporary_index += 1;
        output.push(copy_instruction(temporary.clone(), destination.clone(), &ty));
        for (_, source, _) in &mut copies {
            if *source == destination {
                *source = temporary.clone();
            }
        }
    }
    Ok(())
}

fn copy_instruction(destination: Operand, source: Operand, ty: &Type) -> Instruction {
    machine_instruction(
        if is_fpr(ty) {
            MachineOperation::FloatMove
        } else {
            MachineOperation::Move(IntegerWidth::U64)
        },
        vec![destination, source],
    )
}

fn block_label(block: BlockId) -> Label {
    // Block labels are function-local and depend only on the block index, even
    // though this cache outlives each function lowered on the thread.
    thread_local! {
        static LABELS: std::cell::RefCell<Vec<Label>> = const { std::cell::RefCell::new(Vec::new()) };
    }
    LABELS.with(|labels| {
        let mut labels = labels.borrow_mut();
        while labels.len() <= block.0 {
            let next = labels.len();
            labels.push(Label::new(format!(".ssa_block_{next}")));
        }
        labels[block.0].clone()
    })
}

fn lower_memory_operation(
    operation: MemoryOperation,
    results: &[Operand],
    inputs: &[Operand],
    output: &mut Vec<Instruction>,
) -> Result<(), String> {
    if operation == MemoryOperation::Load64NonZero {
        let ([destination], [address]) = (results, inputs) else {
            return Err("'load64_nonzero' requires one result and one address".to_string());
        };
        emit!(output; MachineOperation::load(MemoryWidth::DoubleWord, false) => [destination.clone(), address.clone()];);
        emit!(output; MachineOperation::Assertion(AssertionOperation::NonZero) => [destination.clone()];);
    } else {
        output.push(machine_instruction(
            MachineOperation::Memory(operation),
            results.iter().chain(inputs).cloned().collect(),
        ));
    }
    Ok(())
}

fn lower_field_access_pair(
    function: &FunctionUses<'_>,
    first_id: InstructionId,
    second_id: InstructionId,
    output: &mut Vec<Instruction>,
) -> Result<bool, String> {
    let first = &function.instructions[first_id.0];
    let second = &function.instructions[second_id.0];
    let (Operation::FieldAccess(first_access), Operation::FieldAccess(second_access)) =
        (&first.operation, &second.operation)
    else {
        return Ok(false);
    };
    let (Some(first_pair), Some(second_pair)) = (first_access.pair, second_access.pair) else {
        return Ok(false);
    };
    if first_access.kind != second_access.kind
        || first_pair.id != second_pair.id
        || first_pair.order > 1
        || second_pair.order != 1 - first_pair.order
    {
        return Ok(false);
    }
    let Some(operation) = first_access.kind.paired_operation() else {
        return Ok(false);
    };
    let operands = |instruction: &crate::ssa::Instruction| {
        instruction
            .results
            .iter()
            .chain(&instruction.inputs)
            .map(|value| value_operand(function, *value))
            .collect::<Result<Vec<_>, _>>()
    };
    let first_operands = operands(first)?;
    let second_operands = operands(second)?;
    let (first_operands, second_operands) = if first_pair.order == 0 {
        (first_operands, second_operands)
    } else {
        (second_operands, first_operands)
    };
    let ([first_result, first_input], [second_result, second_input]) =
        (first_operands.as_slice(), second_operands.as_slice())
    else {
        return Err("paired field access requires two operands".to_string());
    };
    output.push(machine_instruction(
        MachineOperation::Memory(operation),
        vec![
            first_result.clone(),
            second_result.clone(),
            first_input.clone(),
            second_input.clone(),
        ],
    ));
    if first_access.kind.nonzero() {
        for result in [first_result, second_result] {
            emit!(output; MachineOperation::Assertion(AssertionOperation::NonZero) => [result.clone()];);
        }
    }
    Ok(true)
}

/// The SSA results of an instruction, whose count semantic analysis has
/// already checked against the operation's signature.
fn require_results<'a, T, const COUNT: usize>(results: &'a [T], what: &str) -> Result<&'a [T; COUNT], String> {
    results
        .try_into()
        .map_err(|_| format!("'{what}' requires {COUNT} SSA result(s)"))
}

/// The SSA inputs of an instruction, whose count semantic analysis has
/// already checked against the operation's signature.
fn require_inputs<'a, T, const COUNT: usize>(inputs: &'a [T], what: &str) -> Result<&'a [T; COUNT], String> {
    inputs
        .try_into()
        .map_err(|_| format!("'{what}' requires {COUNT} SSA input(s)"))
}

fn lower_instruction(
    function: &FunctionUses<'_>,
    instruction_id: InstructionId,
    instruction: &super::ir::Instruction,
    output: &mut Vec<Instruction>,
) -> Result<(), String> {
    let inputs = instruction
        .inputs
        .iter()
        .map(|value| value_operand(function, *value))
        .collect::<Result<Vec<_>, _>>()?;
    let results = instruction
        .results
        .iter()
        .map(|value| value_operand(function, *value))
        .collect::<Result<Vec<_>, _>>()?;
    match &instruction.operation {
        Operation::FieldAccess(access) => {
            lower_memory_operation(access.kind.memory_operation(), &results, &inputs, output)?;
        }
        Operation::Intrinsic(Intrinsic::Value(ValueOperation::ExtractTag { rematerialized: false })) => {
            let [destination] = require_results(&results, "extract_tag")?;
            let [value] = require_inputs(&inputs, "extract_tag")?;
            emit!(output; MachineOperation::ExtractTag => [destination.clone(), value.clone()];);
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Copy)) => {
            let [Operand::BytecodeField(destination), Operand::BytecodeField(source)] = inputs.as_slice() else {
                return Err("'copy_operand' requires two bytecode fields".to_string());
            };
            let destination_index_register =
                VirtualRegister::new(format!("ssa_move_operand_{}_destination_index", instruction_id.0));
            let source_index_register =
                VirtualRegister::new(format!("ssa_move_operand_{}_source_index", instruction_id.0));
            let destination_index = Operand::VirtualRegister(destination_index_register.clone());
            let source_index = Operand::VirtualRegister(source_index_register.clone());
            let value = Operand::VirtualRegister(format!("ssa_move_operand_{}_value", instruction_id.0).into());
            emit!(output; MachineOperation::load_pair(PairWidth::Word) => [destination_index.clone(), source_index.clone(), bytecode_field_memory(destination), bytecode_field_memory(source)];);
            emit!(output;
                MachineOperation::load(MemoryWidth::DoubleWord, false) => [value.clone(), Operand::Address(MemoryAddress {
                        base: AddressRegister::Interpreter(InterpreterRegister::Values),
                        index: Some(AddressRegister::Virtual(source_index_register)),
                        scale: Some(AddressScale::Immediate(8)),
                        displacement: None,
                    })];
            );
            emit!(output;
                MachineOperation::store(MemoryWidth::DoubleWord) => [Operand::Address(MemoryAddress {
                        base: AddressRegister::Interpreter(InterpreterRegister::Values),
                        index: Some(AddressRegister::Virtual(destination_index_register)),
                        scale: Some(AddressScale::Immediate(8)),
                        displacement: None,
                    }), value];
            );
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field))) => {
            let [destination] = require_results(&results, "load")?;
            let [Operand::BytecodeField(field)] = inputs.as_slice() else {
                return Err("'load' requires one bytecode operand field".to_string());
            };
            let index = operand_load_index(instruction_id, 0);
            emit!(output; MachineOperation::load(MemoryWidth::Word, false) => [index.clone(), bytecode_field_memory(field)];);
            emit!(output; MachineOperation::load(MemoryWidth::DoubleWord, false) => [destination.clone(), indexed_value_memory(&index)?];);
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Decode)) => {
            let [destination] = require_results(&results, "decode_operand")?;
            let [Operand::BytecodeField(field)] = inputs.as_slice() else {
                return Err("'decode_operand' requires one bytecode field".to_string());
            };
            emit!(output; MachineOperation::load(MemoryWidth::Word, false) => [destination.clone(), bytecode_field_memory(field)];);
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Load(
            OperandLoad::Index | OperandLoad::RematerializedIndex,
        ))) => {
            let [destination] = require_results(&results, "load_indexed_operand")?;
            let [index] = require_inputs(&inputs, "load_indexed_operand")?;
            emit!(output; MachineOperation::load(MemoryWidth::DoubleWord, false) => [destination.clone(), indexed_value_memory(index)?];);
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Store(OperandStore::Index))) => {
            let [index, value] = require_inputs(&inputs, "store_indexed_operand")?;
            // FIXME: A narrow Int32 payload store saves the reboxing instruction,
            //        but stalls a subsequent full Value load due to failed store
            //        forwarding on x86-64. Reconsider this when hot consumers can
            //        consistently load only the payload.
            emit!(output; MachineOperation::store(MemoryWidth::DoubleWord) => [indexed_value_memory(index)?, value.clone()];);
        }
        Operation::Intrinsic(Intrinsic::Value(ValueOperation::ValueToFloat64)) => {
            let [destination] = require_results(&results, "unbox_f64")?;
            let [source] = require_inputs(&inputs, "unbox_f64")?;
            if matches!(source, Operand::Immediate(_) | Operand::LayoutConstant(_)) {
                let temporary = Operand::VirtualRegister(format!("ssa_fp_bits_{}", instruction_id.0).into());
                emit!(output; MachineOperation::Move(IntegerWidth::U64) => [temporary.clone(), source.clone()];);
                emit!(output; MachineOperation::FloatMove => [destination.clone(), temporary];);
            } else {
                emit!(output; MachineOperation::FloatMove => [destination.clone(), source.clone()];);
            }
        }
        Operation::Intrinsic(Intrinsic::Value(ValueOperation::BoxNumber)) => {
            let [destination] = require_results(&results, "box_number")?;
            let [source] = require_inputs(&inputs, "box_number")?;
            let integer = Operand::VirtualRegister(format!("ssa_box_number_{}_integer", instruction_id.0).into());
            let check_negative_zero = Operand::Label(Label::new(format!(
                ".ssa_box_number_{}_check_negative_zero",
                instruction_id.0
            )));
            let not_integer = Operand::Label(Label::new(format!(".ssa_box_number_{}_not_integer", instruction_id.0)));
            let done = Operand::Label(Label::new(format!(".ssa_box_number_{}_done", instruction_id.0)));
            emit!(output; MachineOperation::Float(FloatingPointOperation::Convert(FloatConversion::Float64ToInt32)) => [integer.clone(), source.clone(), not_integer.clone()];);
            emit!(output; MachineOperation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero) => [integer.clone(), check_negative_zero.clone()];);
            emit!(output; MachineOperation::BoxInt32 { clean: false } => [destination.clone(), integer.clone()];);
            emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [done.clone()];);
            emit!(output; MachineOperation::Label => [check_negative_zero];);
            emit!(output; MachineOperation::FloatMove => [destination.clone(), source.clone()];);
            emit!(output; MachineOperation::branch_sign(IntegerWidth::U64, SignCondition::Negative) => [destination.clone(), not_integer.clone()];);
            emit!(output; MachineOperation::BoxInt32 { clean: false } => [destination.clone(), integer];);
            emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [done.clone()];);
            emit!(output; MachineOperation::Label => [not_integer];);
            emit!(output; MachineOperation::Float(FloatingPointOperation::CanonicalizeNan) => [destination.clone(), source.clone()];);
            emit!(output; MachineOperation::Control(ControlOperation::JumpLabel) => [done.clone()];);
            emit!(output; MachineOperation::Label => [done];);
        }
        Operation::Intrinsic(Intrinsic::LowLevel(
            operation @ (LowLevelOperation::ClearBit | LowLevelOperation::ToggleBit),
        )) => {
            let [destination] = require_results(&results, operation.name())?;
            let [source, bit] = require_inputs(&inputs, operation.name())?;
            emit!(output; MachineOperation::Move(IntegerWidth::U64) => [destination.clone(), source.clone()];);
            emit!(output;
                if *operation == LowLevelOperation::ClearBit {
                    MachineOperation::ClearBit
                } else {
                    MachineOperation::ToggleBit
                } => [destination.clone(), bit.clone()];
            );
        }
        Operation::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::Negate)) => {
            let [destination] = require_results(&results, "neg")?;
            let [source] = require_inputs(&inputs, "neg")?;
            emit!(output; MachineOperation::Move(IntegerWidth::U64) => [destination.clone(), source.clone()];);
            emit!(output; MachineOperation::Negate => [destination.clone()];);
        }
        Operation::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::BitwiseNotInt32)) => {
            let [destination] = require_results(&results, "bitwise_not_i32")?;
            let [source] = require_inputs(&inputs, "bitwise_not_i32")?;
            let source = if let Some((_, source)) = folded_bitwise_not_source(function, instruction) {
                value_operand(function, source)?
            } else {
                source.clone()
            };
            emit!(output; MachineOperation::Move(IntegerWidth::U64) => [destination.clone(), source];);
            emit!(output; MachineOperation::Not(IntegerWidth::U32) => [destination.clone()];);
        }
        Operation::Intrinsic(Intrinsic::Value(operation @ (ValueOperation::ToInt32 | ValueOperation::ToUint32))) => {
            let [destination] = require_results(&results, operation.name())?;
            let folded_boolean = folded_i32_bool_source(function, instruction);
            let source =
                if let Some((_, source)) = folded_boolean.or_else(|| folded_u32_unbox_source(function, instruction)) {
                    value_operand(function, source)?
                } else {
                    let [source] = require_inputs(&inputs, operation.name())?;
                    source.clone()
                };
            let representation_only = folded_boolean.is_none()
                && matches!(
                    (operation, &function.values[instruction.inputs[0].0].ty),
                    (ValueOperation::ToUint32, Type::U8 | Type::U16)
                        | (ValueOperation::ToInt32, Type::I8 | Type::I16 | Type::U8 | Type::U16)
                );
            emit!(output;
                if folded_boolean.is_some() {
                    MachineOperation::UnboxInt32
                } else if representation_only {
                    MachineOperation::Move(IntegerWidth::U64)
                } else {
                    MachineOperation::Move(IntegerWidth::U32)
                } => [destination.clone(), source];
            );
        }
        Operation::Intrinsic(Intrinsic::Address(AddressOperation::SequenceElement)) => {
            let [destination] = require_results(&results, "sequence_element_address")?;
            let [base, index, scale] = require_inputs(&inputs, "sequence_element_address")?;
            if let (
                Operand::VirtualRegister(base),
                Operand::VirtualRegister(index),
                Operand::Immediate(scale @ (1 | 2 | 4 | 8)),
            ) = (base, index, scale)
            {
                emit!(output;
                    MachineOperation::LoadEffectiveAddress => [destination.clone(), Operand::Address(MemoryAddress {
                            base: AddressRegister::Virtual(base.clone()),
                            index: Some(AddressRegister::Virtual(index.clone())),
                            scale: Some(AddressScale::Immediate(*scale)),
                            displacement: None,
                        })];
                );
            } else {
                emit!(output; MachineOperation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Multiply), width: IntegerWidth::U64 } => [destination.clone(), index.clone(), scale.clone()];);
                emit!(output; MachineOperation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Add), width: IntegerWidth::U64 } => [destination.clone(), base.clone()];);
            }
        }
        Operation::Intrinsic(Intrinsic::IntegerBinary(operation)) => {
            let [destination] = require_results(&results, operation.name())?;
            let [lhs, rhs] = require_inputs(&inputs, operation.name())?;
            let machine_operation = MachineOperation::IntegerBinary {
                operation: *operation,
                width: IntegerWidth::U64,
            };
            if matches!(
                machine_operation,
                MachineOperation::IntegerBinary {
                    operation: IntegerBinaryOperation::Binary(BinaryOperation::And),
                    ..
                }
            ) && function.values[instruction.results[0].0].ty == Type::ValueTag
            {
                emit!(output; MachineOperation::Move(IntegerWidth::U64) => [destination.clone(), lhs.clone()];);
                emit!(output; MachineOperation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::And), width: IntegerWidth::U16 } => [destination.clone(), rhs.clone()];);
            } else if operation.supports_narrow_result()
                && integer_result_can_stay_narrow(function, instruction.results[0])
            {
                let lhs = folded_32bit_operation_source(function, instruction.inputs[0])
                    .map(|(_, source)| value_operand(function, source))
                    .unwrap_or_else(|| Ok(lhs.clone()))?;
                let rhs = folded_shift_count(function, instruction.inputs[1])
                    .map(|(_, source)| source)
                    .unwrap_or(instruction.inputs[1]);
                let rhs = folded_32bit_operation_source(function, rhs)
                    .map(|(_, source)| value_operand(function, source))
                    .unwrap_or_else(|| value_operand(function, rhs))?;
                emit!(output; narrow_integer_operation(machine_operation) => [destination.clone(), lhs, rhs];);
            } else if *operation == IntegerBinaryOperation::Binary(BinaryOperation::Multiply) {
                emit!(output; machine_operation => [destination.clone(), lhs.clone(), rhs.clone()];);
                narrow_wide_result_to_i32(function, instruction.results[0], destination, output);
            } else {
                emit!(output; MachineOperation::Move(IntegerWidth::U64) => [destination.clone(), lhs.clone()];);
                let rhs = materialize_symbolic_logical_rhs(*operation, destination, rhs, output)?;
                emit!(output; machine_operation => [destination.clone(), rhs];);
                narrow_wide_result_to_i32(function, instruction.results[0], destination, output);
            }
        }
        Operation::Intrinsic(Intrinsic::IntegerComparison(operation)) => {
            return Err(format!(
                "comparison '{}' was not folded into control flow",
                operation.name()
            ));
        }
        Operation::Intrinsic(Intrinsic::FloatingPoint(FloatingPointOperation::Binary(operation))) => {
            let name = FloatingPointOperation::Binary(*operation).name();
            let [destination] = require_results(&results, name)?;
            let [lhs, rhs] = require_inputs(&inputs, name)?;
            emit!(output; MachineOperation::FloatMove => [destination.clone(), lhs.clone()];);
            emit!(output; MachineOperation::Float(FloatingPointOperation::Binary(*operation)) => [destination.clone(), rhs.clone()];);
        }
        Operation::Intrinsic(Intrinsic::FloatingPoint(operation)) => {
            output.push(machine_instruction(
                MachineOperation::Float(*operation),
                results.into_iter().chain(inputs).collect(),
            ));
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::LoadPair)) => {
            let [lhs, rhs] = require_results(&results, "load_operand_pair")?;
            let [Operand::BytecodeField(lhs_field), Operand::BytecodeField(rhs_field)] = inputs.as_slice() else {
                return Err("'load_operand_pair' requires two bytecode fields".to_string());
            };
            let lhs_index = operand_load_index(instruction_id, 0);
            let rhs_index = operand_load_index(instruction_id, 1);
            emit!(output; MachineOperation::load_pair(PairWidth::Word) => [lhs_index.clone(), rhs_index.clone(), bytecode_field_memory(lhs_field), bytecode_field_memory(rhs_field)];);
            emit!(output; MachineOperation::load(MemoryWidth::DoubleWord, false) => [lhs.clone(), indexed_value_memory(&lhs_index)?];);
            emit!(output; MachineOperation::load(MemoryWidth::DoubleWord, false) => [rhs.clone(), indexed_value_memory(&rhs_index)?];);
        }
        Operation::Intrinsic(Intrinsic::Control(ControlOperation::JumpBytecode)) => {
            let [target] = require_inputs(&inputs, "jump")?;
            if matches!(target, Operand::BytecodeField(_)) {
                emit!(output; MachineOperation::Control(ControlOperation::JumpBytecode) => [target.clone()];);
                return Ok(());
            }
            let target = if matches!(target, Operand::VirtualRegister(_)) {
                target.clone()
            } else {
                let temporary = Operand::VirtualRegister(format!("ssa_bytecode_jump_target_{}", output.len()).into());
                emit!(output; MachineOperation::LoadLabel => [temporary.clone(), target.clone()];);
                temporary
            };
            emit!(output; MachineOperation::Control(ControlOperation::GotoHandler) => [target];);
        }
        Operation::Intrinsic(Intrinsic::Aggregate(AggregateOperation::Copy16)) => {
            let [destination, source] = require_inputs(&inputs, "copy16_field")?;
            let suffix = output.len();
            let destination_address = Operand::VirtualRegister(format!("ssa_copy16_{suffix}_destination").into());
            let source_address = Operand::VirtualRegister(format!("ssa_copy16_{suffix}_source").into());
            let low = Operand::VirtualRegister(format!("ssa_copy16_{suffix}_low").into());
            let high = Operand::VirtualRegister(format!("ssa_copy16_{suffix}_high").into());
            emit!(output; MachineOperation::LoadEffectiveAddress => [destination_address.clone(), destination.clone()];);
            emit!(output; MachineOperation::LoadEffectiveAddress => [source_address.clone(), source.clone()];);
            emit!(output; MachineOperation::load_pair(PairWidth::DoubleWord) => [low.clone(), high.clone(), offset_memory(&source_address, 0)?, offset_memory(&source_address, 8)?];);
            emit!(output; MachineOperation::store_pair(PairWidth::DoubleWord) => [offset_memory(&destination_address, 0)?, offset_memory(&destination_address, 8)?, low, high];);
        }
        Operation::Intrinsic(Intrinsic::Aggregate(AggregateOperation::Zero16)) => {
            let [destination] = require_inputs(&inputs, "zero16_field")?;
            let suffix = output.len();
            let destination_address = Operand::VirtualRegister(format!("ssa_zero16_{suffix}_destination").into());
            let zero = Operand::VirtualRegister(format!("ssa_zero16_{suffix}_zero").into());
            emit!(output; MachineOperation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::Xor), width: IntegerWidth::U64 } => [zero.clone(), zero.clone()];);
            emit!(output; MachineOperation::LoadEffectiveAddress => [destination_address.clone(), destination.clone()];);
            emit!(output; MachineOperation::store_pair(PairWidth::DoubleWord) => [offset_memory(&destination_address, 0)?, offset_memory(&destination_address, 8)?, zero.clone(), zero];);
        }
        Operation::Intrinsic(Intrinsic::Bytecode(operation @ BytecodeOperation::Load(field_width))) => {
            let [destination] = require_results(&results, operation.name())?;
            let [Operand::BytecodeField(field)] = inputs.as_slice() else {
                return Err(format!("'{}' requires a bytecode field", operation.name()));
            };
            let width = (*field_width).into();
            emit!(output; MachineOperation::load(width, false) => [destination.clone(), bytecode_field_memory(field)];);
        }
        Operation::Intrinsic(Intrinsic::Bytecode(BytecodeOperation::LoadInlineInt32)) => {
            return Err("'load_inline_int32' reached LowIR lowering".to_string());
        }
        Operation::Intrinsic(Intrinsic::Address(AddressOperation::BytecodeField)) => {
            let [destination] = require_results(&results, "bytecode_field_address")?;
            let [Operand::BytecodeField(field)] = inputs.as_slice() else {
                return Err("'bytecode_field_address' requires a bytecode field".to_string());
            };
            emit!(output; MachineOperation::LoadEffectiveAddress => [destination.clone(), bytecode_field_memory(field)];);
        }
        Operation::Intrinsic(Intrinsic::Address(AddressOperation::EmbeddedField)) => {
            let [destination] = require_results(&results, "embedded_field_address")?;
            let [address] = require_inputs(&inputs, "embedded_field_address")?;
            emit!(output; MachineOperation::LoadEffectiveAddress => [destination.clone(), address.clone()];);
        }
        Operation::Intrinsic(Intrinsic::Memory(operation)) => {
            lower_memory_operation(*operation, &results, &inputs, output)?;
        }
        Operation::Intrinsic(Intrinsic::Operand(OperandOperation::Store(OperandStore::Field))) => {
            output.push(machine_instruction(MachineOperation::StoreOperand, inputs));
        }
        Operation::Intrinsic(Intrinsic::Call(operation)) => {
            let expected_inputs = match operation {
                CallOperation::SlowPath | CallOperation::Interpreter | CallOperation::RawNative => 1,
                CallOperation::BinarySlowPath => 4,
                CallOperation::JumpSlowPath => 5,
                CallOperation::Helper => 2,
            };
            if inputs.len() != expected_inputs || results.len() != operation.result_count() {
                return Err(format!("'{}' has invalid SSA operand arity", operation.name()));
            }
            output.push(machine_instruction(
                MachineOperation::Call(*operation),
                inputs.into_iter().chain(results).collect(),
            ));
        }
        Operation::Intrinsic(Intrinsic::Branch(operation)) => {
            return Err(format!(
                "branch operation '{}' reached instruction lowering",
                operation.name()
            ));
        }
        Operation::Intrinsic(Intrinsic::Assertion(operation)) => {
            output.push(machine_instruction(MachineOperation::Assertion(*operation), inputs));
        }
        Operation::Intrinsic(Intrinsic::Value(operation)) => {
            let machine_operation = value_machine_operation(*operation)
                .ok_or_else(|| format!("value operation '{}' requires specialized lowering", operation.name()))?;
            output.push(machine_instruction(
                machine_operation,
                results.into_iter().chain(inputs).collect(),
            ));
        }
        Operation::Intrinsic(Intrinsic::LowLevel(operation)) => {
            if *operation == LowLevelOperation::DivideModulo {
                let [_quotient, remainder] = require_results(&results, operation.name())?;
                let [lhs, rhs] = require_inputs(&instruction.inputs, operation.name())?;
                let lhs = folded_32bit_operation_source(function, *lhs)
                    .map(|(_, source)| value_operand(function, source))
                    .unwrap_or_else(|| value_operand(function, *lhs))?;
                let rhs = folded_32bit_operation_source(function, *rhs)
                    .map(|(_, source)| value_operand(function, source))
                    .unwrap_or_else(|| value_operand(function, *rhs))?;
                output.push(machine_instruction(
                    MachineOperation::Modulo,
                    vec![remainder.clone(), lhs, rhs],
                ));
            } else {
                output.push(machine_instruction(
                    low_level_machine_operation(*operation),
                    results.into_iter().chain(inputs).collect(),
                ));
            }
        }
        Operation::Intrinsic(Intrinsic::CheckedInteger(operation)) => {
            return Err(format!(
                "checked operation '{}' reached instruction lowering",
                operation.name()
            ));
        }
        Operation::Intrinsic(Intrinsic::Classification(operation)) => {
            return Err(format!(
                "classification operation '{}' reached instruction lowering",
                operation.name()
            ));
        }
        Operation::Intrinsic(Intrinsic::Control(operation)) => {
            output.push(machine_instruction(MachineOperation::Control(*operation), inputs));
        }
        Operation::Parameter(parameter) => {
            return Err(format!(
                "unresolved operation parameter {} reached LowIR lowering",
                parameter.0
            ));
        }
        Operation::InlineCall(id) => {
            return Err(format!("unexpanded inline call #{} reached LowIR lowering", id.index()));
        }
        Operation::MachineAssign { destination, intrinsic } => {
            let destination = Operand::InterpreterRegister(*destination);
            let operation = match intrinsic {
                Intrinsic::Address(AddressOperation::EmbeddedField) => MachineOperation::LoadEffectiveAddress,
                Intrinsic::LowLevel(LowLevelOperation::Move)
                    if destination
                        == Operand::InterpreterRegister(crate::types::InterpreterRegister::ProgramCounter) =>
                {
                    MachineOperation::Move(IntegerWidth::U32)
                }
                Intrinsic::LowLevel(LowLevelOperation::Move) => MachineOperation::Move(IntegerWidth::U64),
                Intrinsic::Memory(operation) => {
                    lower_memory_operation(*operation, &[destination], &inputs, output)?;
                    return Ok(());
                }
                _ => return Err(format!("'{}' cannot assign a machine register", intrinsic.name())),
            };
            output.push(Instruction {
                opcode: operation,
                operands: std::iter::once(destination).chain(inputs).collect(),
            });
        }
        Operation::BlockReference(_) => {}
        Operation::Address => {}
        // A guard needs the block's label naming and its emitted position, so
        // block lowering handles it rather than this per-instruction pass.
        Operation::Guard { .. } => return Err("a guard is lowered with its block".to_string()),
    }
    Ok(())
}

fn folded_bitwise_not_source(
    function: &FunctionUses<'_>,
    instruction: &super::ir::Instruction,
) -> Option<(InstructionId, ValueId)> {
    folded_unary_source(
        function,
        instruction,
        Operation::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::BitwiseNotInt32)),
        |operation| {
            matches!(
                operation,
                Operation::Intrinsic(Intrinsic::Value(ValueOperation::UnboxInt32 { .. }))
            )
        },
    )
}

fn folded_checked_i32_source(function: &FunctionUses<'_>, value: ValueId) -> Option<(InstructionId, ValueId)> {
    if value_use_count(function, value) != 1 && !integer_result_can_stay_narrow(function, value) {
        return None;
    }
    checked_i32_unbox_source(function, value)
}

fn checked_i32_unbox_source(function: &FunctionUses<'_>, value: ValueId) -> Option<(InstructionId, ValueId)> {
    let (instruction, unbox) = defining_instruction(function, value)?;
    if unbox.operation != Operation::Intrinsic(Intrinsic::Value(ValueOperation::UnboxInt32 { rematerialized: false })) {
        return None;
    }
    let [source] = unbox.inputs.as_slice() else {
        return None;
    };
    Some((instruction, *source))
}

fn recoverable_checked_i32_unbox_source(
    function: &FunctionUses<'_>,
    value: ValueId,
    checked_block: BlockId,
) -> Option<(InstructionId, ValueId)> {
    let (instruction, source) = checked_i32_unbox_source(function, value)?;
    let Terminator::CheckedOperation {
        inputs,
        success,
        failure,
        ..
    } = function.blocks[checked_block.0].terminator.as_ref()?
    else {
        return None;
    };
    let mut reached = HashSet::default();
    let mut worklist = vec![success.block, failure.block];
    while let Some(block) = worklist.pop() {
        if !reached.insert(block) {
            continue;
        }
        worklist.extend(
            function.blocks[block.0]
                .terminator
                .iter()
                .flat_map(Terminator::successors)
                .map(|edge| edge.block),
        );
    }
    if function.users_of(source).iter().any(|user| match user {
        ValueUser::Instruction(user) if *user == instruction => false,
        ValueUser::Instruction(user)
            if function
                .position(*user)
                .is_some_and(|(block, _)| BlockId(block as usize) == checked_block) =>
        {
            false
        }
        ValueUser::Instruction(user)
            if function
                .position(*user)
                .is_some_and(|(block, _)| function.blocks[block as usize].layout == BlockLayout::Cold)
                && function.instructions[user.0].operation
                    == Operation::Intrinsic(Intrinsic::Value(ValueOperation::UnboxInt32 { rematerialized: true })) =>
        {
            false
        }
        ValueUser::Instruction(user) => function
            .position(*user)
            .is_none_or(|(block, _)| reached.contains(&BlockId(block as usize))),
        ValueUser::Terminator(block) => reached.contains(block) || *block == checked_block,
    }) {
        return None;
    }
    if !inputs.contains(&value) || success.arguments.contains(&value) {
        return None;
    }
    for (block_index, block) in function.blocks.iter().enumerate() {
        let block_id = BlockId(block_index);
        if block
            .instructions
            .iter()
            .any(|instruction| function.instructions[instruction.0].inputs.contains(&value))
        {
            return None;
        }
        let terminator = block.terminator.as_ref()?;
        if block_id != checked_block && terminator.inputs().contains(&value) {
            return None;
        }
    }
    Some((instruction, source))
}

fn folded_checked_i32_bytecode_field(
    function: &FunctionUses<'_>,
    value: ValueId,
) -> Option<(InstructionId, BytecodeFieldId)> {
    if value_use_count(function, value) != 1 {
        return None;
    }
    let (instruction, load) = defining_instruction(function, value)?;
    if load.operation != Operation::Intrinsic(Intrinsic::Bytecode(BytecodeOperation::Load(FieldWidth::U32))) {
        return None;
    }
    let [field] = load.inputs.as_slice() else {
        return None;
    };
    let ValueDefinition::FunctionParameter(index) = function.values[field.0].definition else {
        return None;
    };
    Some((instruction, BytecodeFieldId::new(index)))
}

fn folded_32bit_operation_source(function: &FunctionUses<'_>, value: ValueId) -> Option<(Vec<InstructionId>, ValueId)> {
    let mut instructions = Vec::new();
    let mut source = value;
    loop {
        if value_use_count(function, source) != 1 && !integer_result_can_stay_narrow(function, source) {
            break;
        }
        let Some((instruction, conversion)) = defining_instruction(function, source) else {
            break;
        };
        if !matches!(
            conversion.operation,
            Operation::Intrinsic(Intrinsic::Value(
                ValueOperation::UnboxInt32 { rematerialized: false } | ValueOperation::ToUint32
            ))
        ) {
            break;
        }
        let [input] = conversion.inputs.as_slice() else {
            break;
        };
        instructions.push(instruction);
        source = *input;
    }
    (!instructions.is_empty()).then_some((instructions, source))
}

fn folded_shift_count(function: &FunctionUses<'_>, value: ValueId) -> Option<(Vec<InstructionId>, ValueId)> {
    let (mut instructions, value) =
        folded_32bit_operation_source(function, value).unwrap_or_else(|| (Vec::new(), value));
    let (instruction, mask) = single_use_instruction(function, value)?;
    if mask.operation
        != Operation::Intrinsic(Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(
            BinaryOperation::And,
        )))
    {
        return None;
    }
    let [lhs, rhs] = mask.inputs.as_slice() else {
        return None;
    };
    let source = if integer_constant(function, *rhs) == Some(31) {
        *lhs
    } else if integer_constant(function, *lhs) == Some(31) {
        *rhs
    } else {
        return None;
    };
    if matches!(function.values[source.0].definition, ValueDefinition::Constant(_)) {
        return None;
    }
    instructions.push(instruction);
    Some((instructions, source))
}

fn folded_u32_unbox_source(
    function: &FunctionUses<'_>,
    instruction: &super::ir::Instruction,
) -> Option<(InstructionId, ValueId)> {
    folded_unary_source(
        function,
        instruction,
        Operation::Intrinsic(Intrinsic::Value(ValueOperation::ToUint32)),
        |operation| {
            matches!(
                operation,
                Operation::Intrinsic(Intrinsic::Value(ValueOperation::UnboxInt32 { .. }))
            )
        },
    )
}

fn folded_i32_bool_source(
    function: &FunctionUses<'_>,
    instruction: &super::ir::Instruction,
) -> Option<(InstructionId, ValueId)> {
    folded_unary_source(
        function,
        instruction,
        Operation::Intrinsic(Intrinsic::Value(ValueOperation::ToInt32)),
        |operation| *operation == Operation::Intrinsic(Intrinsic::Value(ValueOperation::UnboxBoolean)),
    )
}

fn folded_unary_source(
    function: &FunctionUses<'_>,
    instruction: &super::ir::Instruction,
    operation: Operation,
    source_matches: impl FnOnce(&Operation) -> bool,
) -> Option<(InstructionId, ValueId)> {
    if instruction.operation != operation {
        return None;
    }
    let [input] = instruction.inputs.as_slice() else {
        return None;
    };
    let (unbox_instruction, source) = defining_instruction(function, *input)?;
    if !source_matches(&source.operation) || value_use_count(function, *input) != 1 {
        return None;
    }
    let [boxed] = source.inputs.as_slice() else {
        return None;
    };
    Some((unbox_instruction, *boxed))
}

fn value_operand(function: &FunctionUses<'_>, value: ValueId) -> Result<Operand, String> {
    let value = resolve_reuse_value(function, value);
    let virtual_register = |name| {
        let class = function.values[value.0]
            .ty
            .register_class()
            .expect("materialized SSA values have a register class");
        Operand::VirtualRegister(VirtualRegister::with_class(name, class))
    };
    Ok(match &function.values[value.0].definition {
        ValueDefinition::FunctionParameter(index) => Operand::BytecodeField(BytecodeFieldId::new(*index)),
        ValueDefinition::InstructionResult { instruction, .. }
            if function.instructions[instruction.0].operation == Operation::Address =>
        {
            memory_operand(function, &function.instructions[instruction.0].inputs)?
        }
        ValueDefinition::InstructionResult { instruction, .. }
            if matches!(
                function.instructions[instruction.0].operation,
                Operation::BlockReference(_)
            ) =>
        {
            let Operation::BlockReference(target) = function.instructions[instruction.0].operation else {
                unreachable!()
            };
            if !function.instructions[instruction.0].inputs.is_empty() {
                return Err("capturing block references cannot be used as machine labels".to_string());
            }
            Operand::Label(block_label(target))
        }
        ValueDefinition::InstructionResult { .. }
        | ValueDefinition::BlockParameter { .. }
        | ValueDefinition::TerminatorResult { .. } => Operand::VirtualRegister(
            function
                .register(value)
                .ok_or_else(|| format!("SSA value {value:?} has no register class"))?
                .clone(),
        ),
        ValueDefinition::Constant(Constant::Integer(value)) => Operand::Immediate(*value),
        ValueDefinition::Constant(Constant::Layout(constant)) => Operand::LayoutConstant(*constant),
        ValueDefinition::Constant(Constant::KnownLayout(known)) => {
            return Err(format!(
                "unresolved known layout constant {} reached LowIR lowering",
                known.name()
            ));
        }
        ValueDefinition::Constant(Constant::LayoutValue(value)) => {
            return Err(format!("unresolved layout value {value:?} reached LowIR lowering"));
        }
        ValueDefinition::Constant(Constant::Symbol(name)) => {
            return Err(format!("unresolved layout constant '{name}' reached LowIR lowering"));
        }
        ValueDefinition::Constant(Constant::SlowPath(name))
        | ValueDefinition::Constant(Constant::FunctionSymbol(name)) => {
            Operand::Relocation(Relocation::function_call(name.clone()))
        }
        ValueDefinition::Constant(Constant::Operation(operation)) => {
            let operation = match operation {
                crate::intrinsic::OperationValue::Intrinsic(intrinsic) => intrinsic.name().to_string(),
                crate::intrinsic::OperationValue::InlineFunction(id) => {
                    format!("inline function #{}", id.index())
                }
            };
            return Err(format!("operation value '{operation}' remains after SSA inlining"));
        }
        ValueDefinition::Constant(Constant::MachineRegister(register)) => match register {
            crate::types::RegisterReference::Interpreter(register) => Operand::InterpreterRegister(*register),
            crate::types::RegisterReference::Named(name) => virtual_register(name.clone()),
        },
        ValueDefinition::Dead => return Err(format!("cannot select dead SSA value {value:?}")),
    })
}

fn offset_memory(base: &Operand, offset: i64) -> Result<Operand, String> {
    let base = address_register(base).ok_or_else(|| "indexed memory requires a register base".to_string())?;
    Ok(Operand::Address(MemoryAddress {
        base,
        index: None,
        scale: None,
        displacement: (offset != 0).then_some(AddressDisplacement::Immediate(offset)),
    }))
}

fn address_register(operand: &Operand) -> Option<AddressRegister> {
    match operand {
        Operand::VirtualRegister(register) => Some(AddressRegister::Virtual(register.clone())),
        Operand::InterpreterRegister(register) => Some(AddressRegister::Interpreter(*register)),
        Operand::PhysicalRegister(register) => Some(AddressRegister::Physical(*register)),
        _ => None,
    }
}

fn resolve_reuse_value(function: &Function, mut value: ValueId) -> ValueId {
    loop {
        let Some((_, instruction)) = defining_instruction(function, value) else {
            return value;
        };
        if instruction.operation != Operation::Intrinsic(Intrinsic::Value(ValueOperation::Reuse)) {
            return value;
        }
        let [source] = instruction.inputs.as_slice() else {
            return value;
        };
        value = *source;
    }
}

fn memory_operand(function: &FunctionUses<'_>, components: &[ValueId]) -> Result<Operand, String> {
    if !(1..=3).contains(&components.len()) {
        return Err(format!(
            "memory address requires one to three components, got {}",
            components.len()
        ));
    }
    let mut operands = components
        .iter()
        .map(|component| value_operand(function, *component))
        .collect::<Result<Vec<_>, _>>()?;
    let base =
        address_register(&operands.remove(0)).ok_or_else(|| "memory address base must be a register".to_string())?;
    let mut address = MemoryAddress {
        base,
        index: None,
        scale: None,
        displacement: None,
    };
    match operands.as_slice() {
        [] | [Operand::Immediate(0)] => {}
        [index] if address_register(index).is_some() => {
            address.index = address_register(index);
        }
        [Operand::Immediate(offset)] => {
            address.displacement = Some(AddressDisplacement::Immediate(*offset));
        }
        [Operand::LayoutConstant(offset)] => {
            address.displacement = Some(AddressDisplacement::LayoutConstant(*offset));
        }
        [Operand::BytecodeField(offset)] => {
            address.displacement = Some(AddressDisplacement::BytecodeField(*offset));
        }
        [Operand::Immediate(index), Operand::Immediate(scale)] => {
            address.displacement = Some(AddressDisplacement::Immediate(
                index
                    .checked_mul(*scale)
                    .ok_or_else(|| "constant sequence offset overflow".to_string())?,
            ));
        }
        [index, scale] if address_register(index).is_some() => {
            address.index = address_register(index);
            match scale {
                Operand::Immediate(1) => {}
                Operand::Immediate(scale) => address.scale = Some(AddressScale::Immediate(*scale)),
                Operand::LayoutConstant(scale) => {
                    address.scale = Some(AddressScale::LayoutConstant(*scale));
                }
                Operand::BytecodeField(displacement) => {
                    address.displacement = Some(AddressDisplacement::BytecodeField(*displacement));
                }
                other => return Err(format!("invalid memory address scale {other:?}")),
            }
        }
        _ => return Err(format!("invalid memory address components {operands:?}")),
    }
    Ok(Operand::Address(address))
}

fn narrow_integer_operation(operation: MachineOperation) -> MachineOperation {
    let MachineOperation::IntegerBinary { operation, .. } = operation else {
        unreachable!()
    };
    MachineOperation::IntegerBinary {
        operation,
        width: IntegerWidth::U32,
    }
}

fn typed_narrow_signed_branch_operation(
    function: &Function,
    operation: BranchOperation,
    inputs: &[ValueId],
) -> Option<MachineOperation> {
    if !are_i32_pair(function, inputs) {
        return None;
    }
    let BranchOperation::Ordered {
        relation,
        signedness: IntegerSignedness::Signed,
        ..
    } = operation
    else {
        return None;
    };
    Some(MachineOperation::branch_ordered(
        IntegerWidth::U32,
        relation,
        IntegerSignedness::Signed,
    ))
}

fn branch_uses_narrow_inputs(function: &Function, operation: BranchOperation, inputs: &[ValueId]) -> bool {
    are_i32_pair(function, inputs)
        && matches!(
            operation,
            BranchOperation::Ordered {
                signedness: IntegerSignedness::Signed,
                ..
            }
        )
}

fn typed_signed_comparison_uses_narrow_inputs(
    function: &Function,
    operation: IntegerComparisonOperation,
    inputs: &[ValueId],
) -> bool {
    are_i32_pair(function, inputs)
        && matches!(
            operation,
            IntegerComparisonOperation::Equal
                | IntegerComparisonOperation::NotEqual
                | IntegerComparisonOperation::Relational {
                    domain: ComparisonDomain::SignedInteger,
                    ..
                }
        )
}

fn checked_i32_operation_uses_narrow_inputs(
    function: &Function,
    operation: CheckedIntegerOperation,
    inputs: &[ValueId],
) -> bool {
    match operation {
        CheckedIntegerOperation::Negate => inputs.len() == 1 && function.values[inputs[0].0].ty == Type::I32,
        _ => are_i32_pair(function, inputs),
    }
}

fn are_i32_pair(function: &Function, inputs: &[ValueId]) -> bool {
    inputs.len() == 2 && inputs.iter().all(|input| function.values[input.0].ty == Type::I32)
}

/// Clear the top half of a 32-bit result computed at full width.
///
/// Addition, subtraction and multiplication cannot produce a narrow result
/// directly, because the overflow check they usually carry needs the wide one,
/// so they are emitted at full width and the checked form truncates afterwards.
/// An unchecked one has no such check to truncate for it, and everything that
/// consumes an `i32` is entitled to assume the top half is clear.
fn narrow_wide_result_to_i32(
    function: &Function,
    result: ValueId,
    destination: &Operand,
    output: &mut Vec<Instruction>,
) {
    if function.values[result.0].ty != Type::I32 {
        return;
    }
    emit!(output; MachineOperation::Move(IntegerWidth::U32) => [destination.clone(), destination.clone()];);
}

fn integer_result_can_stay_narrow(function: &FunctionUses<'_>, result: ValueId) -> bool {
    match function.values[result.0].ty {
        Type::U32 => true,
        Type::I32 => i32_consumers_stay_narrow(function, result, &mut HashSet::default()),
        _ => false,
    }
}

fn i32_consumers_stay_narrow(function: &FunctionUses<'_>, value: ValueId, visiting: &mut HashSet<ValueId>) -> bool {
    if !visiting.insert(value) {
        return false;
    }
    let mut found_use = false;
    for user in function.users_of(value) {
        let instruction = match user {
            ValueUser::Terminator(block) => {
                let terminator = function.blocks[block.0].terminator.as_ref().unwrap();
                let Terminator::CheckedOperation {
                    operation,
                    inputs,
                    success,
                    failure,
                    ..
                } = terminator
                else {
                    return false;
                };
                let uses_narrow_inputs = match operation {
                    Operation::Intrinsic(Intrinsic::Branch(operation)) => {
                        branch_uses_narrow_inputs(function, *operation, inputs)
                    }
                    Operation::Intrinsic(Intrinsic::CheckedInteger(operation)) => {
                        checked_i32_operation_uses_narrow_inputs(function, *operation, inputs)
                    }
                    _ => false,
                };
                if !inputs.contains(&value)
                    || success.arguments.contains(&value)
                    || failure.arguments.contains(&value)
                    || !uses_narrow_inputs
                {
                    return false;
                }
                found_use = true;
                continue;
            }
            ValueUser::Instruction(instruction) => &function.instructions[instruction.0],
        };
        found_use = true;
        match &instruction.operation {
            Operation::InlineCall(_) => return false,
            Operation::Intrinsic(Intrinsic::Value(
                ValueOperation::BoxInt32 { clean: true } | ValueOperation::ToUint32,
            )) => continue,
            Operation::Intrinsic(Intrinsic::LowLevel(LowLevelOperation::DivideModulo)) => {}
            Operation::Intrinsic(Intrinsic::IntegerBinary(operation)) if operation.supports_narrow_result() => {}
            Operation::Intrinsic(Intrinsic::IntegerComparison(operation))
                if typed_signed_comparison_uses_narrow_inputs(function, *operation, &instruction.inputs) =>
            {
                continue;
            }
            _ => return false,
        }
        if !instruction.results.iter().all(|result| {
            value_use_count(function, *result) == 0
                || matches!(function.values[result.0].ty, Type::I32 | Type::U32)
                    && (function.values[result.0].ty == Type::U32
                        || i32_consumers_stay_narrow(function, *result, visiting))
        }) {
            return false;
        }
    }
    visiting.remove(&value);
    found_use
}

fn materialize_symbolic_logical_rhs(
    operation: IntegerBinaryOperation,
    destination: &Operand,
    rhs: &Operand,
    output: &mut Vec<Instruction>,
) -> Result<Operand, String> {
    if !matches!(
        operation,
        IntegerBinaryOperation::Binary(BinaryOperation::Or) | IntegerBinaryOperation::Binary(BinaryOperation::Xor)
    ) || !matches!(rhs, Operand::LayoutConstant(_))
    {
        return Ok(rhs.clone());
    }
    let Operand::VirtualRegister(destination) = destination else {
        return Err(format!("'{}' requires a register result", operation.name()));
    };
    let temporary = Operand::VirtualRegister(format!("{destination}_rhs").into());
    emit!(output; MachineOperation::Move(IntegerWidth::U64) => [temporary.clone(), rhs.clone()];);
    Ok(temporary)
}

/// Emit the machine branch that goes to `then_label` when `condition` holds.
///
/// A branch terminator and a guard differ only in where control goes when the
/// condition fails, so both select their machine instruction here. `name` makes
/// the labels and temporaries this emits unique within the handler.
fn emit_conditional_branch(
    function: &FunctionUses<'_>,
    constants: &LayoutConstants,
    name: &str,
    block_body_start: usize,
    condition: ValueId,
    then_label: &Label,
    body: &mut Vec<Instruction>,
) -> Result<(), String> {
    let branch = selected_branch(function, condition);
    if let Some((_, lhs, rhs)) = selected_int32_pair(function, condition) {
        let failure_label = Label::new(format!(".ssa_edge_{name}_else"));
        emit!(body;
            MachineOperation::branch_tag(EqualityCondition::NotEqual) => [value_operand(function, lhs)?, layout_operand(
                    constants,
                    KnownLayoutConstant::Int32Tag,
                )?, Operand::Label(failure_label.clone())];
        );
        emit!(body;
            MachineOperation::branch_tag(EqualityCondition::Equal) => [value_operand(function, rhs)?, layout_operand(
                    constants,
                    KnownLayoutConstant::Int32Tag,
                )?, Operand::Label(then_label.clone())];
        );
        emit!(body; MachineOperation::Label => [Operand::Label(failure_label)];);
    } else if let Some(branch) = branch {
        let direct_tag_branch = branch
            .direct_tag_source
            .as_ref()
            .and_then(|source| {
                if !branch.early_branches.is_empty() || branch.and_mask.is_some() {
                    return None;
                }
                let [SelectedBranchInput::Value(lhs), SelectedBranchInput::Value(rhs)] = branch.inputs.as_slice()
                else {
                    return None;
                };
                if !matches!(
                    branch.operation,
                    MachineOperation::Branch(BranchOperation::Equality {
                        width: IntegerWidth::U64 | IntegerWidth::U16,
                        ..
                    })
                ) {
                    return None;
                }
                if *lhs == source.tag {
                    direct_tag_constant(function, *rhs).map(|tag| (source.value, tag))
                } else if *rhs == source.tag {
                    direct_tag_constant(function, *lhs).map(|tag| (source.value, tag))
                } else {
                    None
                }
            })
            .or_else(|| singleton_value_branch(function, &branch));
        if let Some(source) = branch.direct_tag_source.as_ref()
            && direct_tag_branch.is_none()
        {
            let tag = value_operand(function, source.tag)?;
            emit!(body; MachineOperation::ExtractTag => [tag, value_operand(function, source.value)?];);
        }
        for early_branch in &branch.early_branches {
            let mut operands = early_branch
                .inputs
                .iter()
                .map(|input| branch_input_operand(function, constants, input))
                .collect::<Result<Vec<_>, _>>()?;
            materialize_symbolic_branch_rhs(early_branch.operation, name, block_body_start, &mut operands, body);
            operands.push(Operand::Label(then_label.clone()));
            body.push(machine_instruction(early_branch.operation, operands));
        }
        if let Some((value, expected_tag)) = direct_tag_branch {
            let singleton = matches!(
                &expected_tag,
                Operand::LayoutConstant(constant)
                    if constant.category()
                        == LayoutConstantCategory::Value
            );
            emit!(body;
                match (
                    singleton,
                    matches!(
                        branch.operation,
                        MachineOperation::Branch(BranchOperation::Equality {
                            condition: EqualityCondition::Equal,
                            ..
                        })
                    ),
                ) {
                    (true, true) => MachineOperation::branch_singleton(EqualityCondition::Equal),
                    (true, false) => MachineOperation::branch_singleton(EqualityCondition::NotEqual),
                    (false, true) => MachineOperation::branch_tag(EqualityCondition::Equal),
                    (false, false) => MachineOperation::branch_tag(EqualityCondition::NotEqual),
                } => [value_operand(function, value)?, expected_tag, Operand::Label(then_label.clone())];
            );
        } else {
            let mut operands = branch
                .inputs
                .iter()
                .map(|input| branch_input_operand(function, constants, input))
                .collect::<Result<Vec<_>, _>>()?;
            if let Some((source, destination, mask)) = branch.and_mask {
                if source != destination {
                    let destination = value_operand(function, destination)?;
                    emit!(body; MachineOperation::Move(IntegerWidth::U64) => [destination, value_operand(function, source)?];);
                }
                emit!(body; MachineOperation::IntegerBinary { operation: IntegerBinaryOperation::Binary(BinaryOperation::And), width: IntegerWidth::U16 } => [value_operand(function, destination)?, layout_operand(constants, mask)?];);
            }
            materialize_symbolic_branch_rhs(branch.operation, name, block_body_start, &mut operands, body);
            operands.push(Operand::Label(then_label.clone()));
            body.push(machine_instruction(branch.operation, operands));
        }
    } else {
        emit!(body; MachineOperation::branch_zero(IntegerWidth::U64, ZeroCondition::NonZero) => [value_operand(function, condition)?, Operand::Label(then_label.clone())];);
    }

    Ok(())
}

fn materialize_symbolic_branch_rhs(
    operation: MachineOperation,
    name: &str,
    block_body_start: usize,
    operands: &mut [Operand],
    output: &mut Vec<Instruction>,
) {
    if matches!(operation, MachineOperation::Branch(BranchOperation::Bits { .. })) {
        return;
    }
    let [_, rhs] = operands else {
        return;
    };
    if !matches!(rhs, Operand::LayoutConstant(_))
        || is_direct_tag_constant(rhs)
        || is_direct_i32_constant(rhs)
        || (matches!(
            operation,
            MachineOperation::Branch(BranchOperation::Equality {
                width: IntegerWidth::U32,
                ..
            })
        ) && is_direct_u32_constant(rhs))
    {
        return;
    }
    if let Some(register) = output[block_body_start..].iter().rev().find_map(|instruction| {
        let [Operand::VirtualRegister(register), source] = instruction.operands.as_slice() else {
            return None;
        };
        (instruction.opcode.operation() == MachineOperation::Move(IntegerWidth::U64)
            && register.is_ssa()
            && source == rhs)
            .then(|| Operand::VirtualRegister(register.clone()))
    }) {
        *rhs = register;
        return;
    }
    let temporary = Operand::VirtualRegister(format!("ssa_branch_{name}_rhs").into());
    emit!(output; MachineOperation::Move(IntegerWidth::U64) => [temporary.clone(), rhs.clone()];);
    *rhs = temporary;
}

fn is_direct_i32_constant(operand: &Operand) -> bool {
    matches!(operand, Operand::LayoutConstant(constant) if i32::try_from(constant.value()).is_ok())
}

fn is_direct_u32_constant(operand: &Operand) -> bool {
    matches!(operand, Operand::LayoutConstant(constant) if u32::try_from(constant.value()).is_ok())
}

fn is_direct_tag_constant(operand: &Operand) -> bool {
    matches!(
        operand,
        Operand::LayoutConstant(constant)
            if constant.category() == LayoutConstantCategory::Tag
    )
}

fn value_machine_operation(operation: ValueOperation) -> Option<MachineOperation> {
    Some(match operation {
        ValueOperation::Reuse
        | ValueOperation::AssumeInt32
        | ValueOperation::AssumeUint32
        | ValueOperation::TruncateUint8
        | ValueOperation::TruncateUint16
        | ValueOperation::ReinterpretUint64AsValue => MachineOperation::Move(IntegerWidth::U64),
        ValueOperation::BoxInt32 { clean } => MachineOperation::BoxInt32 { clean },
        ValueOperation::BoxFloat64 | ValueOperation::ValueToFloat64 => MachineOperation::FloatMove,
        ValueOperation::UnboxInt32 { .. } | ValueOperation::UnboxBoolean => MachineOperation::UnboxInt32,
        ValueOperation::ExtractTag { .. } => MachineOperation::ExtractTag,
        ValueOperation::ToInt32 | ValueOperation::ToUint32 => MachineOperation::Move(IntegerWidth::U32),
        ValueOperation::UnboxObject => MachineOperation::UnboxObject,
        ValueOperation::BoxNumber | ValueOperation::LogicalNot => return None,
    })
}

fn low_level_machine_operation(operation: LowLevelOperation) -> MachineOperation {
    match operation {
        LowLevelOperation::LoadLabel => MachineOperation::LoadLabel,
        LowLevelOperation::DivideModulo => MachineOperation::Modulo,
        LowLevelOperation::Move => MachineOperation::Move(IntegerWidth::U64),
        LowLevelOperation::LoadEffectiveAddress => MachineOperation::LoadEffectiveAddress,
        LowLevelOperation::LoadVm => MachineOperation::LoadVm,
        LowLevelOperation::Increment32Memory => MachineOperation::Increment32Memory,
        LowLevelOperation::ClearBit => MachineOperation::ClearBit,
        LowLevelOperation::ToggleBit => MachineOperation::ToggleBit,
        LowLevelOperation::Negate => MachineOperation::Negate,
        LowLevelOperation::BitwiseNotInt32 => MachineOperation::Not(IntegerWidth::U32),
        LowLevelOperation::Add => MachineOperation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(BinaryOperation::Add),
            width: IntegerWidth::U64,
        },
        LowLevelOperation::Subtract => MachineOperation::IntegerBinary {
            operation: IntegerBinaryOperation::Binary(BinaryOperation::Subtract),
            width: IntegerWidth::U64,
        },
    }
}

fn machine_instruction(operation: MachineOperation, operands: Vec<Operand>) -> Instruction {
    Instruction {
        opcode: operation,
        operands,
    }
}

fn bytecode_field_memory(field: &BytecodeFieldId) -> Operand {
    Operand::Address(MemoryAddress {
        base: AddressRegister::Interpreter(InterpreterRegister::ProgramBase),
        index: Some(AddressRegister::Interpreter(InterpreterRegister::ProgramCounter)),
        scale: None,
        displacement: Some(AddressDisplacement::BytecodeField(*field)),
    })
}

fn indexed_value_memory(index: &Operand) -> Result<Operand, String> {
    let Operand::VirtualRegister(index) = index else {
        return Err("loaded operand index must be an SSA register".to_string());
    };
    Ok(Operand::Address(MemoryAddress {
        base: AddressRegister::Interpreter(InterpreterRegister::Values),
        index: Some(AddressRegister::Virtual(index.clone())),
        scale: Some(AddressScale::Immediate(8)),
        displacement: None,
    }))
}

fn operand_load_index(instruction: InstructionId, result_index: usize) -> Operand {
    Operand::VirtualRegister(format!("ssa_operand_load_{}_{}_index", instruction.0, result_index).into())
}

fn is_fpr(ty: &Type) -> bool {
    ty.register_class() == Some(RegisterClass::FloatingPoint)
}

fn layout_operand(constants: &LayoutConstants, known: KnownLayoutConstant) -> Result<Operand, String> {
    constants
        .known(known)
        .map(Operand::LayoutConstant)
        .ok_or_else(|| format!("unknown constant '{}'", known.name()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rejects_materialized_values_without_a_register_class() {
        let mut function = Function::new("invalid", Vec::new(), Vec::new());
        let value = function.append_instruction(
            function.entry,
            Intrinsic::LowLevel(LowLevelOperation::Negate),
            Vec::new(),
            vec![Type::Tuple(Vec::new())],
        )[0];
        let function = FunctionUses::new(&function, crate::identity::HandlerId::new(0));

        assert_eq!(
            value_operand(&function, value).unwrap_err(),
            format!("SSA value {value:?} has no register class")
        );
    }

    #[test]
    fn keeps_i32_values_narrow_when_only_a_checked_terminator_uses_them() {
        let mut function = Function::new("narrow", vec![Type::I32, Type::I32], Vec::new());
        let value = function.append_instruction(
            function.entry,
            Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)),
            vec![function.parameter(0), function.parameter(1)],
            vec![Type::I32],
        )[0];
        let success = function.create_named_block("success", BlockLayout::Hot, vec![Type::I32]);
        let failure = function.create_empty_block("failure", BlockLayout::Cold);
        function.set_checked_operation(
            function.entry,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![value, function.parameter(1)],
            vec![Type::I32],
            crate::ssa::Effects::PURE,
            success,
            Vec::new(),
            Edge::new(failure),
        );
        let function = FunctionUses::new(&function, crate::identity::HandlerId::new(0));

        assert!(integer_result_can_stay_narrow(&function, value));
    }

    #[test]
    fn does_not_recover_a_checked_unbox_with_a_live_source() {
        let mut function = Function::new("live-source", vec![Type::Value, Type::Value], Vec::new());
        let lhs = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::UnboxInt32 { rematerialized: false }),
            vec![function.parameter(0)],
            vec![Type::I32],
        )[0];
        let rhs = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::UnboxInt32 { rematerialized: false }),
            vec![function.parameter(1)],
            vec![Type::I32],
        )[0];
        let success = function.create_named_block("success", BlockLayout::Hot, vec![Type::I32]);
        let failure = function.create_empty_block("failure", BlockLayout::Cold);
        function.set_checked_operation(
            function.entry,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![lhs, rhs],
            vec![Type::I32],
            crate::ssa::Effects::PURE,
            success,
            Vec::new(),
            Edge::new(failure),
        );
        function.append_instruction(
            success,
            Intrinsic::Value(ValueOperation::ExtractTag { rematerialized: false }),
            vec![function.parameter(0)],
            vec![Type::ValueTag],
        );
        function.set_terminator(success, Terminator::Return(Vec::new()));
        function.set_terminator(failure, Terminator::Return(Vec::new()));
        let function = FunctionUses::new(&function, crate::identity::HandlerId::new(0));

        assert!(recoverable_checked_i32_unbox_source(&function, lhs, function.entry).is_none());
        assert!(recoverable_checked_i32_unbox_source(&function, rhs, function.entry).is_some());
    }

    #[test]
    fn folds_checked_multiply_unboxes_without_clobbering_failure_inputs() {
        let mut function = Function::new("multiply", vec![Type::Value, Type::Value], Vec::new());
        let lhs = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::UnboxInt32 { rematerialized: false }),
            vec![function.parameter(0)],
            vec![Type::I32],
        )[0];
        let rhs = function.append_instruction(
            function.entry,
            Intrinsic::Value(ValueOperation::UnboxInt32 { rematerialized: false }),
            vec![function.parameter(1)],
            vec![Type::I32],
        )[0];
        let success = function.create_named_block("success", BlockLayout::Hot, vec![Type::I32]);
        let failure = function.create_named_block("failure", BlockLayout::Hot, vec![Type::I32]);
        function.set_checked_operation(
            function.entry,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Multiply),
            vec![lhs, rhs],
            vec![Type::I32],
            crate::ssa::Effects::PURE,
            success,
            Vec::new(),
            Edge::with_arguments(failure, vec![lhs]),
        );
        let result = function.blocks[success.0].parameters[0];
        function.append_instruction(
            success,
            Intrinsic::Value(ValueOperation::BoxInt32 { clean: true }),
            vec![result],
            vec![Type::Value],
        );
        function.set_terminator(success, Terminator::Return(Vec::new()));
        function.set_terminator(failure, Terminator::Return(Vec::new()));
        let function = FunctionUses::new(&function, crate::identity::HandlerId::new(0));

        let handler = lower_handler_internal(
            &function,
            false,
            &LayoutConstants::default(),
            crate::identity::HandlerId::new(0),
        )
        .unwrap();
        let multiply = handler
            .instructions
            .iter()
            .find(|instruction| instruction.opcode == MachineOperation::Overflow(OverflowOperation::MultiplyCopy))
            .expect("expected a checked multiplication");

        assert!(
            !handler
                .instructions
                .iter()
                .any(|instruction| instruction.opcode == MachineOperation::UnboxInt32)
        );
        assert_ne!(multiply.operands[0], multiply.operands[1]);
    }

    #[test]
    fn preserves_an_unverified_lhs_with_a_checked_bytecode_field_rhs() {
        let mut function = Function::new("field-rhs", vec![Type::I32, Type::I32, Type::InlineInt32], Vec::new());
        let lhs = function.append_instruction(
            function.entry,
            Intrinsic::IntegerBinary(IntegerBinaryOperation::Binary(BinaryOperation::Add)),
            vec![function.parameter(0), function.parameter(1)],
            vec![Type::I32],
        )[0];
        let rhs = function.append_instruction(
            function.entry,
            Intrinsic::Bytecode(BytecodeOperation::Load(FieldWidth::U32)),
            vec![function.parameter(2)],
            vec![Type::I32],
        )[0];
        let success = function.create_named_block("success", BlockLayout::Hot, vec![Type::I32, Type::I32]);
        let failure = function.create_empty_block("failure", BlockLayout::Hot);
        function.set_checked_operation(
            function.entry,
            Intrinsic::CheckedInteger(CheckedIntegerOperation::Add),
            vec![lhs, rhs],
            vec![Type::I32],
            crate::ssa::Effects::PURE,
            success,
            vec![lhs],
            Edge::new(failure),
        );
        function.set_terminator(success, Terminator::Return(Vec::new()));
        function.set_terminator(failure, Terminator::Return(Vec::new()));
        let function = FunctionUses::new(&function, crate::identity::HandlerId::new(0));

        let handler = lower_handler_internal(
            &function,
            false,
            &LayoutConstants::default(),
            crate::identity::HandlerId::new(0),
        )
        .unwrap();
        let checked_add = handler
            .instructions
            .iter()
            .find(|instruction| instruction.opcode == MachineOperation::Overflow(OverflowOperation::AddWithOverflow))
            .expect("expected a checked addition");

        assert!(handler.instructions.iter().any(|instruction| {
            instruction.opcode == MachineOperation::Move(IntegerWidth::U64)
                && instruction.operands.first() == checked_add.operands.first()
        }));
    }

    #[test]
    fn reorders_lowered_block_fragments_without_splitting_internal_labels() {
        let first = BlockId(0);
        let second = BlockId(1);
        let instructions = vec![
            machine_instruction(MachineOperation::Move(IntegerWidth::U64), vec![]),
            machine_instruction(MachineOperation::Label, vec![Operand::Label(block_label(second))]),
            machine_instruction(MachineOperation::Label, vec![Operand::Label(Label::new(".internal"))]),
        ];
        let fragments = LoweredBlockFragments::new(&instructions, &[first, second]).unwrap();
        let reordered = fragments.reorder(&instructions, &[second, first]);

        assert_eq!(reordered[0].operands, [Operand::Label(block_label(second))]);
        assert_eq!(reordered[1].operands, [Operand::Label(Label::new(".internal"))]);
        assert_eq!(
            reordered[2].opcode.operation(),
            MachineOperation::Move(IntegerWidth::U64)
        );
    }
}
