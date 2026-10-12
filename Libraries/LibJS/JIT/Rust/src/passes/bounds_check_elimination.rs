/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Bounds check elimination: bounds checks of indices whose ranges are
//! known to be within the count go away.
//!
//! The ranges come from loop induction variables and from int32 arithmetic
//! on them. An induction variable is a loop header phi that starts at a
//! value of known range and grows by a constant step on every iteration,
//! where the back edge is only taken while the grown value compares below a
//! constant bound, like the counters of loops that test at their bottom
//! (`do { ... } while (++i != 1024)`). Its values then lie between its
//! start and the bound. Arithmetic on values of known ranges has a known
//! range where it cannot overflow: additions, subtractions, shifts by
//! constants and masks with constants.
//!
//! The pass runs after loop invariant code motion, which gives every loop
//! one entry: a header phi's first input comes from outside the loop, and
//! its others over back edges. Merges of values from outside the loop, like
//! the starts of a counter that the function entry and an on-stack
//! replacement entry bring, are in the range of all their inputs.
//!
//! The counter an on-stack replacement entry finds in the frame could be
//! anything. Where the entry checks it to be an int32, it is checked to be
//! below the loop's bound there too, once, unless such a check exited there
//! before: the loop keeps it below the bound anyway. Those checks are only
//! added where a bounds check, a rewritten shift or an operation that stops
//! checking for overflow relies on them, including through the counts of
//! the bounds checks that bound its operands.
//!
//! With the same ranges, `((x << k) + c) >> k` becomes `x + (c >> k)` where
//! nothing overflows and `c` is a multiple of 2^k, like the index arithmetic
//! of asm.js heap accesses.
//!
//! Additions, subtractions and multiplications whose operands' ranges keep
//! the result an int32 (and, for multiplications, from being -0) stop
//! checking for that, like `(a & 0x3fff) * (b >> 14)` and the `i + 1` after
//! an access at `i`. Besides the ranges above, the ranges of operands come
//! from masks and shifts of any int32, and from the bounds checks of
//! accesses (and the branches on an index being in bounds) that dominate
//! the operation: an index is below the elements' count there.

use super::dominators::Dominators;
use super::edit::Constants;
use super::edit::Replacements;
use crate::bitset::BitSet;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::fast_hash::HashMap;
use crate::ir::BinaryOp;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;

/// The inclusive range of an int32 value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct Range {
    min: i64,
    max: i64,
}

impl Range {
    /// Every int32.
    const INT32: Self = Self {
        min: i32::MIN as i64,
        max: i32::MAX as i64,
    };

    fn new(min: i64, max: i64) -> Option<Self> {
        (i64::from(i32::MIN) <= min && min <= max && max <= i64::from(i32::MAX)).then_some(Self { min, max })
    }

    fn intersection(self, other: Self) -> Option<Self> {
        Self::new(self.min.max(other.min), self.max.min(other.max))
    }

    fn contains(self, value: i64) -> bool {
        self.min <= value && value <= self.max
    }

    /// The range of the sum of values of `self` and `other`, if it is
    /// within the int32 range.
    fn sum(self, other: Self) -> Option<Self> {
        Self::new(self.min + other.min, self.max + other.max)
    }

    /// The range of the difference of values of `self` and `other`, if it
    /// is within the int32 range.
    fn difference(self, other: Self) -> Option<Self> {
        Self::new(self.min - other.max, self.max - other.min)
    }

    /// The range of the product of values of `self` and `other`, if it is
    /// within the int32 range.
    fn product(self, other: Self) -> Option<Self> {
        let products = [
            self.min * other.min,
            self.min * other.max,
            self.max * other.min,
            self.max * other.max,
        ];
        Self::new(*products.iter().min()?, *products.iter().max()?)
    }
}

struct Ranges<'a> {
    graph: &'a Graph,
    dominators: &'a Dominators,
    /// The block of each phi.
    phi_blocks: &'a HashMap<NodeId, BlockId>,
    /// The ranges of entering counters that checks added where they enter
    /// would give them.
    assumed: &'a HashMap<NodeId, Range>,
    known: HashMap<NodeId, Option<Range>>,
}

