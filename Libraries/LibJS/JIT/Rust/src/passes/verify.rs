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
//! - `StoreSlot` writes only the reserved registers, written constants, and
//!   slots that the next call reading the frame (a `Generic` node, or a call
//!   that reads its operands from the frame) reads (see
//!   `verify_frame_writes()`).

use super::dominators::Dominators;
use crate::bytecode::FrameLayout;
use crate::bytecode::Operand;
use crate::bytecode::RESERVED_REGISTER_COUNT;
use crate::code::Repr;
use crate::ir::BlockId;
use crate::ir::Graph;
use crate::ir::Locations;
use crate::ir::NodeId;
use crate::ir::Op;

/// Where a node is: its block and its position there (phis come first, then
/// the body, then the control node).
#[derive(Debug, Clone, Copy)]
struct Place {
    block: BlockId,
    index: usize,
}

/// Checks that `StoreSlot` writes frame memory only where nothing else can:
/// the reserved registers (the this value and the saved lexical
/// environment, which slow paths write there too), written constants, and the
/// slots a node that reads the frame reads, right before it. Merges and back
/// edges never write frame memory: slots that differ get phis.
pub fn verify_frame_writes(graph: &Graph, layout: &FrameLayout) -> Result<(), String> {
    let mut errors = Vec::new();
    for (index, block) in graph.blocks.iter().enumerate() {
        let nodes = block
            .body
            .iter()
            .chain(block.control.iter())
            .copied()
            .collect::<Vec<_>>();
        for (position, node) in nodes.iter().enumerate() {
            let Op::StoreSlot { slot } = graph.node(*node).op else {
                continue;
            };
            let operand = Operand::from_raw(slot).raw();
            let is_constant = operand >= layout.registers_and_locals_count && operand < layout.arguments_base();
            if operand < RESERVED_REGISTER_COUNT || is_constant {
                continue;
            }
            // NB: Conversions of the reader's inputs may come in between.
            let reader = nodes[position + 1..].iter().find(|next| {
                let op = &graph.node(**next).op;
                let properties = op.properties();
                !matches!(op, Op::StoreSlot { .. })
                    && (properties.is_call
                        || properties.reads != Locations::NONE
                        || properties.writes != Locations::NONE)
            });
            let reads_frame = reader.is_some_and(|reader| {
                let properties = graph.node(*reader).op.properties();
                properties.is_call && properties.reads.intersects(Locations::FRAME)
            });
            if !reads_frame {
                errors.push(format!(
                    "StoreSlot v{} in b{index} writes a slot that no node reading the frame reads next",
                    node.0
                ));
            }
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n"))
    }
}

/// Returns a description of every violated invariant, or `Ok` if there is none.
pub fn verify(graph: &Graph) -> Result<(), String> {
    let places = verify_blocks(graph)?;
    verify_edges(graph)?;
    verify_nodes(graph, &places)?;
    verify_frame_states(graph)
}

/// Checks what each block holds, and returns where each node is.
fn verify_blocks(graph: &Graph) -> Result<Vec<Option<Place>>, String> {
    let mut errors: Vec<String> = Vec::new();
    let mut places: Vec<Option<Place>> = vec![None; graph.nodes.len()];
    for (index, block) in graph.blocks.iter().enumerate() {
        let block_id = BlockId::from_index(index);
        let Some(control) = block.control else {
            errors.push(format!("b{index} has no control node"));
            continue;
        };
        if !graph.node(control).op.is_control() {
            errors.push(format!("b{index} ends in v{}, which is not a control node", control.0));
        }
        for phi in &block.phis {
            if graph.node(*phi).op != Op::Phi {
                errors.push(format!("b{index} has v{} among its phis", phi.0));
            }
            if graph.node(*phi).inputs.len() != block.predecessors.len() {
                errors.push(format!(
                    "phi v{} of b{index} has {} inputs for {} predecessors",
                    phi.0,
                    graph.node(*phi).inputs.len(),
                    block.predecessors.len()
                ));
            }
        }
        for node in &block.body {
            let op = &graph.node(*node).op;
            if *op == Op::Phi || op.is_control() || matches!(op, Op::Constant(_)) {
                errors.push(format!("b{index} has {} v{} in its body", op.name(), node.0));
            }
        }
        for (position, node) in graph.block_nodes(block_id).enumerate() {
            match places[node.index()] {
                Some(other) => errors.push(format!("v{} is in b{} and in b{index}", node.0, other.block.0)),
                None => {
                    places[node.index()] = Some(Place {
                        block: block_id,
                        index: position,
                    });
                }
            }
        }
    }
    if errors.is_empty() {
        Ok(places)
    } else {
        Err(errors.join("\n"))
    }
}

/// Checks the edges between blocks and their order.
fn verify_edges(graph: &Graph) -> Result<(), String> {
    let mut errors: Vec<String> = Vec::new();
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
                errors.push(format!(
                    "b{index} has {edges_out} edges to b{}, which lists it {edges_in} times",
                    successor.0
                ));
            }
        }
        if !block.phis.is_empty() {
            for predecessor in &block.predecessors {
                if graph.successors(*predecessor).len() != 1 {
                    errors.push(format!(
                        "b{index} has phis, but its predecessor b{} also leads elsewhere",
                        predecessor.0
                    ));
                }
            }
        }
        for predecessor in &block.predecessors {
            if !graph.successors(*predecessor).contains(&block_id) {
                errors.push(format!(
                    "b{index} lists b{} as a predecessor, which does not branch to it",
                    predecessor.0
                ));
            }
            // NB: Cold code comes after the code it rejoins.
            if predecessor.index() >= index
                && !block.is_loop_header
                && !(graph.block(*predecessor).is_cold && !block.is_cold)
            {
                errors.push(format!(
                    "b{index} is reached from the later b{} but is no loop header",
                    predecessor.0
                ));
            }
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n"))
    }
}

