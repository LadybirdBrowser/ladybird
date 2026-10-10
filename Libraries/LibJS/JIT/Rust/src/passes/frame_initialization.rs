/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Lazy frame publication and initialization.
//!
//! Direct calls of compiled functions build their callee's frame without
//! making it the running execution context. Until the code publishes it,
//! the frame is invisible: the garbage collector, stack walks and runtime
//! functions only see the running frame and its callers. So the frame must
//! be published before every node that may let other code see it, which
//! are those that run other code, may collect garbage or may exit lazily.
//! Exits and the prologue's resumption in the interpreter publish it
//! themselves.
//!
//! The `Enter` bytecode empties the registers and locals of the frame and
//! copies the constants into it, so that the interpreter, its slow paths and
//! the garbage collector (which only visits the reserved registers and the
//! arguments of frames that are not initialized) see a valid frame. Compiled
//! code keeps its values in machine registers and stack slots, so it only
//! needs an initialized frame where something can observe the frame's slots:
//! stores of values to them, slow paths and calls, and materialized frames
//! of inlined calls. Exits initialize the frame in the runtime if needed.
//!
//! This pass moves the `InitializeFrame` the builder put at `Enter` to right
//! before the first node that observes the frame's slots on each path:
//! where the frame is known not to be initialized yet as `InitializeFrame`,
//! and where it may be (after merges of paths that did and did not
//! initialize it, as in loops) as `EnsureFrameInitialized`, which checks
//! first. Both publish the frame too. Register-saving slow path calls where
//! the frame may not be initialized initialize it themselves before they
//! run. Before nodes that only need the frame published, where it may not
//! be, it puts a `PublishFrame`.

use crate::bytecode::FrameLayout;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;

/// Whether something holds on the paths reaching a point.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
enum Holds {
    No,
    Yes,
    /// On some paths, not on others.
    Maybe,
}

impl Holds {
    fn merge(self, other: Holds) -> Holds {
        if self == other { self } else { Holds::Maybe }
    }
}

/// What is known of the frame at a point. An initialized frame is published.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct State {
    published: Holds,
    initialized: Holds,
}

impl State {
    fn merge(self, other: State) -> State {
        State {
            published: self.published.merge(other.published),
            initialized: self.initialized.merge(other.initialized),
        }
    }

    /// The node to put before a node with `need`, if any, and the state
    /// after both.
    fn satisfy(self, need: &Need) -> (Option<Op>, State) {
        const INITIALIZED: State = State {
            published: Holds::Yes,
            initialized: Holds::Yes,
        };
        match (need, self.initialized, self.published) {
            (Need::Initialized, Holds::No, _) => (Some(Op::InitializeFrame), INITIALIZED),
            (Need::Initialized, Holds::Maybe, _) => (Some(Op::EnsureFrameInitialized), INITIALIZED),
            (Need::Published, _, Holds::No | Holds::Maybe) => (
                Some(Op::PublishFrame),
                State {
                    published: Holds::Yes,
                    ..self
                },
            ),
            _ => (None, self),
        }
    }
}

/// What a node needs of the frame.
enum Need {
    Nothing,
    /// The frame must be published before the node runs.
    Published,
    /// The frame must be initialized before the node runs.
    Initialized,
    /// The node's out of line slow path needs an initialized frame.
    InitializedForSlowPath,
}

fn need(node: &Node, layout: &FrameLayout) -> Need {
    // Reserved registers and arguments are valid in frames that are not
    // initialized, and the garbage collector visits them.
    let observes_slot = |slot: u32| slot >= RESERVED_REGISTER_COUNT && slot < layout.arguments_base();
    match &node.op {
        Op::StoreSlot { slot } | Op::LoadSlot { slot } => {
            if observes_slot(*slot) {
                Need::Initialized
            } else {
                Need::Nothing
            }
        }
        Op::CallSlowPath {
            saves_registers: false,
            executable: 0,
            ..
        } => Need::Initialized,
        Op::CallSlowPath { executable: 0, .. } => Need::InitializedForSlowPath,
        // NB: Slow paths and calls in inlined callees run in the frames of
        //     the inlined calls (or without them), which are linked to the
        //     compiled function's frame, and only observe its slots through
        //     those frames, whose full translation initializes it.
        Op::CallSlowPath { .. } => Need::Published,
        Op::CallDirect { executable, .. } | Op::CallNative { executable, .. } if *executable != 0 => Need::Published,
        // NB: The helper only looks at its argument.
        Op::ToBoolean => Need::Nothing,
        op => {
            let properties = op.properties();
            if properties.is_call || properties.can_lazy_exit {
                Need::Initialized
            } else if properties.allocates {
                Need::Published
            } else {
                Need::Nothing
            }
        }
    }
}

pub fn run(graph: &mut Graph, layout: &FrameLayout) {
    // Only functions entered through `Enter` initialize their frame.
    let Some((entry_block, entry_node)) = graph.blocks.iter().enumerate().find_map(|(index, block)| {
        block
            .body
            .iter()
            .find(|node| graph.node(**node).op == Op::InitializeFrame)
            .map(|node| (index, *node))
    }) else {
        return;
    };
    graph.blocks[entry_block].body.retain(|node| *node != entry_node);
    let entry_pc = graph.node(entry_node).pc;

    let block_count = graph.blocks.len();
    let osr_blocks = graph.osr_entries.iter().map(|(_, block)| *block).collect::<Vec<_>>();
    let mut end_states: Vec<Option<State>> = vec![None; block_count];
    // Where to put which initialization, as (block, index in its body).
    let mut insertions: Vec<(usize, usize, Op)>;
    let mut slow_paths: Vec<NodeId>;
    loop {
        insertions = Vec::new();
        slow_paths = Vec::new();
        let mut changed = false;
        for block in 0..block_count {
            let block_id = BlockId::from_index(block);
            let predecessors = &graph.blocks[block].predecessors;
            let state = if osr_blocks.contains(&block_id) {
                // The interpreter ran `Enter` for frames entering here, which
                // are the running frame.
                Some(State {
                    published: Holds::Yes,
                    initialized: Holds::Yes,
                })
            } else if predecessors.is_empty() {
                Some(State {
                    published: Holds::No,
                    initialized: Holds::No,
                })
            } else {
                predecessors
                    .iter()
                    .filter_map(|predecessor| end_states[predecessor.index()])
                    .reduce(State::merge)
            };
            let Some(mut current) = state else {
                continue;
            };
            let body = &graph.blocks[block].body;
            let nodes = body
                .iter()
                .enumerate()
                .chain(graph.blocks[block].control.iter().map(|control| (body.len(), control)));
            for (index, node) in nodes {
                let need = need(graph.node(*node), layout);
                if let Need::InitializedForSlowPath = need {
                    if current.initialized != Holds::Yes {
                        slow_paths.push(*node);
                    }
                    continue;
                }
                let (op, after) = current.satisfy(&need);
                if let Some(op) = op {
                    insertions.push((block, index, op));
                }
                current = after;
            }
            if end_states[block] != Some(current) {
                end_states[block] = Some(current);
                changed = true;
            }
        }
        if !changed {
            break;
        }
    }

    // Insert from the back so earlier indices stay valid.
    insertions.sort_by_key(|(block, index, _)| (*block, *index));
    for (block, index, op) in insertions.into_iter().rev() {
        let node = graph.add_node(Node::new(op, Vec::new(), None, entry_pc));
        graph.blocks[block].body.insert(index, node);
    }
    graph.slow_paths_initializing_frame = slow_paths;
}
