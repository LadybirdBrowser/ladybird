/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{
    ControlFlowOpcode, ControlFlowOperand, Instruction, Label, Operand,
};
use crate::target::description::{ControlOperation, Operation};
#[cfg(test)]
use crate::target::description::{CallKind, EqualityCondition, IntegerWidth, PairWidth, ZeroCondition};
use std::collections::{BTreeSet, HashMap, HashSet};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct BlockId(usize);

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum EdgeKind {
    Branch,
    Fallthrough,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) struct Edge {
    pub(crate) target: BlockId,
    pub(crate) kind: EdgeKind,
}

#[derive(Debug, Clone)]
pub(crate) struct BasicBlock<O, C> {
    pub(crate) instructions: Vec<Instruction<O, C>>,
    pub(crate) successors: Vec<Edge>,
    pub(crate) is_cold: bool,
}

#[derive(Debug, Clone, PartialEq, Eq)]
pub(crate) struct BlockLayout {
    pub(crate) hot: Vec<BlockId>,
    pub(crate) cold: Vec<BlockId>,
}

#[derive(Debug, Clone)]
pub(crate) struct ControlFlowGraph<O = Operand, C = Operation> {
    blocks: Vec<BasicBlock<O, C>>,
}

pub(crate) struct LinearControlFlow<O, C> {
    pub(crate) instructions: Vec<Instruction<O, C>>,
    pub(crate) successors: Vec<Vec<usize>>,
}

impl<O: ControlFlowOperand, C: ControlFlowOpcode> ControlFlowGraph<O, C> {
    pub(crate) fn from_instructions(
        instructions: Vec<Instruction<O, C>>,
    ) -> Result<Self, String> {
        let cold_labels = collect_cold_labels(&instructions)?;
        let instructions: Vec<_> = instructions
            .into_iter()
            .filter(|instruction| instruction.opcode.operation() != Operation::Cold)
            .collect();
        if instructions.is_empty() {
            return Ok(Self { blocks: Vec::new() });
        }

        let mut block_starts = BTreeSet::from([0]);
        for (index, instruction) in instructions.iter().enumerate() {
            if instruction.opcode.operation() == Operation::Label {
                block_starts.insert(index);
            }
            if instruction_ends_block(instruction) && index + 1 < instructions.len() {
                block_starts.insert(index + 1);
            }
        }

        let block_starts: Vec<_> = block_starts.into_iter().collect();
        let mut blocks = Vec::with_capacity(block_starts.len());
        let mut instruction_to_block = vec![BlockId(0); instructions.len()];
        for (block_index, &start) in block_starts.iter().enumerate() {
            let end = block_starts.get(block_index + 1).copied().unwrap_or(instructions.len());
            instruction_to_block[start..end].fill(BlockId(block_index));
            blocks.push(BasicBlock {
                instructions: instructions[start..end].to_vec(),
                successors: Vec::new(),
                is_cold: false,
            });
        }

        let labels = instructions
            .iter()
            .enumerate()
            .filter_map(|(index, instruction)| {
                defined_label(instruction).map(|label| (label.clone(), instruction_to_block[index]))
            })
            .collect::<HashMap<_, _>>();

        for label in cold_labels {
            let block_id = labels
                .get(&label)
                .copied()
                .ok_or_else(|| format!("cold annotation references missing label '{label}'"))?;
            for block in blocks.iter_mut().skip(block_id.0) {
                if block.is_cold
                    || (block.instructions[0].opcode.operation() == Operation::Label
                        && sole_label_operand(&block.instructions[0]) != Some(&label))
                {
                    break;
                }
                block.is_cold = true;
            }
        }

        let mut graph = Self { blocks };
        graph.rebuild_successors();
        Ok(graph)
    }

