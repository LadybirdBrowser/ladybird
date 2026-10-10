/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Redundant check and load elimination.
//!
//! A forward dataflow analysis over the control flow graph computes, before
//! every node, what is known about values (see `Facts`): what kinds of value
//! they may be, which shapes and elements kinds objects may have, which
//! prototype chains are valid, which values passed which value checks, and
//! what named properties, bindings and element counts hold. Loop headers
//! start optimistically with what holds on entry and are revisited until
//! what the back edges bring agrees with it.
//!
//! Branches on the kind of a value tell their targets what it is, and
//! become jumps where what is known decides them.
//!
//! With those facts, checks that already passed are removed, loads of a
//! property whose value is known are replaced by that value (including
//! values stored before, and the constants a value check established), and
//! shape switches on objects whose shapes are known only keep the cases
//! that can match, or become jumps.
//!
//! A removed check's value is the latest refinement of its input on every
//! path to it, which a check or branch that passed made, so that what uses
//! it stays behind that check. Checks with no such refinement stay.
//!
//! Objects keep stable shapes (see `Dependency::StableShape`) while other
//! code runs, as long as the compiled code stays valid. The first shape
//! check that relies on that after other code ran becomes an `AssumeValid`,
//! and the code depends on the stable shapes known from then on.

use super::edit;
use super::edit::Constants;
use super::edit::Replacements;
use super::facts::ElementCount;
use super::facts::Facts;
use super::facts::Location;
use super::facts::ValueKind;
use super::facts::elements_kind_implies;
use crate::bitset::BitSet;
use crate::code::Dependency;
use crate::inline_vec::InlineVec;
use crate::ir::BlockId;
use crate::ir::ElementsKind;
use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::ShapeCheck;
use crate::snapshot::CellId;

/// What the analysis decided for the nodes of one visit of the graph.
struct Decisions {
    replacements: Replacements,
    removed: BitSet,
    /// Control nodes to replace, with what.
    controls: Vec<(NodeId, Op)>,
    /// Shape checks that become `AssumeValid` nodes.
    assumptions: Vec<NodeId>,
    /// The stable shapes the code relies on staying stable.
    stable_shapes: Vec<CellId>,
}

impl Decisions {
    /// The value the analysis knows `value` as: what it is replaced by, and
    /// the value it refines (facts are about values, whatever refines them).
    fn value(&self, graph: &Graph, value: NodeId) -> NodeId {
        let mut value = self.replacements.resolve(value);
        loop {
            let unrefined = graph.unrefined(value);
            let replaced = self.replacements.resolve(unrefined);
            if replaced == value {
                return value;
            }
            value = replaced;
        }
    }
}

pub fn run(graph: &mut Graph) {
    let mut constants = Constants::new(graph);
    let block_count = graph.blocks.len();
    // The facts flowing along each edge, from the latest visit of its source.
    let mut edge_facts: Vec<Vec<(BlockId, Facts)>> = vec![Vec::new(); block_count];
    // NB: Only blocks with predecessors later in the order can see other
    //     facts in another visit: they decide when the visits are done.
    let mut entry_facts: Vec<Option<Facts>> = vec![None; block_count];
    let revisited = (0..block_count)
        .map(|block| {
            graph.blocks[block]
                .predecessors
                .iter()
                .any(|predecessor| predecessor.index() >= block)
        })
        .collect::<Vec<_>>();
    let decisions = loop {
        let mut decisions = Decisions {
            replacements: Replacements::new(graph),
            removed: BitSet::new(graph.nodes.len()),
            controls: Vec::new(),
            assumptions: Vec::new(),
            stable_shapes: Vec::new(),
        };
        let mut changed = false;
        let mut visited = vec![false; block_count];
        for block in 0..block_count {
            let block_id = BlockId::from_index(block);
            let Some(facts) = facts_at_entry(graph, block_id, &mut edge_facts, &visited) else {
                // No visited predecessor reaches this block.
                edge_facts[block].clear();
                continue;
            };
            visited[block] = true;
            if revisited[block] && entry_facts[block].as_ref() != Some(&facts) {
                changed = true;
                entry_facts[block] = Some(facts.clone());
            }
            edge_facts[block] = visit_block(graph, &mut constants, block_id, facts, &mut decisions);
        }
        if !changed {
            break decisions;
        }
    };

    decisions.replacements.apply(graph);
    edit::remove_nodes(graph, &decisions.removed);
    for node in decisions.assumptions {
        graph.nodes[node.index()].op = Op::AssumeValid;
        graph.nodes[node.index()].inputs.clear();
        graph.nodes[node.index()].repr = None;
    }
    for shape in decisions.stable_shapes {
        graph.depend_on(Dependency::StableShape(shape));
    }
    let changed_control_flow = !decisions.controls.is_empty();
    for (node, op) in decisions.controls {
        match op {
            Op::Jump { target } => edit::make_jump(graph, node, target),
            op => graph.nodes[node.index()].op = op,
        }
    }
    if changed_control_flow {
        edit::clean_up_control_flow(graph);
    }
}

