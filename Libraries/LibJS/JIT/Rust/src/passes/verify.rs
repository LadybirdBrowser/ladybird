/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Checks the structural invariants of a graph that every pass relies on
//! and must preserve:
//!
//! - every block ends in one control node, has only phis among its phis and
//!   no phis or control nodes in its body, and no node is in two places;
//! - predecessor lists match the successors of the predecessors' control
//!   nodes, phis have one input per predecessor, and the predecessors of
//!   blocks with phis lead nowhere else (so moves into the phis have a place
//!   at the end of each predecessor);
//! - blocks come after their predecessors in linear order, except loop
//!   headers reached over back edges and blocks that cold blocks rejoin,
//!   and an entry (block 0 or an on-stack replacement entry, exactly the
//!   blocks without predecessors) reaches every block;
//! - every input and every frame state value is defined before it is used:
//!   earlier in the same block, or in a block dominating the use (for phi
//!   inputs, dominating the end of the predecessor the input flows in from);
//! - nodes have as many inputs as their op takes, with the representations
//!   it expects of them, and jumps have none;
//! - no branch tests shapes (only `ShapeSwitch` does);
//! - every node that can exit, eagerly or lazily, has a frame state, and frame states list
//!   each slot once (with a value or as held by the frame), with values
//!   that have a representation (or virtual objects);
//! - slow path calls have a frame state, and those that save registers
//!   are in cold blocks;
//! - `StoreSlot` writes only written constants, the arguments the compiled
//!   function's legacy `arguments` shows, and slots that the next call
//!   reading the frame (a `Generic` node, or a call that reads its operands
//!   from the frame) or `Suspend` reads (see `verify_frame_writes()`), and
//!   never one of `Graph::unwritten_slots`.

use ak::Zone;
use ak::ZoneVec;
use core::fmt;

use super::dominators::Dominators;
use crate::bytecode::FrameLayout;
use crate::bytecode::Operand;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::code::Repr;
use crate::debug_text::DebugText;
use crate::ir::BlockId;
use crate::ir::ElementsKind;
use crate::ir::Graph;
use crate::ir::Locations;
use crate::ir::NodeId;
use crate::ir::Op;

/// The descriptions of the violated invariants, one on each line.
struct Errors<'w>(DebugText<'w>);

impl<'w> Errors<'w> {
    fn new_in(work: &'w Zone) -> Self {
        Self(DebugText::new_in(work))
    }

    /// Adds the description of a violated invariant. This is what `write!` calls.
    fn write_fmt(&mut self, message: fmt::Arguments<'_>) {
        if !self.0.is_empty() {
            self.0.push('\n');
        }
        self.0.write_fmt(message);
    }

    fn finish(self) -> Result<(), DebugText<'w>> {
        if self.0.is_empty() { Ok(()) } else { Err(self.0) }
    }
}

/// Where a node is: its block and its position there (phis come first, then
/// the body, then the control node).
#[derive(Debug, Clone, Copy)]
struct Place {
    block: BlockId,
    index: usize,
}

/// Checks that `StoreSlot` writes frame memory only where something reads it: written constants, which live there,
/// the arguments the compiled function's legacy `arguments` shows, and the slots a node that reads the frame reads,
/// right before it. Merges and back edges never write frame memory: slots that differ get phis.
pub fn verify_frame_writes<'w>(graph: &Graph, layout: &FrameLayout, work: &'w Zone) -> Result<(), DebugText<'w>> {
    let mut errors = Errors::new_in(work);
    let mut nodes = ZoneVec::new_in(work);
    for (index, block) in graph.blocks.iter().enumerate() {
        nodes.assign(&block.body);
        nodes.extend(block.control);
        for (position, node) in nodes.iter().enumerate() {
            let Op::StoreSlot { slot } = graph.node(*node).op else {
                continue;
            };
            let operand = Operand::from_raw(slot).raw();
            if graph.is_unwritten_slot(operand) {
                write!(
                    errors,
                    "StoreSlot v{} in b{index} writes slot {operand}, which nothing writes (see `Graph::unwritten_slots`)",
                    node.0
                );
            }
            let is_constant = operand >= layout.registers_and_locals_count && operand < layout.arguments_base();
            if is_constant || graph.legacy_arguments.contains(&operand) {
                continue;
            }
            // NB: Conversions of the reader's inputs may come in between, and
            //     so may the frame's initialization for stores of arguments
            //     and reserved registers, which it leaves as they are (see
            //     `frame_initialization`).
            let survives_initialization = operand >= layout.arguments_base() || operand < RESERVED_REGISTER_COUNT;
            let reader = nodes[position + 1..].iter().find(|next| {
                let op = &graph.node(**next).op;
                let properties = op.properties();
                let initializes_frame =
                    matches!(op, Op::InitializeFrame | Op::EnsureFrameInitialized | Op::PublishFrame);
                !matches!(op, Op::StoreSlot { .. })
                    && !(survives_initialization && initializes_frame)
                    && (properties.reads != Locations::NONE || properties.writes != Locations::NONE)
            });
            let reads_frame = reader.is_some_and(|reader| {
                let op = &graph.node(*reader).op;
                let properties = op.properties();
                (properties.is_call && properties.reads.intersects(Locations::FRAME))
                    || matches!(op, Op::Suspend { .. })
            });
            if !reads_frame {
                write!(
                    errors,
                    "StoreSlot v{} in b{index} writes a slot that no node reading the frame reads next",
                    node.0
                );
            }
        }
    }
    errors.finish()
}