    pub(crate) fn layout_hot_and_cold(&self) -> Result<BlockLayout, String> {
        let mut hot = Vec::new();
        let mut cold = Vec::new();
        let mut index = 0;
        while index < self.blocks.len() {
            let block = &self.blocks[index];
            let block_id = BlockId(index);
            if !block.is_cold {
                hot.push(block_id);
                index += 1;
                continue;
            }
            let label = block
                .instructions
                .first()
                .and_then(defined_label)
                .map(Label::as_str)
                .unwrap_or("<unnamed>");
            if index == 0 {
                return Err(format!("handler entry block '{label}' cannot be cold"));
            }
            let previous_instruction = self.blocks[index - 1]
                .instructions
                .last()
                .expect("basic block must contain an instruction");
            if !previous_instruction.opcode.description().terminal {
                return Err(format!("cold block '{label}' receives fallthrough control flow"));
            }
            while index < self.blocks.len() && self.blocks[index].is_cold {
                cold.push(BlockId(index));
                index += 1;
            }
            let last_instruction = self.blocks[index - 1]
                .instructions
                .last()
                .expect("basic block must contain an instruction");
            if !last_instruction.opcode.description().terminal {
                return Err(format!(
                    "cold block '{label}' produces fallthrough control flow after '{:?}'",
                    last_instruction.opcode
                ));
            }
        }
        Ok(BlockLayout { hot, cold })
    }

    pub(crate) fn single_instruction(&self) -> Option<&Instruction<O, C>> {
        let [block] = self.blocks.as_slice() else {
            return None;
        };
        let [instruction] = block.instructions.as_slice() else {
            return None;
        };
        Some(instruction)
    }

    pub(crate) fn thread_unconditional_jumps(&mut self) {
        let redirects = self
            .blocks
            .iter()
            .filter_map(|block| match block.instructions.as_slice() {
                [label, jump] if jump.opcode.operation() == Operation::Control(ControlOperation::JumpLabel) => {
                    Some((defined_label(label)?.clone(), sole_label_operand(jump)?.clone()))
                }
                _ => None,
            })
            .collect::<HashMap<_, _>>();

        for block in &mut self.blocks {
            for instruction in &mut block.instructions {
                if instruction.opcode.operation() == Operation::Label {
                    continue;
                }
                for operand in &mut instruction.operands {
                    let Some(target) = operand.label_mut() else {
                        continue;
                    };
                    let mut visited = HashSet::new();
                    while visited.insert(target.clone()) {
                        let Some(redirect) = redirects.get(target) else {
                            break;
                        };
                        *target = redirect.clone();
                    }
                }
            }
        }
        self.rebuild_successors();
    }

    pub(crate) fn remove_unreferenced_jump_blocks(&mut self) {
        loop {
            let referenced_labels = self
                .blocks
                .iter()
                .flat_map(|block| &block.instructions)
                .filter(|instruction| instruction.opcode.operation() != Operation::Label)
                .flat_map(|instruction| &instruction.operands)
                .filter_map(ControlFlowOperand::label)
                .cloned()
                .collect::<HashSet<_>>();
            let removable_block = self.blocks.iter().enumerate().find_map(|(index, block)| {
                let [label, jump] = block.instructions.as_slice() else {
                    return None;
                };
                let label = defined_label(label)?;
                let previous = index.checked_sub(1).and_then(|index| self.blocks.get(index))?;
                let previous_is_terminal = previous
                    .instructions
                    .last()
                    .is_some_and(|instruction| instruction.opcode.description().terminal);
                (jump.opcode.operation() == Operation::Control(ControlOperation::JumpLabel) && previous_is_terminal && !referenced_labels.contains(label)).then_some(index)
            });
            let Some(index) = removable_block else {
                break;
            };
            self.blocks.remove(index);
        }
        self.rebuild_successors();
    }