impl Ranges<'_> {
    fn of(&mut self, value: NodeId) -> Option<Range> {
        if let Some(range) = self.assumed.get(&value) {
            return Some(*range);
        }
        if let Some(range) = self.known.get(&value) {
            return *range;
        }
        // NB: Cycles through phis that are no induction variables have no
        //     range; a phi in progress has none until it is done.
        self.known.insert(value, None);
        let range = self.compute(value);
        self.known.insert(value, range);
        range
    }

    fn constant(&self, value: NodeId) -> Option<i64> {
        self.graph.int32_constant(value).map(i64::from)
    }

    fn compute(&mut self, value: NodeId) -> Option<Range> {
        let node = self.graph.node(value);
        if node.repr != Some(Repr::Int32) {
            return None;
        }
        if let Some(constant) = self.constant(value) {
            return Range::new(constant, constant);
        }
        // NB: A value past its bounds check is below the count, and other
        //     refinements keep the range of what they refine.
        if bounds_index(&node.op) {
            let index = self.of(node.inputs[0]);
            return self.below_count(index, node.inputs[1]);
        }
        if let Some(refined) = node.op.refined_input() {
            return self.of(node.inputs[refined]);
        }
        match node.op {
            Op::Phi if self.is_merge(value) => {
                let inputs = node.inputs.clone();
                let mut range: Option<Range> = None;
                for input in inputs {
                    let input = self.of(input)?;
                    range = Some(match range {
                        None => input,
                        Some(range) => Range::new(range.min.min(input.min), range.max.max(input.max))?,
                    });
                }
                range
            }
            Op::Phi => self.induction_variable(value),
            Op::Int32Binary { op } | Op::Int32WrappingBinary { op } => {
                let (lhs, rhs) = (node.inputs[0], node.inputs[1]);
                match op {
                    BinaryOp::Add => self.of(lhs)?.sum(self.of(rhs)?),
                    BinaryOp::Sub => self.of(lhs)?.difference(self.of(rhs)?),
                    BinaryOp::LeftShift => {
                        let (lhs, shift) = (self.of(lhs)?, self.constant(rhs)? & 31);
                        Range::new(lhs.min << shift, lhs.max << shift)
                    }
                    BinaryOp::RightShift => {
                        let shift = self.constant(rhs)? & 31;
                        let lhs = self.of(lhs).unwrap_or(Range::INT32);
                        Range::new(lhs.min >> shift, lhs.max >> shift)
                    }
                    BinaryOp::Mul => self.of(lhs)?.product(self.of(rhs)?),
                    // NB: Shifting any int32 right as an unsigned number by
                    //     at least one leaves a non-negative int32.
                    BinaryOp::UnsignedRightShift => {
                        let shift = self.constant(rhs)? & 31;
                        match self.of(lhs) {
                            Some(lhs) if lhs.min >= 0 => Range::new(lhs.min >> shift, lhs.max >> shift),
                            _ if shift > 0 => Range::new(0, i64::from(u32::MAX) >> shift),
                            _ => None,
                        }
                    }
                    BinaryOp::BitwiseAnd => {
                        let mask = self.constant(rhs).filter(|mask| *mask >= 0)?;
                        match self.of(lhs) {
                            Some(lhs) if lhs.min >= 0 => Range::new(0, lhs.max.min(mask)),
                            _ => Range::new(0, mask),
                        }
                    }
                    _ => None,
                }
            }
            // NB: Storage capacities are int32 values (see
            //     `Op::LoadElementsCapacity`), which are never negative.
            Op::LoadElementsCapacity => Range::new(0, Range::INT32.max),
            // NB: The smaller of two values as unsigned numbers is at most
            //     the one that is not negative.
            Op::Uint32Min => {
                let (lhs, rhs) = (self.of(node.inputs[0]), self.of(node.inputs[1]));
                [lhs, rhs]
                    .into_iter()
                    .flatten()
                    .filter(|range| range.min >= 0)
                    .map(|range| range.max)
                    .min()
                    .and_then(|max| Range::new(0, max))
            }
            _ => None,
        }
    }

    /// The range of `index` (of range `range`, if it has one) where it is
    /// below `count` as unsigned numbers, which a bounds check made it. A
    /// count of unknown range leaves the range of the index.
    fn below_count(&mut self, range: Option<Range>, count: NodeId) -> Option<Range> {
        let count = match self.constant(count) {
            Some(count) => Range::new(count, count)?,
            None => match self.of(count).filter(|count| count.min >= 0) {
                Some(count) => count,
                None => return range,
            },
        };
        let below = Range::new(0, count.max - 1)?;
        match range {
            Some(range) => range.intersection(below),
            None => Some(below),
        }
    }

    /// For `((x << k) + c) >> k` and `(x << k) >> k` where nothing can
    /// overflow and `c` is a multiple of 2^k: `x` and `c >> k`, which it is.
    fn unscaled(&mut self, shift: NodeId) -> Option<(NodeId, i64)> {
        let node = self.graph.node(shift);
        let amount = self.constant(node.inputs[1])? & 31;
        let shifted = self.graph.node(node.inputs[0]);
        let (scaled, offset) = match shifted.op {
            Op::Int32Binary { op: BinaryOp::Add } | Op::Int32WrappingBinary { op: BinaryOp::Add } => {
                (shifted.inputs[0], self.constant(shifted.inputs[1])?)
            }
            _ => (node.inputs[0], 0),
        };
        if offset % (1 << amount) != 0 {
            return None;
        }
        let scaled_node = self.graph.node(scaled);
        let is_left_shift = matches!(
            scaled_node.op,
            Op::Int32Binary {
                op: BinaryOp::LeftShift
            } | Op::Int32WrappingBinary {
                op: BinaryOp::LeftShift
            }
        );
        if !is_left_shift || self.constant(scaled_node.inputs[1])? & 31 != amount {
            return None;
        }
        // NB: Known ranges of the shifted value and the sum mean neither
        //     overflowed.
        self.of(scaled)?;
        self.of(node.inputs[0])?;
        Some((scaled_node.inputs[0], offset >> amount))
    }

    /// Whether `predecessor` reaches `block` over a back edge.
    fn is_back_edge(&self, predecessor: BlockId, block: BlockId) -> bool {
        self.dominators.dominates(block, predecessor)
    }

    /// Whether `phi` merges values that no back edge brings.
    fn is_merge(&self, phi: NodeId) -> bool {
        self.phi_blocks.get(&phi).is_some_and(|block| {
            self.graph
                .block(*block)
                .predecessors
                .iter()
                .all(|predecessor| !self.is_back_edge(*predecessor, *block))
        })
    }

    /// The step, the comparison and the bound of the loop header phi `phi`,
    /// if it is shaped like an induction variable, whatever its start.
    fn induction_step(&self, phi: NodeId) -> Option<(i64, Comparison, i64)> {
        let block = *self.phi_blocks.get(&phi)?;
        let inputs = &self.graph.node(phi).inputs;
        let predecessors = &self.graph.block(block).predecessors;
        if inputs.len() != 2 || self.is_back_edge(predecessors[0], block) || !self.is_back_edge(predecessors[1], block)
        {
            return None;
        }
        let next = self.graph.unrefined(inputs[1]);
        let next_node = self.graph.node(next);
        let step = match next_node.op {
            Op::Int32Binary { op: BinaryOp::Add } | Op::Int32WrappingBinary { op: BinaryOp::Add }
                if self.graph.unrefined(next_node.inputs[0]) == phi =>
            {
                self.constant(next_node.inputs[1]).filter(|step| *step > 0)?
            }
            _ => return None,
        };
        let (comparison, bound) = self.back_edge_condition(predecessors[1], next)?;
        Some((step, comparison, bound))
    }

    /// The range of the loop header phi `phi`, if it is an induction
    /// variable that grows by a constant step while the grown value stays
    /// below a constant bound.
    fn induction_variable(&mut self, phi: NodeId) -> Option<Range> {
        let (step, comparison, bound) = self.induction_step(phi)?;
        let start = self.of(self.graph.node(phi).inputs[0])?;
        // NB: The phi's values are its start, and grown values for which the
        //     back edge was taken.
        let last = match comparison {
            Comparison::LessThan => bound - 1,
            Comparison::LessThanEquals => bound,
            // NB: A loop until the grown value equals the bound only stops
            //     there if it steps on it.
            Comparison::StrictlyInequals | Comparison::LooselyInequals
                if start.max < bound && (step == 1 || (start.min == start.max && (bound - start.min) % step == 0)) =>
            {
                bound - step
            }
            _ => return None,
        };
        let max = start.max.max(last);
        // NB: Growing the last value must not overflow either.
        Range::new(max, max + step)?;
        Range::new(start.min, max)
    }

    /// The comparison and constant bound of the grown value `next` under
    /// which the back edge from `source` is taken: `source` only jumps to
    /// the header, after a branch on comparing `next` with a constant.
    fn back_edge_condition(&self, source: BlockId, next: NodeId) -> Option<(Comparison, i64)> {
        if !matches!(self.graph.control(source).op, Op::Jump { .. }) {
            return None;
        }
        let [branch_block] = self.graph.block(source).predecessors[..] else {
            return None;
        };
        let branch = self.graph.control(branch_block);
        let Op::Branch {
            condition: BranchCondition::Bool,
            if_true,
            if_false,
        } = branch.op
        else {
            return None;
        };
        let compare = self.graph.node(self.graph.unrefined(branch.inputs[0]));
        let Op::Int32Compare { comparison } = compare.op else {
            return None;
        };
        if self.graph.unrefined(compare.inputs[0]) != next {
            return None;
        }
        let bound = self.constant(compare.inputs[1])?;
        let comparison = if if_true == source {
            comparison
        } else if if_false == source {
            match comparison {
                Comparison::GreaterThanEquals => Comparison::LessThan,
                Comparison::GreaterThan => Comparison::LessThanEquals,
                Comparison::StrictlyEquals => Comparison::StrictlyInequals,
                Comparison::LooselyEquals => Comparison::LooselyInequals,
                _ => return None,
            }
        } else {
            return None;
        };
        Some((comparison, bound))
    }

    /// The entering counters of `assumed` that the range of `value` relies
    /// on, added to `used`.
    fn assumptions_of(&self, value: NodeId, used: &mut BitSet, visited: &mut BitSet) {
        let mut pending = vec![value];
        while let Some(value) = pending.pop() {
            if visited.contains(value.index()) {
                continue;
            }
            visited.insert(value.index());
            if self.assumed.contains_key(&value) {
                used.insert(value.index());
                continue;
            }
            pending.extend(self.graph.node(value).inputs.iter().copied());
        }
    }
}