/// The facts at the start of `block`, which takes them from the edges into
/// it: what holds on every edge into it from a visited predecessor. Edges
/// from predecessors not visited yet in this round (loop back edges) are
/// assumed to agree, and checked when the next round sees what they bring.
/// Roots start knowing nothing, except that the code is valid: invalidated
/// code is discarded, so nothing enters it.
fn facts_at_entry(
    graph: &Graph,
    block: BlockId,
    edge_facts: &mut [Vec<(BlockId, Facts)>],
    visited: &[bool],
) -> Option<Facts> {
    let predecessors = &graph.block(block).predecessors;
    if predecessors.is_empty() {
        let mut facts = Facts::default();
        facts.assume_code_valid();
        return Some(facts);
    }
    let mut incoming = Vec::new();
    for predecessor in predecessors {
        let is_back_edge = predecessor.index() >= block.index();
        if !is_back_edge && !visited[predecessor.index()] {
            continue;
        }
        let edges = &mut edge_facts[predecessor.index()];
        incoming.extend(
            edges
                .extract_if(.., |(target, _)| *target == block)
                .map(|(_, facts)| facts),
        );
    }
    match incoming.len() {
        0 => None,
        1 => incoming.pop(),
        _ => Some(Facts::merge(incoming.iter())),
    }
}

/// Applies the nodes of `block` to `facts`, recording decisions, and returns
/// the facts flowing along each of its outgoing edges.
fn visit_block(
    graph: &mut Graph,
    constants: &mut Constants,
    block: BlockId,
    mut facts: Facts,
    decisions: &mut Decisions,
) -> Vec<(BlockId, Facts)> {
    // NB: A phi makes another value of the objects it merges, which anything
    //     may see.
    for phi in &graph.block(block).phis {
        for input in &graph.node(*phi).inputs {
            facts.expose(decisions.value(graph, *input));
        }
    }
    let body = graph.block(block).body.clone();
    for node in body {
        visit_node(graph, constants, node, &mut facts, decisions);
    }
    let control = graph.control_id(block);
    // A branch on the kind of a value (or of the elements of an object)
    // knows it where it holds, and only keeps the edge that what is known
    // already decides.
    if let Op::Branch {
        condition,
        if_true,
        if_false,
    } = graph.node(control).op
        && Facts::is_about_kinds(condition)
    {
        let value = decisions.value(graph, graph.node(control).inputs[0]);
        if let Some(holds) = facts.decides(graph, value, condition) {
            let target = if holds { if_true } else { if_false };
            decisions.controls.push((control, Op::Jump { target }));
            return vec![(target, facts)];
        }
        let mut passed = facts.clone();
        passed.forget_refinement(value);
        passed.set_passed(value, condition);
        facts.set_failed(value, condition);
        return vec![(if_true, passed), (if_false, facts)];
    }
    let Op::ShapeSwitch { cases } = graph.node(control).op.clone() else {
        let successors = graph.successors(block);
        let Some((last, others)) = successors.split_last() else {
            return Vec::new();
        };
        let mut edges = others
            .iter()
            .map(|successor| (*successor, facts.clone()))
            .collect::<Vec<_>>();
        edges.push((*last, facts));
        return edges;
    };
    let object = decisions.value(graph, graph.node(control).inputs[0]);
    // NB: Shapes known only while the code is valid would need a check.
    let known = facts
        .shapes(object)
        .filter(|_| !facts.is_unvalidated(object))
        .map(<[ShapeCheck]>::to_vec);
    let possible = cases
        .iter()
        .filter(|(shape, _)| known.as_ref().is_none_or(|known| known.contains(shape)))
        .copied()
        .collect::<Vec<_>>();
    let covers_known = known
        .as_ref()
        .is_some_and(|known| known.iter().all(|shape| possible.iter().any(|(case, _)| case == shape)));
    match possible.as_slice() {
        [(_, target)] if covers_known => decisions.controls.push((control, Op::Jump { target: *target })),
        _ if possible.len() < cases.len() && !possible.is_empty() => {
            decisions.controls.push((
                control,
                Op::ShapeSwitch {
                    cases: possible.clone(),
                },
            ));
        }
        _ => {}
    }
    possible
        .iter()
        .map(|(shape, target)| {
            let mut case_facts = facts.clone();
            case_facts.set_shapes(object, &[*shape]);
            (*target, case_facts)
        })
        .collect()
}