    pub(crate) fn remove_unreachable_blocks(&mut self) {
        if self.blocks.is_empty() {
            return;
        }

        let mut reachable = HashSet::new();
        let mut pending = vec![BlockId(0)];
        while let Some(block) = pending.pop() {
            if !reachable.insert(block) {
                continue;
            }
            pending.extend(self.blocks[block.0].successors.iter().map(|edge| edge.target));
        }

        let mut index = 0usize;
        self.blocks.retain(|_| {
            let keep = reachable.contains(&BlockId(index));
            index += 1;
            keep
        });
        self.rebuild_successors();
    }

    pub(crate) fn linearize_omitting_jumps_to_next_block(
        &self,
        blocks: &[BlockId],
    ) -> Vec<Instruction<O, C>> {
        let inverted_branches = blocks
            .iter()
            .enumerate()
            .filter_map(|(index, &block_id)| {
                let block = &self.blocks[block_id.0];
                let next_label = blocks
                    .get(index + 1)
                    .and_then(|next| self.blocks[next.0].instructions.first())
                    .and_then(defined_label)?;
                let [.., branch, jump] = block.instructions.as_slice() else {
                    return None;
                };
                let inverted = super::optimize::inverted_branch_operation(&branch.opcode)?;
                let branch_target = branch.operands.last().and_then(ControlFlowOperand::label)?;
                let jump_target = (jump.opcode.operation() == Operation::Control(ControlOperation::JumpLabel))
                    .then(|| sole_label_operand(jump))
                    .flatten()?;
                if branch_target != next_label {
                    return None;
                }
                let mut branch = branch.clone();
                branch.opcode = branch.opcode.replacing_operation(inverted);
                *branch.operands.last_mut().unwrap() = O::from_label(jump_target.clone());
                Some((block_id, (2, branch)))
            })
            .collect::<HashMap<_, _>>();
        let explicitly_referenced_labels = self
            .blocks
            .iter()
            .flat_map(|block| &block.instructions)
            .filter(|instruction| instruction.opcode.operation() != Operation::Label)
            .flat_map(|instruction| &instruction.operands)
            .filter_map(ControlFlowOperand::label)
            .cloned()
            .collect::<HashSet<_>>();
        let mut inverted_branches = inverted_branches;
        let mut skipped_blocks = HashSet::new();
        for blocks in blocks.windows(3) {
            let [source_id, fallthrough_id, target_id] = blocks else {
                unreachable!();
            };
            if inverted_branches.contains_key(source_id) {
                continue;
            }
            let source = &self.blocks[source_id.0];
            let fallthrough = &self.blocks[fallthrough_id.0];
            let target = &self.blocks[target_id.0];
            let Some(branch) = source.instructions.last() else {
                continue;
            };
            let Some(inverted) = super::optimize::inverted_branch_operation(&branch.opcode) else {
                continue;
            };
            let Some(branch_target) = branch.operands.last().and_then(ControlFlowOperand::label) else {
                continue;
            };
            let Some(target_label) = target.instructions.first().and_then(defined_label) else {
                continue;
            };
            if branch_target != target_label {
                continue;
            }
            let jump = match fallthrough.instructions.as_slice() {
                [jump] => jump,
                [label, jump]
                    if defined_label(label)
                        .is_some_and(|label| !explicitly_referenced_labels.contains(label)) => jump,
                _ => continue,
            };
            let Some(jump_target) = (jump.opcode.operation() == Operation::Control(ControlOperation::JumpLabel))
                .then(|| sole_label_operand(jump))
                .flatten()
            else {
                continue;
            };
            let mut branch = branch.clone();
            branch.opcode = branch.opcode.replacing_operation(inverted);
            *branch.operands.last_mut().unwrap() = O::from_label(jump_target.clone());
            inverted_branches.insert(*source_id, (1, branch));
            skipped_blocks.insert(*fallthrough_id);
        }
        let omitted_jumps = blocks
            .iter()
            .enumerate()
            .filter_map(|(index, &block_id)| {
                let block = &self.blocks[block_id.0];
                blocks.get(index + 1).is_some_and(|&next_block_id| {
                    let Some(last) = block.instructions.last() else {
                        return false;
                    };
                    let Some(target) = (last.opcode.operation() == Operation::Control(ControlOperation::JumpLabel)).then(|| sole_label_operand(last)).flatten() else {
                        return false;
                    };
                    self.blocks[next_block_id.0]
                        .instructions
                        .first()
                        .and_then(defined_label)
                        .is_some_and(|label| label == target)
                }).then_some(block_id)
            })
            .filter(|block| !inverted_branches.contains_key(block))
            .filter(|block| !skipped_blocks.contains(block))
            .collect::<HashSet<_>>();
        let mut referenced_labels = HashSet::new();
        for (block_index, block) in self.blocks.iter().enumerate() {
            let block_id = BlockId(block_index);
            if skipped_blocks.contains(&block_id) {
                continue;
            }
            let end = block.instructions.len()
                - usize::from(omitted_jumps.contains(&block_id))
                - inverted_branches.get(&block_id).map_or(0, |(removed, _)| *removed);
            for instruction in &block.instructions[..end] {
                if instruction.opcode.operation() != Operation::Label {
                    referenced_labels.extend(
                        instruction
                            .operands
                            .iter()
                            .filter_map(ControlFlowOperand::label)
                            .cloned(),
                    );
                }
            }
            if let Some((_, branch)) = inverted_branches.get(&block_id) {
                referenced_labels.extend(
                    branch
                        .operands
                        .iter()
                        .filter_map(ControlFlowOperand::label)
                        .cloned(),
                );
            }
        }
        let mut instructions = Vec::new();
        for &block_id in blocks {
            if skipped_blocks.contains(&block_id) {
                continue;
            }
            let block = &self.blocks[block_id.0];
            let end = block.instructions.len()
                - usize::from(omitted_jumps.contains(&block_id))
                - inverted_branches.get(&block_id).map_or(0, |(removed, _)| *removed);
            instructions.extend(
                block.instructions[..end]
                    .iter()
                    .filter(|instruction| {
                        defined_label(instruction)
                            .is_none_or(|label| referenced_labels.contains(label))
                    })
                    .cloned(),
            );
            if let Some((_, branch)) = inverted_branches.get(&block_id) {
                instructions.push(branch.clone());
            }
        }
        instructions
    }