/// Checks that values are defined before their uses, and the rules of each
/// kind of node.
fn verify_nodes(graph: &Graph, places: &[Option<Place>]) -> Result<(), String> {
    let mut errors: Vec<String> = Vec::new();
    let dominators = Dominators::compute(graph);
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
            errors.push(format!(
                "b{index} has {} predecessors, but is {}",
                block.predecessors.len(),
                if is_entry { "an entry" } else { "no entry" }
            ));
        }
        if !dominators.is_reachable(block_id) {
            errors.push(format!("b{index} is reachable from no entry"));
            continue;
        }
        for phi in &block.phis {
            for (input, predecessor) in graph.node(*phi).inputs.iter().zip(&block.predecessors) {
                if graph.node(*input).repr != graph.node(*phi).repr {
                    errors.push(format!(
                        "phi v{} of b{index} is {:?}, but its input v{} is {:?}",
                        phi.0,
                        graph.node(*phi).repr,
                        input.0,
                        graph.node(*input).repr
                    ));
                }
                let end = graph.block_nodes(*predecessor).count();
                if !is_defined_before(*input, *predecessor, end) {
                    errors.push(format!(
                        "phi v{} of b{index} uses v{} from b{}, where it is not defined",
                        phi.0, input.0, predecessor.0
                    ));
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
                    errors.push(format!(
                        "v{} ({}) in b{index} uses v{}, which is not defined before it",
                        node_id.0,
                        node.op.name(),
                        input.0
                    ));
                } else if graph.node(*input).repr.is_none() {
                    errors.push(format!(
                        "v{} ({}) uses v{}, which has no value",
                        node_id.0,
                        node.op.name(),
                        input.0
                    ));
                }
            }
            // NB: Lazy exits of calls find their values after the call, but
            //     those are defined before it all the same.
            if let Some(frame_state) = node.frame_state {
                for (slot, value) in graph.frame_state_values(frame_state) {
                    if graph.node(value).repr.is_none() && !matches!(graph.node(value).op, Op::VirtualObject { .. }) {
                        errors.push(format!(
                            "the frame state of v{} has v{} for slot {slot}, which has no value",
                            node_id.0, value.0
                        ));
                    }
                    if graph.node(value).repr == Some(Repr::Pointer) {
                        errors.push(format!(
                            "the frame state of v{} has the cell address v{} for slot {slot}",
                            node_id.0, value.0
                        ));
                    }
                    if !is_defined_before(value, block_id, position) {
                        errors.push(format!(
                            "the frame state of v{} ({}) in b{index} has v{} for slot {slot}, which is not defined before it",
                            node_id.0,
                            node.op.name(),
                            value.0
                        ));
                    }
                }
            }
            if let Some(message) = check_input_reprs(graph, node_id) {
                errors.push(message);
            }
            if (node.op.can_eager_exit() || node.op.properties().can_lazy_exit) && node.frame_state.is_none() {
                errors.push(format!(
                    "v{} ({}) can exit, but has no frame state",
                    node_id.0,
                    node.op.name()
                ));
            }
            match node.op {
                Op::Jump { .. } if !node.inputs.is_empty() => {
                    errors.push(format!("v{} (Jump) has inputs", node_id.0));
                }
                Op::Branch {
                    condition: crate::ir::BranchCondition::Shape(_),
                    ..
                } => errors.push(format!("v{} branches on a shape", node_id.0)),
                _ => {}
            }
            if let Op::CallSlowPath { .. } = node.op {
                if node.frame_state.is_none() {
                    errors.push(format!("v{} (CallSlowPath) has no frame state", node_id.0));
                }
                if matches!(
                    node.op,
                    Op::CallSlowPath {
                        saves_registers: true,
                        ..
                    }
                ) && !block.is_cold
                {
                    errors.push(format!(
                        "v{} (CallSlowPath) is in b{index}, which is not cold",
                        node_id.0
                    ));
                }
            }
            if let Op::Refine { .. } = node.op {
                let at_head = block.body[..position - block.phis.len()]
                    .iter()
                    .all(|other| matches!(graph.node(*other).op, Op::Refine { .. }));
                if !at_head || block.predecessors.len() != 1 {
                    errors.push(format!(
                        "v{} (Refine) is not at the head of b{index}, a block with one predecessor",
                        node_id.0
                    ));
                }
            }
            if let Some(index) = node.op.refined_input()
                && node.repr.is_some()
                && node.repr != graph.node(node.inputs[index]).repr
            {
                errors.push(format!(
                    "v{} ({}) changes the representation of the value it refines",
                    node_id.0,
                    node.op.name()
                ));
            }
            // A slow path's further outputs are where its call left them only
            // until anything else runs.
            if let Op::SlowPathOutput { .. } = node.op {
                let call = node.inputs[0];
                let before = graph.block_nodes(block_id).take(position).collect::<Vec<_>>();
                let follows_call = matches!(graph.node(call).op, Op::CallSlowPath { .. })
                    && before.iter().position(|other| *other == call).is_some_and(|start| {
                        before[start + 1..]
                            .iter()
                            .all(|other| matches!(graph.node(*other).op, Op::SlowPathOutput { .. }))
                    });
                if !follows_call {
                    errors.push(format!(
                        "v{} (SlowPathOutput) does not follow its slow path call v{} directly",
                        node_id.0, call.0
                    ));
                }
            }
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n"))
    }
}