/// Whether input `index` of a node with `op` is an object whose properties
/// (or shape) the node accesses, and nothing else.
fn accesses_properties(op: &Op, index: usize) -> bool {
    match op {
        Op::CheckObject | Op::CellAddress | Op::InitializeNamed { .. } => index == 0,
        Op::CheckShape { .. }
        | Op::ShapeSwitch { .. }
        | Op::LoadNamed { .. }
        | Op::CheckAccessorFunction { .. }
        | Op::LoadAccessorFunction { .. } => true,
        Op::StoreNamed { .. } | Op::AddNamed { .. } => index != 1,
        _ => false,
    }
}

fn visit_node(
    graph: &mut Graph,
    constants: &mut Constants,
    node_id: NodeId,
    facts: &mut Facts,
    decisions: &mut Decisions,
) {
    let node = graph.node(node_id);
    let inputs = node
        .inputs
        .iter()
        .map(|input| decisions.value(graph, *input))
        .collect::<InlineVec<_, 4>>();
    let input = |index: usize| inputs[index];
    for (index, input) in inputs.iter().enumerate() {
        if facts.is_unexposed(*input) && !accesses_properties(&node.op, index) {
            facts.expose(*input);
        }
    }
    // NB: A check known to pass is replaced by what refined its input
    //     before (see `Facts::refinement()`), and stays where nothing did.
    let refines = node.op.refined_input().is_some() && node.repr.is_some();
    let anchor = if refines {
        facts
            .refinement(input(0))
            .or_else(|| graph.constant_value(input(0)).map(|_| input(0)))
    } else {
        None
    };
    let removable = !refines || anchor.is_some();
    let mut remove = false;
    let mut becomes_assumption = false;
    match node.op.clone() {
        Op::CheckObject => {
            let value = input(0);
            remove = removable && facts.is_object(value);
            facts.set_object(value);
        }
        Op::CheckShape { shapes } => {
            let object = input(0);
            remove = removable
                && facts
                    .shapes(object)
                    .is_some_and(|known| known.iter().all(|shape| shapes.contains(shape)));
            if remove && facts.is_unvalidated(object) {
                // The object still has its stable shape if the code is still
                // valid, and so do all objects with known shapes after this.
                if !facts.is_code_valid() {
                    remove = false;
                    becomes_assumption = true;
                    decisions.assumptions.push(node_id);
                    if let Some(anchor) = anchor {
                        decisions.replacements.replace(node_id, anchor);
                    }
                    facts.assume_code_valid();
                }
                validate(facts, &graph.stable_shapes, decisions);
            } else if !remove {
                facts.set_shapes(object, &shapes);
            }
        }
        // NB: Values that pass are int32 values.
        Op::CheckInt32 => facts.set_value_kind(input(0), ValueKind::Int32),
        Op::CheckPrototypeChainValid { validity } => {
            remove = facts.is_prototype_chain_valid(validity);
            facts.set_prototype_chain_valid(validity);
        }
        Op::CheckValue { expected, .. } => {
            let value = input(0);
            remove =
                removable && (graph.constant_value(value) == Some(expected) || facts.is_checked_value(value, expected));
            if !remove {
                facts.set_checked_value(value, expected);
                // A checked load of a property or a global binding makes it
                // known to hold the constant.
                if let Some(location @ (Location::Named { .. } | Location::GlobalBinding { .. })) =
                    loaded_location(graph, decisions, value)
                    && facts.contents(location) == Some(value)
                {
                    let constant = constants.get(graph, expected);
                    facts.set_contents(location, constant);
                }
            }
        }
        Op::LoadNamed { .. }
        | Op::LoadGlobalBinding { .. }
        | Op::LoadEnvironmentBinding { .. }
        | Op::LoadElementsLength
        | Op::LoadElementsCapacity
        | Op::LoadTypedArrayLength => {
            let location = loaded_location(graph, decisions, node_id).expect("loads load a location");
            remove = reuse_contents(facts, location, node_id, decisions);
        }
        Op::AssumeValid => {
            remove = facts.is_code_valid();
            facts.assume_code_valid();
            validate(facts, &graph.stable_shapes, decisions);
        }
        Op::StoreGlobalBinding { environment, index } => {
            facts.forget_bindings_at(index);
            facts.set_contents(Location::GlobalBinding { environment, index }, input(0));
        }
        Op::StoreEnvironmentBinding { index } => {
            facts.forget_bindings_at(index);
            let location = Location::Binding {
                environment: input(0),
                index,
            };
            facts.set_contents(location, input(1));
        }
        Op::StoreNamed { offset } => {
            let (object, value) = (input(0), input(1));
            facts.forget_named_properties_at(offset);
            facts.set_contents(Location::Named { object, offset }, value);
        }
        Op::AddNamed { offset, shape, .. } => {
            // NB: Values known to have the old shape may be the object, which
            //     has the new one now. No object had the new property before.
            let object = input(0);
            facts.forget_shapes_of(object);
            facts.set_object(object);
            facts.set_shapes(
                object,
                &[ShapeCheck {
                    shape,
                    dictionary_generation: None,
                }],
            );
            facts.set_contents(Location::Named { object, offset }, input(1));
        }
        Op::CheckElements { kind } => {
            let object = input(0);
            // NB: A holey access takes packed elements too.
            remove = removable
                && facts
                    .elements_kind(object)
                    .is_some_and(|known| elements_kind_implies(known, kind) == Some(true));
            facts.set_object(object);
            if !remove {
                facts.set_elements_kind(object, kind);
            }
        }
        Op::AllocateObject {
            shape, property_count, ..
        } => {
            // A new object of the shape, with every property undefined.
            new_object(facts, node_id);
            facts.set_unexposed(node_id);
            facts.set_shapes(
                node_id,
                &[ShapeCheck {
                    shape,
                    dictionary_generation: None,
                }],
            );
            let undefined = constants.get(graph, crate::ir::value::UNDEFINED);
            for offset in 0..property_count {
                facts.set_contents(
                    Location::Named {
                        object: node_id,
                        offset,
                    },
                    undefined,
                );
            }
        }
        Op::InitializeNamed { offset } => {
            // NB: Nothing else can see the object yet, so no other object's
            //     property is the same one.
            facts.set_contents(
                Location::Named {
                    object: input(0),
                    offset,
                },
                input(1),
            );
        }
        // NB: SliceArguments is a runtime call that makes a new array and
        //     runs nothing else.
        Op::AllocateFunction { .. } | Op::SliceArguments => new_object(facts, node_id),
        Op::AllocateArray { count } => {
            // A new array, with packed elements if it has any.
            new_object(facts, node_id);
            if count != 0 {
                facts.set_elements_kind(node_id, ElementsKind::Packed);
            }
        }
        Op::CheckAppendableArray => {
            // It exits for anything but arrays.
            facts.set_object(input(0));
        }
        ref op => {
            facts.apply(&op.properties(), &graph.stable_shapes);
        }
    }
    if remove {
        if let Some(anchor) = anchor {
            decisions.replacements.replace(node_id, anchor);
        }
        decisions.removed.insert(node_id.index());
    } else if refines && !becomes_assumption {
        facts.set_refinement(input(0), node_id);
    }
}