    pub(crate) fn linearize_for_analysis(&self) -> LinearControlFlow<O, C> {
        let mut block_starts = Vec::with_capacity(self.blocks.len());
        let mut instruction_count = 0;
        for block in &self.blocks {
            block_starts.push(instruction_count);
            instruction_count += block.instructions.len();
        }

        let mut instructions = Vec::with_capacity(instruction_count);
        let mut successors = vec![Vec::new(); instruction_count];
        for (block_index, block) in self.blocks.iter().enumerate() {
            let block_start = block_starts[block_index];
            instructions.extend(block.instructions.iter().cloned());
            for instruction_offset in 0..block.instructions.len().saturating_sub(1) {
                let instruction_index = block_start + instruction_offset;
                successors[instruction_index].push(instruction_index + 1);
            }
            let last_instruction = block_start + block.instructions.len() - 1;
            successors[last_instruction].extend(block.successors.iter().map(|edge| block_starts[edge.target.0]));
        }
        LinearControlFlow {
            instructions,
            successors,
        }
    }

    fn rebuild_successors(&mut self) {
        let labels = self
            .blocks
            .iter()
            .enumerate()
            .filter_map(|(index, block)| {
                block
                    .instructions
                    .first()
                    .and_then(defined_label)
                    .map(|label| (label.clone(), BlockId(index)))
            })
            .collect::<HashMap<_, _>>();

        for block_index in 0..self.blocks.len() {
            let (branch_targets, terminal) = {
                let last_instruction = self.blocks[block_index]
                    .instructions
                    .last()
                    .expect("basic block must contain an instruction");
                let branch_targets = if last_instruction.opcode.operation() == Operation::Label {
                    Vec::new()
                } else {
                    last_instruction
                        .operands
                        .iter()
                        .filter_map(ControlFlowOperand::label)
                        .filter_map(|label| labels.get(label).copied())
                        .collect()
                };
                let terminal = last_instruction.opcode.description().terminal;
                (branch_targets, terminal)
            };

            let has_fallthrough = !terminal && block_index + 1 < self.blocks.len();
            let block = &mut self.blocks[block_index];
            block.successors.clear();
            block.successors.extend(branch_targets.into_iter().map(|target| Edge {
                target,
                kind: EdgeKind::Branch,
            }));
            if has_fallthrough {
                block.successors.push(Edge {
                    target: BlockId(block_index + 1),
                    kind: EdgeKind::Fallthrough,
                });
            }
        }
    }
}

