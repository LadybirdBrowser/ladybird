/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Interrupt check elimination: small counting loops lose their
//! `Op::CheckInterrupt`.
//!
//! The check is two instructions, which is a lot for a loop of a few. It is
//! there so that no script runs forever and an interrupt arrives soon, and
//! both still hold for two kinds of innermost loops. One that is known to
//! run a few thousand times at most stops soon by itself. One that counts
//! towards any other bound ends within 2^32 iterations, like a builtin that
//! goes through a huge array, and script then stops at the check of a loop
//! around it. A counting loop without a loop around it keeps its check, or it
//! could run for seconds after an interrupt. The code around a loop must not
//! be able to continue without passing a check.
//!
//! A loop counts if a header phi changes by a constant on every back edge,
//! towards a bound that a branch tests on every iteration: an int32 only has
//! so many values, and the addition exits on overflow. One that wraps around
//! instead (`(i + 1) | 0`) only counts if it steps by one and the bound is one
//! it never steps past. The branch is what matters even where the addition
//! exits on overflow: only the check's frame state keeps a counter alive that
//! nothing else uses, without which the loop would not count anything.

use super::dominators::Dominators;
use crate::bitset::BitSet;
use crate::ir::BinaryOp;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Comparison;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;

/// How many nodes a loop may have to lose its check. In larger ones the check
/// costs next to nothing, and they take longer to count to their end.
const MAX_LOOP_SIZE: usize = 64;

/// How many iterations a loop may have to lose its check without a loop around
/// it, the bound V8 uses for this.
const MAX_ITERATIONS_WITHOUT_CHECK: i64 = 5000;

struct Loop {
    header: BlockId,
    blocks: BitSet,
    back_edge_sources: Vec<BlockId>,
}

/// By how much `next` changes the phi `phi`, if that is a constant, and
/// whether the change exits on overflow.
fn step_of(graph: &Graph, phi: NodeId, next: NodeId) -> Option<(i64, bool)> {
    let node = graph.node(graph.unrefined(next));
    let (op, exits_on_overflow) = match node.op {
        Op::Int32Binary { op } => (op, true),
        Op::Int32WrappingBinary { op } => (op, false),
        _ => return None,
    };
    if graph.unrefined(node.inputs[0]) != phi {
        return None;
    }
    let constant = i64::from(graph.int32_constant(graph.unrefined(node.inputs[1]))?);
    let step = match op {
        BinaryOp::Add => constant,
        BinaryOp::Sub => -constant,
        _ => return None,
    };
    (step != 0).then_some((step, exits_on_overflow))
}

/// The bound of a branch that every iteration of the loop passes, and that
/// leaves it once `phi` or `next`, which changes it by `step`, gets there. An
/// addition that wraps around must not get past the bound.
fn tested_bound(
    graph: &Graph,
    dominators: &Dominators,
    loop_: &Loop,
    phi: NodeId,
    next: NodeId,
    step: i64,
    exits_on_overflow: bool,
) -> Option<NodeId> {
    loop_.blocks.iter().map(BlockId::from_index).find_map(|block| {
        let control = graph.control(block);
        let Op::Branch {
            condition: BranchCondition::Bool,
            if_true,
            if_false,
        } = control.op
        else {
            return None;
        };
        if !loop_
            .back_edge_sources
            .iter()
            .all(|source| dominators.dominates(block, *source))
        {
            return None;
        }
        let compare = graph.node(graph.unrefined(control.inputs[0]));
        let Op::Int32Compare { comparison } = compare.op else {
            return None;
        };
        let counter = graph.unrefined(compare.inputs[0]);
        if counter != phi && counter != graph.unrefined(next) {
            return None;
        }
        let stays_in_loop_if_true = match (
            loop_.blocks.contains(if_true.index()),
            loop_.blocks.contains(if_false.index()),
        ) {
            (true, false) => true,
            (false, true) => false,
            _ => return None,
        };
        let is_strict = match (step > 0, comparison, stays_in_loop_if_true) {
            (true, Comparison::LessThan, true)
            | (true, Comparison::GreaterThanEquals, false)
            | (false, Comparison::GreaterThan, true)
            | (false, Comparison::LessThanEquals, false) => true,
            (true, Comparison::LessThanEquals, true)
            | (true, Comparison::GreaterThan, false)
            | (false, Comparison::GreaterThanEquals, true)
            | (false, Comparison::LessThan, false) => false,
            _ => return None,
        };
        (exits_on_overflow || (is_strict && step.abs() == 1)).then(|| graph.unrefined(compare.inputs[1]))
    })
}

/// How much is known about how long a loop runs.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Trip {
    /// It counts towards a bound, so it ends.
    Counted,
    /// It ends within MAX_ITERATIONS_WITHOUT_CHECK iterations.
    Short,
}