/// Returns a description of every violated invariant, one on each line, or `Ok` if there is none. The description and
/// the lists of the checks are in `work`.
pub fn verify_with<'w>(graph: &Graph, work: &'w Zone) -> Result<(), DebugText<'w>> {
    let places = verify_blocks(graph, work)?;
    verify_edges(graph, work)?;
    verify_nodes(graph, &places, work)?;
    verify_frame_states(graph, work)
}

/// Checks what each block holds, and returns where each node is.
fn verify_blocks<'w>(graph: &Graph, work: &'w Zone) -> Result<ZoneVec<'w, Option<Place>>, DebugText<'w>> {
    let mut errors = Errors::new_in(work);
    let mut places: ZoneVec<Option<Place>> = ZoneVec::from_elem_in(None, graph.nodes.len(), work);
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        let Some(control) = block.control else {
            write!(errors, "b{index} has no control node");
            continue;
        };
        if !graph.node(control).op.is_control() {
            write!(errors, "b{index} ends in v{}, which is not a control node", control.0);
        }
        for phi in &block.phis {
            if graph.node(*phi).op != Op::Phi {
                write!(errors, "b{index} has v{} among its phis", phi.0);
            }
            if graph.node(*phi).inputs.len() != block.predecessors.len() {
                write!(
                    errors,
                    "phi v{} of b{index} has {} inputs for {} predecessors",
                    phi.0,
                    graph.node(*phi).inputs.len(),
                    block.predecessors.len()
                );
            }
        }
        for node in &block.body {
            let op = &graph.node(*node).op;
            if *op == Op::Phi || op.is_control() || matches!(op, Op::Constant(_)) {
                write!(errors, "b{index} has {} v{} in its body", op.name(), node.0);
            }
        }
        for (position, node) in graph.block_nodes(block_id).enumerate() {
            match places[node.index()] {
                Some(other) => write!(errors, "v{} is in b{} and in b{index}", node.0, other.block.0),
                None => {
                    places[node.index()] = Some(Place {
                        block: block_id,
                        index: position,
                    });
                }
            }
        }
    }
    errors.finish().map(|()| places)
}

/// Checks the edges between blocks and their order.
fn verify_edges<'w>(graph: &Graph, work: &'w Zone) -> Result<(), DebugText<'w>> {
    let mut errors = Errors::new_in(work);
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        for successor in graph.successors(block_id) {
            let edges_out = graph.successors(block_id).iter().filter(|s| **s == successor).count();
            let edges_in = graph
                .block(successor)
                .predecessors
                .iter()
                .filter(|predecessor| **predecessor == block_id)
                .count();
            if edges_out != edges_in {
                write!(
                    errors,
                    "b{index} has {edges_out} edges to b{}, which lists it {edges_in} times",
                    successor.0
                );
            }
        }
        if !block.phis.is_empty() {
            for predecessor in &block.predecessors {
                if graph.successors(*predecessor).len() != 1 {
                    write!(
                        errors,
                        "b{index} has phis, but its predecessor b{} also leads elsewhere",
                        predecessor.0
                    );
                }
            }
        }
        for predecessor in &block.predecessors {
            if !graph.successors(*predecessor).contains(&block_id) {
                write!(
                    errors,
                    "b{index} lists b{} as a predecessor, which does not branch to it",
                    predecessor.0
                );
            }
            // NB: Cold code comes after the code it rejoins.
            if predecessor.index() >= index
                && !block.is_loop_header
                && !(graph.block(*predecessor).is_cold && !block.is_cold)
            {
                write!(
                    errors,
                    "b{index} is reached from the later b{} but is no loop header",
                    predecessor.0
                );
            }
        }
    }
    errors.finish()
}