/// Whether `op` makes its input 0 below its input 1 as unsigned numbers:
/// a bounds check, or the refinement of a branch on that where it holds.
fn bounds_index(op: &Op) -> bool {
    matches!(
        op,
        Op::CheckBounds
            | Op::Refine {
                condition: BranchCondition::IndexInBounds,
                holds: true,
            }
    )
}

/// The blocks that only on-stack replacement entries reach.
fn osr_only_blocks(graph: &Graph) -> BitSet {
    let reach = |starts: Vec<BlockId>| {
        let mut reached = BitSet::new(graph.blocks.len());
        let mut pending = starts;
        while let Some(block) = pending.pop() {
            if reached.contains(block.index()) {
                continue;
            }
            reached.insert(block.index());
            pending.extend(graph.successors(block).iter().copied());
        }
        reached
    };
    let mut osr = reach(graph.osr_entries.iter().map(|(_, block)| *block).collect());
    osr.subtract(&reach(vec![BlockId::from_index(0)]));
    osr
}

/// The counters that on-stack replacement entries bring to induction
/// variables and check to be int32 values, with the bound each could be
/// checked against: the tightest of the loops it starts.
fn entering_counters(
    graph: &Graph,
    dominators: &Dominators,
    phi_blocks: &HashMap<NodeId, BlockId>,
) -> HashMap<NodeId, Range> {
    let mut entering = HashMap::default();
    if graph.osr_entries.is_empty() {
        return entering;
    }
    let osr_only = osr_only_blocks(graph);
    let mut block_of = HashMap::default();
    for (index, block) in graph.blocks.iter().enumerate() {
        for node in &block.body {
            block_of.insert(*node, index);
        }
    }
    let none = HashMap::default();
    let ranges = Ranges {
        graph,
        dominators,
        phi_blocks,
        assumed: &none,
        known: HashMap::default(),
    };
    for phi in phi_blocks.keys() {
        let Some((_, comparison, bound)) = ranges.induction_step(*phi) else {
            continue;
        };
        if bound <= 0
            || !matches!(
                comparison,
                Comparison::LessThan | Comparison::StrictlyInequals | Comparison::LooselyInequals
            )
        {
            continue;
        }
        // NB: Look through merges of the values the entries bring.
        let mut pending = vec![graph.node(*phi).inputs[0]];
        let mut seen = BitSet::new(graph.nodes.len());
        while let Some(value) = pending.pop() {
            let value = graph.unrefined(value);
            if seen.contains(value.index()) {
                continue;
            }
            seen.insert(value.index());
            let node = graph.node(value);
            if node.op == Op::Phi && ranges.is_merge(value) {
                pending.extend(node.inputs.iter().copied());
                continue;
            }
            let Some(frame_state) = node.frame_state else {
                continue;
            };
            let state = graph.frame_state(frame_state);
            let failed_before = graph
                .exit_sites
                .contains(&(state.executable, state.pc, ExitKind::OutOfBounds));
            if node.op == Op::CheckInt32
                && !failed_before
                && block_of.get(&value).is_some_and(|block| osr_only.contains(*block))
            {
                let range = Range::new(0, bound - 1).expect("the bound is a positive int32");
                entering
                    .entry(value)
                    .and_modify(|assumed: &mut Range| assumed.max = assumed.max.min(range.max))
                    .or_insert(range);
            }
        }
    }
    entering
}