/// The location the load `load` reads, which facts may know the contents
/// of.
fn loaded_location(graph: &Graph, decisions: &Decisions, load: NodeId) -> Option<Location> {
    let node = graph.node(load);
    let input = |index: usize| decisions.value(graph, node.inputs[index]);
    Some(match node.op {
        Op::LoadNamed { offset } => Location::Named {
            object: input(0),
            offset,
        },
        Op::LoadGlobalBinding { environment, index } => Location::GlobalBinding { environment, index },
        Op::LoadEnvironmentBinding { index } => Location::Binding {
            environment: input(0),
            index,
        },
        ref op => {
            let count = ElementCount::loaded_by(op)?;
            // NB: Before value numbering, an object may have several
            //     addresses.
            let address = graph.node(input(0));
            let object = match address.op {
                Op::CellAddress => decisions.value(graph, address.inputs[0]),
                _ => input(0),
            };
            Location::ElementCount { count, object }
        }
    })
}

/// `object` is a new object, which is what it is by its definition.
fn new_object(facts: &mut Facts, object: NodeId) {
    facts.set_object(object);
    facts.set_refinement(object, object);
}

/// The code is known to be valid from here on, which what is known about
/// shapes relies on.
fn validate(facts: &mut Facts, stable_shapes: &[CellId], decisions: &mut Decisions) {
    for shape in facts.validate(stable_shapes) {
        if !decisions.stable_shapes.contains(&shape) {
            decisions.stable_shapes.push(shape);
        }
    }
}

/// A load of `location` by `node`: replaced by what the location is known
/// to hold (returning true), or what it holds from now on.
fn reuse_contents(facts: &mut Facts, location: Location, node: NodeId, decisions: &mut Decisions) -> bool {
    match facts.contents(location) {
        Some(value) => {
            decisions.replacements.replace(node, value);
            true
        }
        None => {
            facts.set_contents(location, node);
            false
        }
    }
}