/// Checks that values are defined before their uses, and the rules of each
/// kind of node.
fn verify_nodes<'w>(graph: &Graph, places: &[Option<Place>], work: &'w Zone) -> Result<(), DebugText<'w>> {
    let mut errors = Errors::new_in(work);
    if graph.blocks.len() > Dominators::MAX_BLOCKS {
        write!(
            errors,
            "the graph has more blocks than the tables of the dominators hold"
        );
        return errors.finish();
    }
    let dominators = &Dominators::of(graph, work);
    let is_defined_before = |value: NodeId, block: BlockId, index: usize| -> bool {
        // NB: Virtual objects are in no block; their properties are listed
        //     with the frame state values that need them.
        if matches!(graph.node(value).op, Op::Constant(_) | Op::VirtualObject { .. }) {
            return true;
        }
        let Some(place) = places[value.index()] else {
            return false;
        };
        if place.block == block {
            return place.index < index;
        }
        dominators.dominates(place.block, block)
    };
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        let is_entry = graph.entries().any(|entry| entry == block_id);
        if block.predecessors.is_empty() != is_entry {
            write!(
                errors,
                "b{index} has {} predecessors, but is {}",
                block.predecessors.len(),
                if is_entry { "an entry" } else { "no entry" }
            );
        }
        if !dominators.is_reachable(block_id) {
            write!(errors, "b{index} is reachable from no entry");
            continue;
        }
        for phi in &block.phis {
            for (input, predecessor) in graph.node(*phi).inputs.iter().zip(&block.predecessors) {
                if graph.node(*input).repr != graph.node(*phi).repr {
                    write!(
                        errors,
                        "phi v{} of b{index} is {:?}, but its input v{} is {:?}",
                        phi.0,
                        graph.node(*phi).repr,
                        input.0,
                        graph.node(*input).repr
                    );
                }
                let end = graph.block_nodes(*predecessor).count();
                if !is_defined_before(*input, *predecessor, end) {
                    write!(
                        errors,
                        "phi v{} of b{index} uses v{} from b{}, where it is not defined",
                        phi.0, input.0, predecessor.0
                    );
                }
            }
        }
        for (position, node_id) in graph.block_nodes(block_id).enumerate() {
            let node = graph.node(node_id);
            if node.op == Op::Phi {
                continue;
            }
            for input in &node.inputs {
                if !is_defined_before(*input, block_id, position) {
                    write!(
                        errors,
                        "v{} ({}) in b{index} uses v{}, which is not defined before it",
                        node_id.0,
                        node.op.name(),
                        input.0
                    );
                } else if graph.node(*input).repr.is_none() {
                    write!(
                        errors,
                        "v{} ({}) uses v{}, which has no value",
                        node_id.0,
                        node.op.name(),
                        input.0
                    );
                }
            }
            // NB: Lazy exits of calls find their values after the call, but
            //     those are defined before it all the same.
            if let Some(frame_state) = node.frame_state {
                for (slot, value) in graph.frame_state_values(frame_state, work) {
                    if graph.node(value).repr.is_none() && !matches!(graph.node(value).op, Op::VirtualObject { .. }) {
                        write!(
                            errors,
                            "the frame state of v{} has v{} for slot {slot}, which has no value",
                            node_id.0, value.0
                        );
                    }
                    // NB: Only the environments of virtual closures are cell addresses (see `VirtualObjectKind`).
                    if graph.node(value).repr == Some(Repr::Pointer) && slot != crate::ir::VIRTUAL_OBJECT_PROPERTY {
                        write!(
                            errors,
                            "the frame state of v{} has the cell address v{} for slot {slot}",
                            node_id.0, value.0
                        );
                    }
                    if !is_defined_before(value, block_id, position) {
                        write!(
                            errors,
                            "the frame state of v{} ({}) in b{index} has v{} for slot {slot}, which is not defined before it",
                            node_id.0,
                            node.op.name(),
                            value.0
                        );
                    }
                }
            }
            check_input_reprs(graph, node_id, &mut errors);
            if (node.op.can_eager_exit() || node.op.properties().can_lazy_exit) && node.frame_state.is_none() {
                write!(
                    errors,
                    "v{} ({}) can exit, but has no frame state",
                    node_id.0,
                    node.op.name()
                );
            }
            match node.op {
                Op::Jump { .. } if !node.inputs.is_empty() => {
                    write!(errors, "v{} (Jump) has inputs", node_id.0);
                }
                Op::Branch {
                    condition: crate::ir::BranchCondition::Shape(_),
                    ..
                } => write!(errors, "v{} branches on a shape", node_id.0),
                _ => {}
            }
            if let Op::CallSlowPath { .. } = node.op {
                if node.frame_state.is_none() {
                    write!(errors, "v{} (CallSlowPath) has no frame state", node_id.0);
                }
                if matches!(
                    node.op,
                    Op::CallSlowPath {
                        saves_registers: true,
                        ..
                    }
                ) && !block.is_cold
                {
                    write!(
                        errors,
                        "v{} (CallSlowPath) is in b{index}, which is not cold",
                        node_id.0
                    );
                }
            }
            if let Op::Refine { .. } = node.op {
                let at_head = block.body[..position - block.phis.len()]
                    .iter()
                    .all(|other| matches!(graph.node(*other).op, Op::Refine { .. }));
                if !at_head || block.predecessors.len() != 1 {
                    write!(
                        errors,
                        "v{} (Refine) is not at the head of b{index}, a block with one predecessor",
                        node_id.0
                    );
                }
            }
            if let Some(index) = node.op.refined_input()
                && node.repr.is_some()
                && node.repr != graph.node(node.inputs[index]).repr
            {
                write!(
                    errors,
                    "v{} ({}) changes the representation of the value it refines",
                    node_id.0,
                    node.op.name()
                );
            }
            // NB: Typed-array element accesses take the storage offset from
            //     its load or the check that it is valid, which they rely on.
            if let Op::LoadElementAt {
                kind: ElementsKind::TypedArray(_),
            }
            | Op::StoreElementAt {
                kind: ElementsKind::TypedArray(_),
            } = node.op
                && graph.node(node.inputs[0]).op != Op::TypedArrayDataAddress
            {
                write!(
                    errors,
                    "v{} ({}) takes no typed array data address",
                    node_id.0,
                    node.op.name()
                );
            }
            if node.op == Op::TypedArrayDataAddress
                && !matches!(
                    graph.node(node.inputs[0]).op,
                    Op::LoadTypedArrayDataOffset | Op::CheckTypedArrayStorage
                )
            {
                write!(errors, "v{} (TypedArrayDataAddress) takes no storage offset", node_id.0);
            }
            // A slow path's further outputs are where its call left them only
            // until anything else runs.
            if let Op::SlowPathOutput { .. } = node.op {
                let call = node.inputs[0];
                let before = || graph.block_nodes(block_id).take(position);
                let follows_call = matches!(graph.node(call).op, Op::CallSlowPath { .. })
                    && before().position(|other| other == call).is_some_and(|start| {
                        before()
                            .skip(start + 1)
                            .all(|other| matches!(graph.node(other).op, Op::SlowPathOutput { .. }))
                    });
                if !follows_call {
                    write!(
                        errors,
                        "v{} (SlowPathOutput) does not follow its slow path call v{} directly",
                        node_id.0, call.0
                    );
                }
            }
        }
    }
    errors.finish()
}

