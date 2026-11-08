/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Interrupt check elimination: small counting loops lose their `Op::CheckInterrupt`, as in LibJS's JIT.
//!
//! A check costs a loop of a few instructions a lot of them. It is there so that no code runs forever and an interrupt
//! arrives soon, and both still hold for two kinds of innermost loops. One that is known to run a few thousand times
//! at most stops soon by itself. One that counts towards any other bound ends within 2^32 iterations, and the code then
//! stops at the check of the loop around it. A counting loop without a loop around it keeps its check, or it could run
//! for seconds after an interrupt.
//!
//! An i32 phi of the loop's header counts if every back edge steps it by one in the same direction, and a branch that
//! every iteration passes leaves the loop once it gets to a bound. Everything wraps around in WebAssembly, so the test
//! must be one that a step by one cannot go past: `<` going up, `>` going down, or `!=` of a bound that stays the same
//! and so eventually comes up. A step of any other size can go past all of those.

use crate::ir::{BlockId, Func, IntCondition, NONE, Op, Type, Value};
use crate::passes::cleanup::Liveliness;
use crate::passes::dominators::Dominators;

/// How many nodes a loop may have to lose its check. In larger ones the check costs next to nothing.
const MAX_LOOP_SIZE: u32 = 64;

/// How many iterations a loop may have to lose its check without a loop around it, the bound V8 uses for this.
const MAX_ITERATIONS_WITHOUT_CHECK: i64 = 5000;

struct Loop<'a> {
    header: BlockId,
    /// The blocks that get back to the header. The block after a loop can have the loop's `loop_index`.
    blocks: &'a [BlockId],
}

impl Loop<'_> {
    fn contains(&self, block: BlockId) -> bool {
        self.blocks.contains(&block)
    }

    /// Whether a value is the same in every iteration.
    fn is_invariant(&self, func: &Func, value: Value) -> bool {
        func.node(value).constant().is_some()
            || self.blocks.iter().all(|&block| {
                let data = &func.blocks[block as usize];
                !(data.start..data.end).contains(&value) && !func.phis(block).any(|phi| phi == value)
            })
    }
}

/// What a branch at the end of a block of the loop knows of `counter`: the bound, and the comparison of the counter with
/// it that keeps the loop going.
fn test_of(func: &Func, loop_: &Loop, block: BlockId, counters: [Value; 2]) -> Option<(Value, IntCondition)> {
    let [first, second] = *func.successors(block) else {
        return None;
    };
    let stays_if_true = match (loop_.contains(first), loop_.contains(second)) {
        (true, false) => true,
        (false, true) => false,
        _ => return None,
    };
    let node = func.node(func.terminator(block));
    let (left, right, condition) = match node.op {
        Op::BranchCompare => (node.args[0], node.args[1], IntCondition::from_flags(node.flags)),
        Op::Branch => {
            let tested = func.resolve(node.args[0]);
            let tested_node = func.node(tested);
            match tested_node.op {
                Op::Compare => (
                    tested_node.args[0],
                    tested_node.args[1],
                    IntCondition::from_flags(tested_node.flags),
                ),
                Op::Eqz => (tested_node.args[0], NONE, IntCondition::Eq),
                _ => (tested, NONE, IntCondition::Ne),
            }
        }
        _ => return None,
    };
    let left = func.resolve(left);
    // NB: The zero of a plain branch is a bound that no node holds.
    let right = if right == NONE { NONE } else { func.resolve(right) };
    let (bound, condition) = if counters.contains(&left) {
        (right, condition)
    } else if right != NONE && counters.contains(&right) {
        (left, condition.swap())
    } else {
        return None;
    };
    Some((bound, if stays_if_true { condition } else { condition.invert() }))
}

/// By how much `next` changes `phi`, if that is one up or down.
fn step_of(func: &Func, phi: Value, next: Value) -> Option<i32> {
    let node = func.node(next);
    // NB: Only an addition or a subtraction has two values as its arguments; a constant's first argument is its value.
    if node.ty != Type::I32 || !matches!(node.op, Op::Add | Op::Sub) {
        return None;
    }
    let (left, right) = (func.resolve(node.args[0]), func.resolve(node.args[1]));
    let step = match node.op {
        Op::Add if left == phi => func.node(right).constant()? as u32 as i32,
        Op::Add if right == phi => func.node(left).constant()? as u32 as i32,
        Op::Sub if left == phi => (func.node(right).constant()? as u32 as i32).wrapping_neg(),
        _ => return None,
    };
    (step == 1 || step == -1).then_some(step)
}

/// How far `start` is from `bound` in steps, if the comparison of the two as it says tells that.
fn distance(start: u32, bound: u32, step: i32, condition: IntCondition) -> Option<i64> {
    let (start, bound) = match condition {
        IntCondition::LtU | IntCondition::GtU => (i64::from(start), i64::from(bound)),
        _ => (i64::from(start as i32), i64::from(bound as i32)),
    };
    let distance = (bound - start) * i64::from(step);
    // NB: A test of `!=` that starts past its bound goes all the way around.
    (condition != IntCondition::Ne || distance >= 0).then_some(distance)
}