/// Whether the iterations from every way into the loop to `bound` are few.
fn is_short(graph: &Graph, loop_: &Loop, phi: NodeId, bound: NodeId, step: i64) -> bool {
    let Some(bound) = graph.int32_constant(bound).map(i64::from) else {
        return false;
    };
    let predecessors = &graph.block(loop_.header).predecessors;
    predecessors
        .iter()
        .zip(&graph.node(phi).inputs)
        .filter(|(predecessor, _)| !loop_.back_edge_sources.contains(predecessor))
        .all(|(_, start)| {
            graph
                .int32_constant(graph.unrefined(*start))
                .is_some_and(|start| (bound - i64::from(start)) / step < MAX_ITERATIONS_WITHOUT_CHECK)
        })
}

/// What the header phi `phi` tells about the end of the loop, if it counts
/// towards one.
fn trip_of(graph: &Graph, dominators: &Dominators, loop_: &Loop, phi: NodeId) -> Option<Trip> {
    let predecessors = &graph.block(loop_.header).predecessors;
    let inputs = &graph.node(phi).inputs;
    let mut direction = 0;
    let mut trip = Trip::Short;
    for (_, next) in predecessors
        .iter()
        .zip(inputs)
        .filter(|(predecessor, _)| loop_.back_edge_sources.contains(predecessor))
    {
        let (step, exits_on_overflow) = step_of(graph, phi, *next)?;
        // NB: Back edges that step in opposite directions could go on forever.
        if direction != 0 && direction != step.signum() {
            return None;
        }
        direction = step.signum();
        let bound = tested_bound(graph, dominators, loop_, phi, *next, step, exits_on_overflow)?;
        if !is_short(graph, loop_, phi, bound, step) {
            trip = Trip::Counted;
        }
    }
    Some(trip)
}

/// Whether the loop has no loop inside it, nothing that makes its check not
/// worth removing, and no edge back to a block around it, which would be a
/// back edge of a loop around it without a check of its own (like a
/// `continue` of an outer loop).
fn is_small_and_innermost(graph: &Graph, loop_: &Loop) -> bool {
    let mut size = 0;
    for index in loop_.blocks.iter() {
        let block = graph.block(BlockId::from_index(index));
        if !block.is_cold {
            size += block.body.len();
            if block.body.iter().any(|node| graph.node(*node).op.properties().is_call) {
                return false;
            }
        }
        for successor in graph.successors(BlockId::from_index(index)) {
            if successor != loop_.header && successor.index() <= index {
                return false;
            }
        }
    }
    size <= MAX_LOOP_SIZE
}

/// Whether what follows the loop can get back to it. Whatever way that goes,
/// it has a back edge of a loop that keeps its check, since only innermost
/// ones lose theirs. This looks at the edges and not at natural loops: an
/// on-stack replacement entry into a loop keeps the loop around it from being
/// a natural loop.
fn has_loop_around(graph: &Graph, loop_: &Loop) -> bool {
    let mut visited = BitSet::new(graph.blocks.len());
    let mut worklist: Vec<BlockId> = loop_.blocks.iter().map(BlockId::from_index).collect();
    while let Some(block) = worklist.pop() {
        for successor in graph.successors(block) {
            if successor == loop_.header && !loop_.blocks.contains(block.index()) {
                return true;
            }
            if !loop_.blocks.contains(successor.index()) && !visited.contains(successor.index()) {
                visited.insert(successor.index());
                worklist.push(successor);
            }
        }
    }
    false
}

pub fn run(graph: &mut Graph) {
    if !graph.nodes.iter().any(|node| matches!(node.op, Op::CheckInterrupt)) {
        return;
    }
    let dominators = Dominators::compute(graph);
    let mut loops = Vec::new();
    for index in 0..graph.blocks.len() {
        let header = BlockId::from_index(index);
        let back_edge_sources = dominators.back_edge_sources(graph, header);
        if back_edge_sources.is_empty() {
            continue;
        }
        let blocks = dominators.loop_blocks(graph, header, &back_edge_sources);
        loops.push(Loop {
            header,
            blocks,
            back_edge_sources,
        });
    }
    for loop_ in &loops {
        if !is_small_and_innermost(graph, loop_) {
            continue;
        }
        let best_trip = graph
            .block(loop_.header)
            .phis
            .iter()
            .filter_map(|phi| trip_of(graph, &dominators, loop_, *phi))
            .max();
        let may_lose_check = match best_trip {
            Some(Trip::Short) => true,
            Some(Trip::Counted) => has_loop_around(graph, loop_),
            None => false,
        };
        if !may_lose_check {
            continue;
        }
        for block in loop_.blocks.iter() {
            let nodes = &graph.nodes;
            graph.blocks[block]
                .body
                .retain(|node| !matches!(nodes[node.index()].op, Op::CheckInterrupt));
        }
    }
}