pub fn run(graph: &mut Graph) {
    let mut phi_blocks = HashMap::default();
    let mut checks = Vec::new();
    let mut shifts = Vec::new();
    let mut arithmetic = Vec::new();
    // NB: For every index that bounds checks and branches check, where
    //     those checks are and the counts.
    let mut bounded: HashMap<NodeId, Vec<(BlockId, usize, NodeId)>> = HashMap::default();
    for (index, block) in graph.blocks.iter().enumerate() {
        for phi in &block.phis {
            phi_blocks.insert(*phi, BlockId::from_index(index));
        }
        for (position, node) in block.body.iter().enumerate() {
            let check = graph.node(*node);
            if bounds_index(&check.op) {
                bounded.entry(graph.unrefined(check.inputs[0])).or_default().push((
                    BlockId::from_index(index),
                    position,
                    check.inputs[1],
                ));
            }
            match check.op {
                Op::CheckBounds => checks.push(*node),
                Op::Int32Binary {
                    op: BinaryOp::RightShift,
                } => shifts.push((*node, BlockId::from_index(index))),
                Op::Int32Binary {
                    op: BinaryOp::Add | BinaryOp::Sub | BinaryOp::Mul,
                } => arithmetic.push((*node, BlockId::from_index(index), position)),
                _ => {}
            }
        }
    }
    // NB: Only bounds checks against constant counts can go away, and only
    //     operations whose operands may have ranges (see `Ranges::compute()`)
    //     can stop checking for overflow. The others need no analysis.
    let may_have_range = |value: NodeId| {
        let value = graph.unrefined(value);
        graph.int32_constant(value).is_some()
            || bounded.contains_key(&value)
            || match graph.node(value).op {
                Op::Phi
                | Op::Int32Binary { .. }
                | Op::Int32WrappingBinary { .. }
                | Op::LoadElementsCapacity
                | Op::Uint32Min => true,
                // NB: The counters that on-stack replacement entries check.
                Op::CheckInt32 => !graph.osr_entries.is_empty(),
                _ => false,
            }
    };
    checks.retain(|check| graph.int32_constant(graph.node(*check).inputs[1]).is_some());
    arithmetic.retain(|(node, ..)| graph.node(*node).inputs.iter().all(|input| may_have_range(*input)));
    if checks.is_empty() && shifts.is_empty() && arithmetic.is_empty() {
        return;
    }
    let dominators = Dominators::compute(graph);
    let entering = entering_counters(graph, &dominators, &phi_blocks);
    let mut ranges = Ranges {
        graph,
        dominators: &dominators,
        phi_blocks: &phi_blocks,
        assumed: &entering,
        known: HashMap::default(),
    };
    let passing = checks
        .into_iter()
        .filter(|check| {
            let node = graph.node(*check);
            match (ranges.constant(node.inputs[1]), ranges.of(node.inputs[0])) {
                (Some(count), Some(index)) => index.min >= 0 && index.max < count,
                _ => false,
            }
        })
        .collect::<Vec<_>>();
    let scaled = shifts
        .into_iter()
        .filter_map(|(shift, block)| {
            ranges
                .unscaled(shift)
                .map(|(value, offset)| (shift, block, value, offset))
        })
        .collect::<Vec<_>>();

    // NB: With the counts of the bounds checks that their operands' ranges
    //     rely on.
    let exact = arithmetic
        .into_iter()
        .filter_map(|(node, block, position)| {
            let node_data = graph.node(node);
            let Op::Int32Binary { op } = node_data.op else {
                unreachable!("only int32 arithmetic is collected");
            };
            let mut counts = Vec::new();
            // NB: Every dominating bounds check narrows the range of an
            //     operand further.
            let mut operand = |input: NodeId| {
                let mut range = ranges.of(input);
                let dominating = bounded.get(&graph.unrefined(input)).into_iter().flatten().filter(
                    |(check_block, check_position, _)| {
                        if *check_block == block {
                            *check_position < position
                        } else {
                            dominators.dominates(*check_block, block)
                        }
                    },
                );
                for (_, _, count) in dominating {
                    let narrowed = ranges.below_count(range, *count);
                    if narrowed.is_some() && narrowed != range {
                        counts.push(*count);
                        range = narrowed;
                    }
                }
                range
            };
            let (lhs, rhs) = (operand(node_data.inputs[0])?, operand(node_data.inputs[1])?);
            let exact = match op {
                BinaryOp::Add => lhs.sum(rhs).is_some(),
                BinaryOp::Sub => lhs.difference(rhs).is_some(),
                // NB: A zero product is -0 where an operand is negative.
                BinaryOp::Mul => {
                    lhs.product(rhs).is_some()
                        && ((lhs.min >= 0 && rhs.min >= 0) || (!lhs.contains(0) && !rhs.contains(0)))
                }
                _ => unreachable!("only additions, subtractions and multiplications are collected"),
            };
            exact.then_some((node, counts))
        })
        .collect::<Vec<_>>();
    if passing.is_empty() && scaled.is_empty() && exact.is_empty() {
        return;
    }
    // NB: Only the entering counters that the removed checks, the
    //     rewritten shifts and the operations that stop checking for overflow
    //     relied on get checked where they enter.
    let mut used = BitSet::new(graph.nodes.len());
    let mut visited = BitSet::new(graph.nodes.len());
    for check in &passing {
        ranges.assumptions_of(graph.node(*check).inputs[0], &mut used, &mut visited);
    }
    for (shift, ..) in &scaled {
        ranges.assumptions_of(*shift, &mut used, &mut visited);
    }
    for (node, counts) in &exact {
        ranges.assumptions_of(*node, &mut used, &mut visited);
        for count in counts {
            ranges.assumptions_of(*count, &mut used, &mut visited);
        }
    }
    let guards = entering
        .iter()
        .filter(|(value, _)| used.contains(value.index()))
        .map(|(value, range)| (*value, range.max + 1))
        .collect::<Vec<_>>();

    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    for check in passing {
        replacements.replace(check, graph.node(check).inputs[0]);
        removed.insert(check.index());
    }
    let mut constants = Constants::new(graph);
    let mut guard_nodes = Vec::new();
    for (value, bound) in guards {
        let count = constants.get_typed(graph, u64::from((bound as i32).cast_unsigned()), Repr::Int32);
        let (frame_state, pc) = (graph.node(value).frame_state, graph.node(value).pc);
        let guard = graph.add_node(Node {
            op: Op::CheckBounds,
            inputs: vec![value, count],
            repr: Some(Repr::Int32),
            frame_state,
            pc,
        });
        let block = graph
            .blocks
            .iter()
            .position(|block| block.body.contains(&value))
            .expect("values are in their blocks");
        let body = &mut graph.blocks[block].body;
        let position = body
            .iter()
            .position(|node| *node == value)
            .expect("the block has the value");
        body.insert(position + 1, guard);
        replacements.replace(value, guard);
        guard_nodes.push((guard, value));
    }
    for (shift, block, value, offset) in scaled {
        let replacement = if offset == 0 {
            value
        } else {
            let offset = constants.get_typed(graph, u64::from((offset as i32).cast_unsigned()), Repr::Int32);
            let sum = graph.add_node(Node::new(
                Op::Int32WrappingBinary { op: BinaryOp::Add },
                vec![value, offset],
                Some(Repr::Int32),
                graph.node(shift).pc,
            ));
            let body = &mut graph.blocks[block.index()].body;
            let position = body
                .iter()
                .position(|node| *node == shift)
                .expect("nodes are in their blocks");
            body.insert(position, sum);
            sum
        };
        replacements.replace(shift, replacement);
        removed.insert(shift.index());
    }
    for (node, _) in exact {
        super::edit::make_wrapping(graph, node);
    }
    replacements.apply(graph);
    // NB: The guards check the values they replace.
    for (guard, value) in guard_nodes {
        graph.nodes[guard.index()].inputs[0] = value;
    }
    super::edit::remove_nodes(graph, &removed);
}