/// Checks that frame states list each slot once.
fn verify_frame_states(graph: &Graph) -> Result<(), String> {
    let mut errors: Vec<String> = Vec::new();
    for (index, frame_state) in graph.frame_states.iter().enumerate() {
        let mut slots: Vec<u32> = frame_state
            .values
            .iter()
            .map(|(slot, _)| *slot)
            .chain(frame_state.in_frame.iter().copied())
            .collect();
        slots.sort_unstable();
        if slots.windows(2).any(|pair| pair[0] == pair[1]) {
            errors.push(format!("fs{index} lists a slot more than once"));
        }
    }
    if errors.is_empty() {
        Ok(())
    } else {
        Err(errors.join("\n"))
    }
}

fn branch_input_reprs(condition: crate::ir::BranchCondition) -> Vec<Repr> {
    let tagged = |count: usize| vec![Repr::Tagged; count];
    match condition {
        crate::ir::BranchCondition::Bool => vec![Repr::Bool],
        crate::ir::BranchCondition::Int32(_) => vec![Repr::Int32, Repr::Int32],
        crate::ir::BranchCondition::Float64(_) => vec![Repr::Float64, Repr::Float64],
        crate::ir::BranchCondition::TaggedEquals { .. } => tagged(2),
        crate::ir::BranchCondition::Nullish
        | crate::ir::BranchCondition::Undefined
        | crate::ir::BranchCondition::Object
        | crate::ir::BranchCondition::NonNegativeInt32
        | crate::ir::BranchCondition::Int32Value
        | crate::ir::BranchCondition::Double
        | crate::ir::BranchCondition::String
        | crate::ir::BranchCondition::ElementsKind(_)
        | crate::ir::BranchCondition::Builtin(_) => tagged(1),
        crate::ir::BranchCondition::ResolvedString
        | crate::ir::BranchCondition::MagicalLength
        | crate::ir::BranchCondition::Extensible => vec![Repr::Pointer],
        crate::ir::BranchCondition::Shape(_) => vec![Repr::Tagged, Repr::Pointer],
        crate::ir::BranchCondition::PropertyIteratorCacheValid => tagged(2),
        crate::ir::BranchCondition::BindingMutable { .. } | crate::ir::BranchCondition::NextBindingOfShape { .. } => {
            vec![Repr::Pointer]
        }
        crate::ir::BranchCondition::IndexInBounds => vec![Repr::Int32, Repr::Int32],
    }
}