/// Checks that frame states list each slot once.
fn verify_frame_states<'w>(graph: &Graph, work: &'w Zone) -> Result<(), DebugText<'w>> {
    let mut errors = Errors::new_in(work);
    let mut slots = ZoneVec::new_in(work);
    for (index, frame_state) in graph.frame_states.iter().enumerate() {
        slots.clear();
        slots.extend(frame_state.values.iter().map(|(slot, _)| *slot));
        slots.extend_from_slice(&frame_state.in_frame);
        slots.sort_unstable();
        if slots.windows(2).any(|pair| pair[0] == pair[1]) {
            write!(errors, "fs{index} lists a slot more than once");
        }
    }
    errors.finish()
}

/// Reports the first input of `node_id` that does not have the representation its op takes, or a wrong count of inputs.
fn check_input_reprs(graph: &Graph, node_id: NodeId, errors: &mut Errors<'_>) {
    let node = graph.node(node_id);
    let expected = match node.op.info().inputs.reprs(node.inputs.len()) {
        Ok(Some(expected)) => expected,
        Ok(None) => return,
        Err(count) => {
            write!(
                errors,
                "v{} ({}) has {} inputs, but takes {count}",
                node_id.0,
                node.op.name(),
                node.inputs.len(),
            );
            return;
        }
    };
    for (input, repr) in node.inputs.iter().zip(expected) {
        let Some(actual) = graph.node(*input).repr else {
            return;
        };
        if actual != repr {
            write!(
                errors,
                "v{} ({}) takes v{} as {repr:?}, but it is {actual:?}",
                node_id.0,
                node.op.name(),
                input.0
            );
            return;
        }
    }
}
