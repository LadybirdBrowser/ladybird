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

use ak::Zone;
use ak::ZoneVec;

use super::liveness::ValueLiveness;
use super::liveness::is_allocated_value;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::tables::Lists;

/// The most values one class holds, which keeps the pairwise checks cheap.
const MAX_CLASS_SIZE: u32 = 32;

/// Where a value is defined: its block, and its position in linear order
/// (`None` for phis, which are defined where their block starts).
#[derive(Clone, Copy)]
struct Definition {
    block: usize,
    position: Option<u32>,
}

/// The spill slot classes of the values of a graph, in tables in the work zone of a compile.
pub(super) struct Coalescing<'w> {
    /// The class of each value: values of one class share their spill slot.
    /// Classes are named by one of their values.
    pub classes: ZoneVec<'w, u32>,
    /// For each value, the next value of its class. The values of a class are a ring, so that two classes join with
    /// no list to copy.
    next: ZoneVec<'w, u32>,
    /// For each value that names a class, how many values the class has.
    sizes: ZoneVec<'w, u32>,
    definitions: ZoneVec<'w, Definition>,
    /// For each value, the blocks where it dies and the positions there.
    deaths: Lists<'w, (usize, u32)>,
}

struct Interference<'a> {
    graph: &'a Graph<'a>,
    liveness: &'a ValueLiveness<'a>,
    definitions: &'a [Definition],
    deaths: &'a Lists<'a, (usize, u32)>,
    work: &'a Zone,
}

impl Interference<'_> {
    /// Whether `value` is live right after `defined` is defined.
    fn is_live_after_definition_of(&self, value: NodeId, defined: NodeId) -> bool {
        let at = self.definitions[defined.index()];
        let of_value = self.definitions[value.index()];
        let at_block = BlockId::from_index(at.block);
        let available = self.liveness.is_live_in(at_block, value)
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
                        .frame_state_values(frame_state, self.work)
                        .any(|(_, frame_value)| frame_value == value)
                })
        };
        self.liveness.is_live_out(at_block, value)
            || self.deaths.get(value.index()).iter().any(|(block, death)| {
                *block == at.block && (*death > position || (*death == position && read_after_definition()))
            })
    }

    fn overlap(&self, a: NodeId, b: NodeId) -> bool {
        self.is_live_after_definition_of(a, b) || self.is_live_after_definition_of(b, a)
    }
}

/// The values of the class of `value`, which starts with `value`.
fn members(next: &[u32], value: u32) -> impl Iterator<Item = NodeId> + '_ {
    let mut member = Some(value);
    core::iter::from_fn(move || {
        let current = member?;
        let following = next[current as usize];
        member = (following != value).then_some(following);
        Some(NodeId(current))
    })
}

impl<'w> Coalescing<'w> {
    /// Finds the class of each value of `graph`.
    pub fn compute(graph: &Graph, liveness: &ValueLiveness, work: &'w Zone) -> Self {
        let count = graph.nodes.len();
        let mut coalescing = Self {
            classes: ZoneVec::from_iter_in(0..count as u32, work),
            next: ZoneVec::new_in(work),
            sizes: ZoneVec::new_in(work),
            definitions: ZoneVec::new_in(work),
            deaths: Lists::new_in(work),
        };
        // NB: Without phis, each value is a class of its own.
        if graph.blocks.iter().all(|block| block.phis.is_empty()) {
            return coalescing;
        }
        coalescing.join_classes(graph, liveness, work);
        coalescing
    }

    /// Puts phis and their inputs in one class where they do not overlap.
    fn join_classes(&mut self, graph: &Graph, liveness: &ValueLiveness, work: &Zone) {
        let count = graph.nodes.len();
        self.definitions.resize(
            count,
            Definition {
                block: 0,
                position: None,
            },
        );
        self.deaths.begin_counts(count);
        for (index, block) in graph.blocks.iter().enumerate() {
            for phi in &block.phis {
                self.definitions[phi.index()] = Definition {
                    block: index,
                    position: None,
                };
            }
            for node in block.body.iter().chain(&block.control) {
                self.definitions[node.index()] = Definition {
                    block: index,
                    position: Some(liveness.positions[node.index()]),
                };
                for value in liveness.dies_at(*node) {
                    self.deaths.count(value.index());
                }
            }
        }
        self.deaths.end_counts((0, 0));
        for (index, block) in graph.blocks.iter().enumerate() {
            for node in block.body.iter().chain(&block.control) {
                let position = liveness.positions[node.index()];
                for value in liveness.dies_at(*node) {
                    self.deaths.fill(value.index(), (index, position));
                }
            }
        }
        self.deaths.end_fill();
        self.next.extend_from_slice(&self.classes);
        self.sizes.resize(count, 1);

        let Self {
            classes,
            next,
            sizes,
            definitions,
            deaths,
        } = self;
        let interference = Interference {
            graph,
            liveness,
            definitions,
            deaths,
            work,
        };
        for block in &graph.blocks {
            for phi in &block.phis {
                for input in &graph.node(*phi).inputs {
                    // NB: Values with a frame slot as their home have no spill slot to share.
                    if input == phi || !is_allocated_value(graph, *input) || super::frame_home(graph, *input).is_some()
                    {
                        continue;
                    }
                    let (a, b) = (classes[phi.index()], classes[input.index()]);
                    if a == b || sizes[a as usize] + sizes[b as usize] > MAX_CLASS_SIZE {
                        continue;
                    }
                    let overlapping = members(next, a).any(|x| members(next, b).any(|y| interference.overlap(x, y)));
                    if overlapping {
                        continue;
                    }
                    let mut member = b;
                    loop {
                        classes[member as usize] = a;
                        member = next[member as usize];
                        if member == b {
                            break;
                        }
                    }
                    // NB: This makes one ring of the two.
                    next.swap(a as usize, b as usize);
                    sizes[a as usize] += sizes[b as usize];
                }
            }
        }
    }
}