/// The representations a node requires of its inputs, if it requires any.
fn expected_input_reprs(graph: &Graph, node: NodeId) -> Option<Vec<Repr>> {
    let node = graph.node(node);
    let tagged = |count: usize| vec![Repr::Tagged; count];
    match node.op {
        Op::Phi | Op::Constant(_) | Op::StoreSlot { .. } => None,
        Op::Branch { condition, .. } | Op::Refine { condition, .. } => Some(branch_input_reprs(condition)),
        Op::UnboxInt32 | Op::UnboxDouble | Op::CheckNumber | Op::StringAddress => Some(tagged(1)),
        Op::StringLength => Some(vec![Repr::Pointer]),
        Op::BoxFloat64 | Op::Float64Unary { .. } | Op::Float64ToInt32 => Some(vec![Repr::Float64]),
        Op::Float64Binary { .. } | Op::Float64Compare { .. } => Some(vec![Repr::Float64, Repr::Float64]),
        Op::Uint32ShiftRight => Some(vec![Repr::Int32, Repr::Int32]),
        Op::Uint32ToFloat64 | Op::IntegerToString => Some(vec![Repr::Int32]),
        Op::StringsEqual | Op::ConcatenateStrings | Op::ToObject => Some(tagged(2)),
        Op::PrimitiveToString => Some(tagged(1)),
        Op::ArrayCreate => Some(vec![Repr::Int32, Repr::Tagged]),
        Op::LoadStringCodeUnit => Some(vec![Repr::Pointer, Repr::Int32]),
        Op::SingleCharacterString | Op::Int32Abs | Op::Int32ToFloat64 => Some(vec![Repr::Int32]),
        Op::AppendElement => Some(vec![Repr::Pointer, Repr::Int32, Repr::Tagged]),
        Op::LoadElementsLength | Op::LoadElementsCapacity => Some(vec![Repr::Pointer]),
        Op::LoadPropertyIteratorKey => Some(vec![Repr::Tagged, Repr::Int32]),
        Op::BranchOnPc { .. } => Some(vec![Repr::Int32]),
        Op::BoxInt32 => Some(vec![Repr::Int32]),
        Op::CheckShape { .. }
        | Op::LoadNamed { .. }
        | Op::ShapeSwitch { .. }
        | Op::CheckAccessorFunction { .. }
        | Op::LoadAccessorFunction { .. } => Some(vec![Repr::Tagged, Repr::Pointer]),
        Op::StoreNamed { .. } | Op::AddNamed { .. } => Some(vec![Repr::Tagged, Repr::Tagged, Repr::Pointer]),
        Op::LoadElementAt { .. } => Some(vec![Repr::Pointer, Repr::Int32]),
        Op::StoreElementAt { kind } => Some(vec![Repr::Pointer, Repr::Int32, kind.stored_repr()]),
        Op::CheckBounds => Some(vec![Repr::Int32, Repr::Int32]),
        Op::LoadTypedArrayLength => Some(vec![Repr::Pointer]),
        Op::BoxBool => Some(vec![Repr::Bool]),
        Op::LoadFunctionEnvironment { .. }
        | Op::BoxCell
        | Op::LoadOuterEnvironment
        | Op::LoadEnvironmentBinding { .. }
        | Op::AppendEnvironmentBinding => Some(vec![Repr::Pointer]),
        Op::StoreEnvironmentBinding { .. } => Some(vec![Repr::Pointer, Repr::Tagged]),
        Op::AllocateFunction { .. } | Op::ProbeGlobalCache { .. } => Some(vec![Repr::Pointer, Repr::Pointer]),
        Op::ProbeGlobalStore { .. } => Some(vec![Repr::Tagged, Repr::Pointer, Repr::Pointer]),
        Op::Int32Binary { .. } | Op::Int32Compare { .. } => Some(vec![Repr::Int32, Repr::Int32]),
        _ => Some(tagged(node.inputs.len())),
    }
}

fn check_input_reprs(graph: &Graph, node_id: NodeId) -> Option<String> {
    let expected = expected_input_reprs(graph, node_id)?;
    let node = graph.node(node_id);
    if node.inputs.len() != expected.len() {
        return Some(format!(
            "v{} ({}) has {} inputs, but takes {}",
            node_id.0,
            node.op.name(),
            node.inputs.len(),
            expected.len()
        ));
    }
    for (input, repr) in node.inputs.iter().zip(expected) {
        let actual = graph.node(*input).repr?;
        if actual != repr {
            return Some(format!(
                "v{} ({}) takes v{} as {repr:?}, but it is {actual:?}",
                node_id.0,
                node.op.name(),
                input.0
            ));
        }
    }
    None
}