/// How long a loop that counts with `phi` takes.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum Trip {
    /// It ends by itself.
    Counted,
    /// It ends within MAX_ITERATIONS_WITHOUT_CHECK iterations.
    Short,
}

fn trip_of(func: &Func, dominators: &Dominators, loop_: &Loop, back_edges: &[usize], phi: Value) -> Option<Trip> {
    let inputs = func.inputs(phi);
    let predecessors = func.predecessors(loop_.header);
    let mut direction = 0;
    let mut next = NONE;
    for &edge in back_edges {
        let input = func.resolve(inputs[edge]);
        let step = step_of(func, phi, input)?;
        if (direction != 0 && step != direction) || (next != NONE && next != input) {
            return None;
        }
        direction = step;
        next = input;
    }
    let test = loop_.blocks.iter().find_map(|&block| {
        if !back_edges
            .iter()
            .all(|&edge| dominators.dominates(block, predecessors[edge]))
        {
            return None;
        }
        let (bound, condition) = test_of(func, loop_, block, [phi, next])?;
        let can_go_past = match (direction, condition) {
            (1, IntCondition::LtS | IntCondition::LtU) | (-1, IntCondition::GtS | IntCondition::GtU) => false,
            (_, IntCondition::Ne) => bound != NONE && !loop_.is_invariant(func, bound),
            _ => true,
        };
        (!can_go_past).then_some((bound, condition))
    });
    let (bound, condition) = test?;
    let bound = if bound == NONE {
        Some(0)
    } else {
        func.node(bound).constant()
    };
    let is_short = bound.is_some_and(|bound| {
        inputs
            .iter()
            .enumerate()
            .filter(|(edge, _)| !back_edges.contains(edge))
            .all(|(_, start)| {
                func.node(func.resolve(*start)).constant().is_some_and(|start| {
                    distance(start as u32, bound as u32, direction, condition)
                        .is_some_and(|distance| distance < MAX_ITERATIONS_WITHOUT_CHECK)
                })
            })
    });
    Some(if is_short { Trip::Short } else { Trip::Counted })
}

/// Marks the checks that go as dead, which keeps `schedule` from placing them.
pub fn run(func: &Func, liveliness: &mut Liveliness) {
    if func.loops.is_empty() {
        return;
    }
    let mut dominators = Dominators::default();
    dominators.compute(func);
    let mut has_loop_inside = vec![false; func.loops.len()];
    for loop_ in &func.loops {
        if loop_.parent != NONE {
            has_loop_inside[loop_.parent as usize] = true;
        }
    }
    let mut blocks = Vec::new();
    let mut back_edges = Vec::new();
    for (index, loop_) in func.loops.iter().enumerate() {
        if has_loop_inside[index] {
            continue;
        }
        back_edges.clear();
        back_edges.extend(
            func.predecessors(loop_.header)
                .iter()
                .enumerate()
                .filter(|(_, predecessor)| **predecessor >= loop_.header)
                .map(|(edge, _)| edge),
        );
        blocks.clear();
        blocks.push(loop_.header);
        for &edge in &back_edges {
            let source = func.predecessors(loop_.header)[edge];
            if !blocks.contains(&source) {
                blocks.push(source);
            }
        }
        let mut next = 1;
        while next < blocks.len() {
            for &predecessor in func.predecessors(blocks[next]) {
                if !blocks.contains(&predecessor) {
                    blocks.push(predecessor);
                }
            }
            next += 1;
        }
        let mut size = 0;
        let mut calls = false;
        for &block in &blocks {
            let data = &func.blocks[block as usize];
            size += data.end - data.start;
            calls |= (data.start..data.end)
                .any(|value| !liveliness.is_dead(value) && matches!(func.node(value).op, Op::Call | Op::Helper));
        }
        if calls || size > MAX_LOOP_SIZE {
            continue;
        }
        let current = Loop {
            header: loop_.header,
            blocks: &blocks,
        };
        let best_trip = func
            .phis(loop_.header)
            .filter(|phi| !liveliness.is_dead(*phi) && func.node(*phi).op == Op::Phi && func.node(*phi).ty == Type::I32)
            .filter_map(|phi| trip_of(func, &dominators, &current, &back_edges, phi))
            .max();
        let may_lose_check = match best_trip {
            Some(Trip::Short) => true,
            Some(Trip::Counted) => loop_.parent != NONE,
            None => false,
        };
        if !may_lose_check {
            continue;
        }
        let header = &func.blocks[loop_.header as usize];
        for value in header.start..header.end {
            if func.node(value).op == Op::CheckInterrupt {
                liveliness.dead[value as usize] = true;
            }
        }
    }
}