fn defined_label<O: ControlFlowOperand, C: ControlFlowOpcode>(
    instruction: &Instruction<O, C>,
) -> Option<&Label> {
    if instruction.opcode.operation() != Operation::Label {
        return None;
    }
    sole_label_operand(instruction)
}

fn sole_label_operand<O: ControlFlowOperand, C>(
    instruction: &Instruction<O, C>,
) -> Option<&Label> {
    let [operand] = instruction.operands.as_slice() else {
        return None;
    };
    operand.label()
}

fn collect_cold_labels<O: ControlFlowOperand, C: ControlFlowOpcode>(
    instructions: &[Instruction<O, C>],
) -> Result<HashSet<Label>, String> {
    instructions
        .iter()
        .filter(|instruction| instruction.opcode.operation() == Operation::Cold)
        .map(|instruction| {
            sole_label_operand(instruction)
                .cloned()
                .ok_or_else(|| "cold annotation requires exactly one label".to_string())
        })
        .collect()
}

fn instruction_ends_block<O: ControlFlowOperand, C: ControlFlowOpcode>(
    instruction: &Instruction<O, C>,
) -> bool {
    instruction.opcode.description().terminal
        || (instruction.opcode.operation() != Operation::Label
            && instruction
                .operands
                .iter()
                .any(|operand| operand.label().is_some()))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[derive(Debug, Clone)]
    enum TestOperand {
        Label(Label),
    }

    impl ControlFlowOperand for TestOperand {
        fn label(&self) -> Option<&Label> {
            match self {
                Self::Label(label) => Some(label),
            }
        }

        fn label_mut(&mut self) -> Option<&mut Label> {
            match self {
                Self::Label(label) => Some(label),
            }
        }

        fn from_label(label: Label) -> Self {
            Self::Label(label)
        }
    }

    fn instruction(operation: Operation, operands: Vec<Operand>) -> Instruction {
        Instruction {
            opcode: operation,
            operands,
        }
    }

    fn label(name: &str) -> Operand {
        Operand::Label(name.into())
    }

    #[test]
    fn supports_operand_specific_control_flow_graphs() {
        let graph: ControlFlowGraph<TestOperand> =
            ControlFlowGraph::from_instructions(vec![Instruction {
            opcode: Operation::Label,
            operands: vec![TestOperand::Label(".entry".into())],
        }])
        .unwrap();

        assert!(graph.single_instruction().is_some());
    }

    #[test]
    fn builds_basic_blocks_and_successor_edges() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero), vec![Operand::VirtualRegister("value".into()), label(".zero")]),
            instruction(Operation::Control(ControlOperation::DispatchNext), vec![]),
            instruction(Operation::Label, vec![label(".zero")]),
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".done")]),
            instruction(Operation::Label, vec![label(".done")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();

        assert_eq!(graph.blocks.len(), 4);
        assert_eq!(
            graph.blocks[0].successors,
            vec![
                Edge {
                    target: BlockId(2),
                    kind: EdgeKind::Branch
                },
                Edge {
                    target: BlockId(1),
                    kind: EdgeKind::Fallthrough
                },
            ]
        );
        assert_eq!(
            graph.blocks[2].successors,
            vec![Edge {
                target: BlockId(3),
                kind: EdgeKind::Branch
            }]
        );
        assert!(graph.blocks[1].successors.is_empty());
        assert!(graph.blocks[3].successors.is_empty());

        let linear = graph.linearize_for_analysis();
        assert_eq!(
            linear.successors,
            vec![vec![2, 1], vec![], vec![3], vec![4], vec![5], vec![]]
        );
    }

    #[test]
    fn outlines_cold_blocks_without_changing_block_contents() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero), vec![Operand::VirtualRegister("value".into()), label(".slow")]),
            instruction(Operation::Control(ControlOperation::DispatchNext), vec![]),
            instruction(Operation::Cold, vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".slow")]),
            instruction(Operation::Call(CallKind::SlowPath), vec![
                Operand::Relocation(crate::low_ir::Relocation::function_call(
                    crate::identity::ExternalSymbol::new("slow_path"),
                )),
            ]),
        ])
        .unwrap();
        let layout = graph.layout_hot_and_cold().unwrap();

        assert_eq!(layout.hot, vec![BlockId(0), BlockId(1)]);
        assert_eq!(layout.cold, vec![BlockId(2)]);
        assert_eq!(
            graph
                .linearize_omitting_jumps_to_next_block(&layout.hot)
                .iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            vec![
                Operation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero),
                Operation::Control(ControlOperation::DispatchNext),
            ]
        );
        assert_eq!(
            graph
                .linearize_omitting_jumps_to_next_block(&layout.cold)
                .iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            vec![Operation::Label, Operation::Call(CallKind::SlowPath)]
        );
    }

    #[test]
    fn inverts_branches_whose_taken_target_follows_a_jump_block() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::branch_memory(PairWidth::Word, EqualityCondition::Equal), vec![Operand::VirtualRegister("value".into()), Operand::Immediate(1), label(".success")]),
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".success")]),
            instruction(Operation::Control(ControlOperation::DispatchNext), vec![]),
            instruction(Operation::Cold, vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".slow")]),
            instruction(Operation::Call(CallKind::SlowPath), vec![
                Operand::Relocation(crate::low_ir::Relocation::function_call(
                    crate::identity::ExternalSymbol::new("slow_path"),
                )),
            ]),
        ])
        .unwrap();
        let layout = graph.layout_hot_and_cold().unwrap();
        let instructions = graph.linearize_omitting_jumps_to_next_block(&layout.hot);

        assert_eq!(
            instructions
                .iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            vec![
                Operation::branch_memory(PairWidth::Word, EqualityCondition::NotEqual),
                Operation::Control(ControlOperation::DispatchNext),
            ]
        );
        assert_eq!(instructions[0].operands.last(), Some(&label(".slow")));
    }

    #[test]
    fn outlines_cold_regions_containing_multiple_basic_blocks() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::Control(ControlOperation::DispatchNext), vec![]),
            instruction(Operation::Cold, vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".slow")]),
            instruction(Operation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero), vec![Operand::VirtualRegister("value".into()), label(".done")]),
            instruction(Operation::Call(CallKind::SlowPath), vec![
                Operand::Relocation(crate::low_ir::Relocation::function_call(
                    crate::identity::ExternalSymbol::new("slow_path"),
                )),
            ]),
            instruction(Operation::Label, vec![label(".done")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();
        let layout = graph.layout_hot_and_cold().unwrap();

        assert_eq!(layout.hot, vec![BlockId(0), BlockId(3)]);
        assert_eq!(layout.cold, vec![BlockId(1), BlockId(2)]);
    }

    #[test]
    fn rejects_cold_fallthrough() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::Move(IntegerWidth::U64), vec![Operand::VirtualRegister("value".into()), Operand::Immediate(1)]),
            instruction(Operation::Cold, vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".slow")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();

        assert_eq!(
            graph.layout_hot_and_cold().unwrap_err(),
            "cold block '.slow' receives fallthrough control flow"
        );
    }

    #[test]
    fn identifies_the_opcode_producing_cold_fallthrough() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::Control(ControlOperation::DispatchNext), vec![]),
            instruction(Operation::Cold, vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".slow")]),
            instruction(
                Operation::Move(IntegerWidth::U64),
                vec![
                    Operand::VirtualRegister("value".into()),
                    Operand::Immediate(1),
                ],
            ),
        ])
        .unwrap();

        assert_eq!(
            graph.layout_hot_and_cold().unwrap_err(),
            "cold block '.slow' produces fallthrough control flow after 'Move(U64)'"
        );
    }

    #[test]
    fn threads_chained_unconditional_jumps() {
        let mut graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".inner")]),
            instruction(Operation::Label, vec![label(".inner")]),
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".outer")]),
            instruction(Operation::Label, vec![label(".outer")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();

        graph.thread_unconditional_jumps();
        let layout = graph.layout_hot_and_cold().unwrap();
        let instructions = graph.linearize_omitting_jumps_to_next_block(&layout.hot);

        assert_eq!(instructions[0].operands, vec![label(".outer")]);
    }

    #[test]
    fn removes_unreferenced_jump_blocks_after_threading() {
        let mut graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::branch_zero(IntegerWidth::U64, ZeroCondition::Zero), vec![Operand::VirtualRegister("value".into()), label(".edge")]),
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".done")]),
            instruction(Operation::Cold, vec![label(".edge")]),
            instruction(Operation::Label, vec![label(".edge")]),
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".target")]),
            instruction(Operation::Label, vec![label(".done")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
            instruction(Operation::Label, vec![label(".target")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();

        graph.thread_unconditional_jumps();
        graph.remove_unreferenced_jump_blocks();
        let layout = graph.layout_hot_and_cold().unwrap();
        let instructions = graph.linearize_omitting_jumps_to_next_block(&layout.hot);

        assert_eq!(instructions[0].operands[1], label(".target"));
        assert!(!instructions.iter().any(|instruction| {
            matches!(instruction.operands.first(), Some(Operand::Label(label)) if label.as_str() == ".edge")
        }));
        assert!(layout.cold.is_empty());
    }

    #[test]
    fn removes_unreachable_blocks_before_cold_layout() {
        let mut graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".slow")]),
            instruction(Operation::Cold, vec![label(".edge")]),
            instruction(Operation::Label, vec![label(".edge")]),
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".return")]),
            instruction(Operation::Label, vec![label(".return")]),
            instruction(Operation::Cold, vec![label(".slow")]),
            instruction(Operation::Label, vec![label(".slow")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();

        graph.remove_unreachable_blocks();
        let layout = graph.layout_hot_and_cold().unwrap();

        assert_eq!(layout.hot, vec![BlockId(0)]);
        assert_eq!(layout.cold, vec![BlockId(1)]);
    }

    #[test]
    fn omits_jump_and_unreferenced_label_to_next_layout_block() {
        let graph = ControlFlowGraph::from_instructions(vec![
            instruction(Operation::Control(ControlOperation::JumpLabel), vec![label(".done")]),
            instruction(Operation::Label, vec![label(".done")]),
            instruction(Operation::Control(ControlOperation::Exit), vec![]),
        ])
        .unwrap();
        let layout = graph.layout_hot_and_cold().unwrap();
        let instructions = graph.linearize_omitting_jumps_to_next_block(&layout.hot);

        assert_eq!(
            instructions
                .iter()
                .map(|instruction| instruction.opcode.operation())
                .collect::<Vec<_>>(),
            vec![Operation::Control(ControlOperation::Exit)]
        );
    }
}
