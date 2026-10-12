/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Spill slot coalescing: phis share their stack slots with their inputs.
//!
//! Where a phi and the value flowing into it both live in stack slots, the
//! predecessor copies the value from one slot to the other at its end. Loops
//! carrying many values around, and merges of many values, then copy them
//! all on every iteration. Phis and inputs whose live ranges do not overlap
//! can use the same slot instead, which leaves nothing to copy.
//!
//! Two SSA values overlap exactly where one is live right after the other
//! is defined. Values join a class of values sharing one slot only if they
//! overlap with no value already in it.

use super::liveness::ValueLiveness;
use super::liveness::is_allocated_value;
use crate::ir::Graph;
use crate::ir::NodeId;

/// The most values one class holds, which keeps the pairwise checks cheap.
const MAX_CLASS_SIZE: usize = 32;

/// Where a value is defined: its block, and its position in linear order
/// (`None` for phis, which are defined where their block starts).
#[derive(Clone, Copy)]
struct Definition {
    block: usize,
    position: Option<u32>,
}

struct Interference<'a> {
    graph: &'a Graph,
    liveness: &'a ValueLiveness,
    definitions: Vec<Definition>,
    /// For each value, the blocks where it dies and the positions there.
    deaths: Vec<Vec<(usize, u32)>>,
}

impl Interference<'_> {
    /// Whether `value` is live right after `defined` is defined.
    fn is_live_after_definition_of(&self, value: NodeId, defined: NodeId) -> bool {
        let at = self.definitions[defined.index()];
        let of_value = self.definitions[value.index()];
        let available = self.liveness.live_in[at.block].contains(value.index())
            || (of_value.block == at.block
                && match (of_value.position, at.position) {
                    (None, _) => true,
                    (Some(_), None) => false,
                    (Some(value_position), Some(position)) => value_position < position,
                });
        let Some(position) = at.position else {
            return available;
        };
        if !available {
            return false;
        }
        // NB: Lazy exits of calls read their frame state values after the
        //     call defined its value.
        let read_after_definition = || {
            let node = self.graph.node(defined);
            node.op.properties().is_call
                && node.frame_state.is_some_and(|frame_state| {
                    self.graph
                        .frame_state_values(frame_state)
                        .any(|(_, frame_value)| frame_value == value)
                })
        };
        self.liveness.live_out[at.block].contains(value.index())
            || self.deaths[value.index()].iter().any(|(block, death)| {
                *block == at.block && (*death > position || (*death == position && read_after_definition()))
            })
    }

    fn overlap(&self, a: NodeId, b: NodeId) -> bool {
        self.is_live_after_definition_of(a, b) || self.is_live_after_definition_of(b, a)
    }
}

/// The class of each value: values of one class share their spill slot.
/// Classes are named by one of their values.
pub(super) fn spill_slot_classes(graph: &Graph, liveness: &ValueLiveness) -> Vec<u32> {
    let count = graph.nodes.len();
    let mut classes = (0..count as u32).collect::<Vec<_>>();
    let mut members = vec![Vec::new(); count];
    let mut definitions = vec![
        Definition {
            block: 0,
            position: None
        };
        count
    ];
    let mut deaths = vec![Vec::new(); count];
    let mut has_phis = false;
    for (index, block) in graph.blocks.iter().enumerate() {
        has_phis |= !block.phis.is_empty();
        for phi in &block.phis {
            definitions[phi.index()] = Definition {
                block: index,
                position: None,
            };
        }
        for node in block.body.iter().chain(&block.control) {
            let position = liveness.positions[node.index()];
            definitions[node.index()] = Definition {
                block: index,
                position: Some(position),
            };
            for value in &liveness.dies_at[node.index()] {
                deaths[value.index()].push((index, position));
            }
        }
    }
    if !has_phis {
        return classes;
    }
    for (index, member) in members.iter_mut().enumerate() {
        member.push(NodeId::from_index(index));
    }
    let interference = Interference {
        graph,
        liveness,
        definitions,
        deaths,
    };
    for block in &graph.blocks {
        for phi in &block.phis {
            for input in &graph.node(*phi).inputs {
                if input == phi || !is_allocated_value(graph, *input) {
                    continue;
                }
                let (a, b) = (classes[phi.index()] as usize, classes[input.index()] as usize);
                if a == b || members[a].len() + members[b].len() > MAX_CLASS_SIZE {
                    continue;
                }
                let overlapping = members[a]
                    .iter()
                    .any(|x| members[b].iter().any(|y| interference.overlap(*x, *y)));
                if overlapping {
                    continue;
                }
                let moved = std::mem::take(&mut members[b]);
                for value in &moved {
                    classes[value.index()] = a as u32;
                }
                members[a].extend(moved);
            }
        }
    }
    classes
}
