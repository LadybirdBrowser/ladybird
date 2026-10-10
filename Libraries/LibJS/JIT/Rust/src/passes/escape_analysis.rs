/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Escape analysis and scalar replacement of allocations.
//!
//! An object an `AllocateObject` makes escapes when anything could see it
//! besides accesses of its named properties: when it is stored into the
//! frame or into an object that escapes, passed to anything but property
//! accesses and checks (an operand of a slow path or a call included), or
//! merged in a phi. An object that does not escape is never allocated: its
//! properties become SSA values, initialized to `undefined`. Stores update
//! them, loads become the value stored last, checks of the object (whose
//! shape is known) go away, and where paths merge, phis merge the values.
//!
//! Frame states only reach the frame when compiled code leaves: at exits,
//! and when slow paths and calls do not continue in compiled code (see
//! `SiteKind::Leave`). Frame states that need the object refer to an
//! `Op::VirtualObject` with the values its properties have there, and the
//! runtime creates it then.
//!
//! Values stored into an object that does not escape do not escape by being
//! stored there, so nested objects (an options bag holding a location
//! object, say) go away together. But where paths merge the property that
//! holds one in a phi, the object escapes and the pass starts over: a phi
//! of objects that are never allocated cannot be the value of a property
//! of a virtual object.

use super::edit;
use super::edit::Constants;
use super::edit::Replacements;
use crate::bitset::BitSet;
use crate::code::Repr;
use crate::fast_hash::HashMap;
use crate::fast_hash::HashSet;
use crate::ir::Block;
use crate::ir::BlockId;
use crate::ir::FrameStateId;
use crate::ir::Graph;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;
use crate::ir::value;
use crate::snapshot::CellId;

/// The shape and property count each object has at some point, which
/// `AddNamed` nodes change.
type Shapes = HashMap<NodeId, (CellId, u32)>;

/// The analysis of one graph.
struct Analysis {
    /// The allocations that may be replaced.
    candidates: HashSet<NodeId>,
    /// For every value that is a candidate or stands for one (a candidate
    /// with empty values made undefined, or its cell address): the candidate.
    roots: HashMap<NodeId, NodeId>,
    escaped: BitSet,
    /// Whether an object that does not escape is stored into another one.
    has_nested_objects: bool,
}

impl Analysis {
    fn root(&self, value: NodeId) -> Option<NodeId> {
        self.roots.get(&value).copied()
    }
}

fn placed_nodes(graph: &Graph) -> Vec<NodeId> {
    graph.blocks.iter().flat_map(Block::nodes).collect()
}

fn analyze(graph: &Graph, placed: &[NodeId], escaping: &[NodeId]) -> Analysis {
    let mut candidates = HashSet::default();
    let mut roots = HashMap::default();
    for node in placed {
        if let Op::AllocateObject { .. } = graph.node(*node).op {
            candidates.insert(*node);
            roots.insert(*node, *node);
        }
    }
    let mut escaped = BitSet::new(graph.nodes.len());
    for object in escaping {
        escaped.insert(object.index());
    }
    if candidates.is_empty() {
        return Analysis {
            candidates,
            roots,
            escaped,
            has_nested_objects: false,
        };
    }

    // Values that stand for a candidate. They are defined after it, so one
    // walk in block order finds them.
    let mut changed = true;
    while changed {
        changed = false;
        for node in placed {
            let data = graph.node(*node);
            let stands_for_input = matches!(data.op, Op::EmptyToUndefined | Op::CellAddress)
                || data.op.refined_input() == Some(0) && data.repr.is_some();
            if !stands_for_input || roots.contains_key(node) {
                continue;
            }
            if let Some(root) = roots.get(&data.inputs[0]).copied() {
                roots.insert(*node, root);
                changed = true;
            }
        }
    }

    let shapes_before = shapes_before_uses(graph, &candidates, &roots, &mut escaped);

    // Values stored into candidates: (value's candidate, the candidate it is stored into).
    let mut stored_into = Vec::new();
    for node in placed {
        let data = graph.node(*node);
        for (index, input) in data.inputs.iter().enumerate() {
            let Some(root) = roots.get(input).copied() else {
                continue;
            };
            // NB: Every use of a candidate is where it has a known shape.
            let Some((shape, count)) = shapes_before.get(&(*node, root)).copied() else {
                escaped.insert(root.index());
                continue;
            };
            let accesses_object = match &data.op {
                Op::EmptyToUndefined | Op::CellAddress | Op::CheckObject => true,
                Op::LoadNamed { offset } => *offset < count,
                Op::CheckShape { shapes } => shapes
                    .iter()
                    .any(|check| check.shape == shape && check.dictionary_generation.is_none()),
                Op::InitializeNamed { offset } | Op::StoreNamed { offset } if index != 1 => *offset < count,
                Op::AddNamed { offset, .. } if index != 1 => *offset == count,
                Op::InitializeNamed { offset } | Op::StoreNamed { offset } | Op::AddNamed { offset, .. } => {
                    stored_into.push((root, roots.get(&data.inputs[0]).copied(), *offset));
                    true
                }
                _ => false,
            };
            if !accesses_object {
                escaped.insert(root.index());
            }
        }
    }
    // Loads of properties of candidates, as (candidate, offset).
    let loaded = placed
        .iter()
        .filter_map(|node| match graph.node(*node).op {
            Op::LoadNamed { offset } => roots.get(&graph.node(*node).inputs[0]).map(|root| (*root, offset)),
            _ => None,
        })
        .collect::<Vec<_>>();
    // An object stored into one that escapes escapes too. So does one stored
    // into a property that is loaded, which makes its value another value
    // standing for the object.
    // FIXME: Track the objects loaded properties may hold, for objects
    //        nested in others that are read.
    let mut changed = true;
    while changed {
        changed = false;
        for (value, target, offset) in &stored_into {
            let target_escapes =
                target.is_none_or(|target| escaped.contains(target.index()) || loaded.contains(&(target, *offset)));
            if target_escapes && !escaped.contains(value.index()) {
                escaped.insert(value.index());
                changed = true;
            }
        }
    }
    let has_nested_objects = stored_into
        .iter()
        .any(|(value, target, _)| target.is_some() && !escaped.contains(value.index()));
    Analysis {
        candidates,
        roots,
        escaped,
        has_nested_objects,
    }
}

/// Merges the shapes objects have at the ends of a block's predecessors:
/// objects whose shapes differ between paths escape, and objects missing on
/// a path are not defined after the merge.
fn merge_shapes<'a>(mut exits: impl Iterator<Item = &'a Shapes>, escaped: &mut BitSet) -> Shapes {
    let Some(first) = exits.next() else {
        return Shapes::default();
    };
    let mut merged = first.clone();
    for exit in exits {
        merged.retain(|object, shape| match exit.get(object) {
            Some(other) if other == shape => true,
            Some(_) => {
                escaped.insert(object.index());
                false
            }
            None => false,
        });
    }
    merged
}

/// The shape and property count each object candidate has where each node
/// uses it, as (node, candidate). Objects whose shapes differ between paths
/// joining, or around a loop, escape.
fn shapes_before_uses(
    graph: &Graph,
    candidates: &HashSet<NodeId>,
    roots: &HashMap<NodeId, NodeId>,
    escaped: &mut BitSet,
) -> HashMap<(NodeId, NodeId), (CellId, u32)> {
    let mut before = HashMap::default();
    let block_count = graph.blocks.len();
    let mut entries: Vec<Shapes> = vec![Shapes::default(); block_count];
    let mut exits: Vec<Option<Shapes>> = vec![None; block_count];
    for (index, block) in graph.blocks.iter().enumerate() {
        let forward = block
            .predecessors
            .iter()
            .filter(|predecessor| predecessor.index() < index)
            .filter_map(|predecessor| exits[predecessor.index()].as_ref());
        let mut shapes = merge_shapes(forward, escaped);
        entries[index] = shapes.clone();
        for node in block.nodes() {
            let data = graph.node(node);
            for input in &data.inputs {
                if let Some(root) = roots.get(input)
                    && let Some(shape) = shapes.get(root)
                {
                    before.insert((node, *root), *shape);
                }
            }
            match data.op {
                Op::AllocateObject {
                    shape, property_count, ..
                } if candidates.contains(&node) => {
                    shapes.insert(node, (shape, property_count));
                }
                Op::AddNamed {
                    shape, property_count, ..
                } => {
                    if let Some(root) = roots.get(&data.inputs[0]) {
                        shapes.insert(*root, (shape, property_count));
                    }
                }
                _ => {}
            }
        }
        exits[index] = Some(shapes);
    }
    for (index, block) in graph.blocks.iter().enumerate() {
        for predecessor in block
            .predecessors
            .iter()
            .filter(|predecessor| predecessor.index() >= index)
        {
            let Some(exit) = exits[predecessor.index()].as_ref() else {
                continue;
            };
            for (object, shape) in &entries[index] {
                if exit.get(object).is_some_and(|other| other != shape) {
                    escaped.insert(object.index());
                }
            }
        }
    }
    before
}

/// The value of each property of each replaced object at one point.
type Properties = HashMap<(NodeId, u32), NodeId>;

pub fn run(graph: &mut Graph) {
    let placed = placed_nodes(graph);
    let mut escaping = Vec::new();
    loop {
        let analysis = analyze(graph, &placed, &escaping);
        // NB: Only objects stored into others can be values of phis of
        //     properties.
        if !analysis.has_nested_objects {
            replace_objects(graph, &analysis).expect("objects not stored into others merge in no phi of properties");
            return;
        }
        let mut copy = graph.clone();
        match replace_objects(&mut copy, &analysis) {
            Ok(()) => {
                *graph = copy;
                return;
            }
            Err(objects) => escaping.extend(objects),
        }
    }
}

/// Replaces the objects of `analysis` that do not escape. Fails with the
/// replaced objects that would be the values of phis of properties where
/// paths merge, which must escape instead, leaving `graph` half edited.
fn replace_objects(graph: &mut Graph, analysis: &Analysis) -> Result<(), Vec<NodeId>> {
    let replaced = analysis
        .candidates
        .iter()
        .filter(|candidate| !analysis.escaped.contains(candidate.index()))
        .copied()
        .collect::<Vec<_>>();
    if replaced.is_empty() {
        return Ok(());
    }
    let is_replaced = |value: NodeId| {
        analysis
            .root(value)
            .is_some_and(|root| !analysis.escaped.contains(root.index()))
    };

    let mut constants = Constants::new(graph);
    let undefined = constants.get(graph, value::UNDEFINED);
    let mut replacements = Replacements::new(graph);
    let mut removed = BitSet::new(graph.nodes.len());
    let block_count = graph.blocks.len();
    let mut exit_properties: Vec<Option<Properties>> = vec![None; block_count];
    let mut exit_shapes: Vec<Option<Shapes>> = vec![None; block_count];
    // Loop header phis whose inputs along back edges are filled in last:
    // (phi, block, property).
    let mut pending_phis: Vec<(NodeId, BlockId, (NodeId, u32))> = Vec::new();
    // Replaced objects that would be values of those phis.
    let mut conflicts: Vec<NodeId> = Vec::new();

    for block_index in 0..block_count {
        let block = BlockId::from_index(block_index);
        let predecessors = graph.block(block).predecessors.clone();
        let forward = predecessors
            .iter()
            .filter(|predecessor| predecessor.index() < block_index)
            .copied()
            .collect::<Vec<_>>();
        let mut properties = Properties::default();
        // NB: Replaced objects have the same shape on every path.
        let mut shapes = forward
            .first()
            .and_then(|first| exit_shapes[first.index()].clone())
            .unwrap_or_default();
        if let Some(first) = forward.first() {
            let has_back_edges = forward.len() < predecessors.len();
            let first_properties = exit_properties[first.index()].clone().unwrap_or_default();
            for (key, first_value) in first_properties {
                let values = predecessors
                    .iter()
                    .map(|predecessor| {
                        if predecessor.index() < block_index {
                            exit_properties[predecessor.index()]
                                .as_ref()
                                .and_then(|exit| exit.get(&key).copied())
                        } else {
                            Some(first_value)
                        }
                    })
                    .collect::<Option<Vec<_>>>();
                // NB: An object missing on a path is not defined there, so
                //     nothing after the merge uses it.
                let Some(values) = values else {
                    continue;
                };
                if !has_back_edges && values.iter().all(|value| *value == first_value) {
                    properties.insert(key, first_value);
                    continue;
                }
                conflicts.extend(
                    values
                        .iter()
                        .map(|value| replacements.resolve(*value))
                        .filter(|value| is_replaced(*value))
                        .filter_map(|value| analysis.root(value)),
                );
                let phi = graph.add_node(Node::new(Op::Phi, values, Some(Repr::Tagged), 0));
                graph.blocks[block_index].phis.push(phi);
                if has_back_edges {
                    pending_phis.push((phi, block, key));
                }
                properties.insert(key, phi);
            }
        }

        let nodes = graph
            .block(block)
            .body
            .iter()
            .chain(&graph.block(block).control)
            .copied()
            .collect::<Vec<_>>();
        for node in nodes {
            let data = graph.node(node).clone();
            if let Some(frame_state) = data.frame_state {
                let new_frame_state = virtualize_frame_state(
                    graph,
                    analysis,
                    &is_replaced,
                    &replacements,
                    (&properties, &shapes),
                    frame_state,
                );
                graph.nodes[node.index()].frame_state = new_frame_state.or(data.frame_state);
            }
            let object = data.inputs.first().copied().filter(|object| is_replaced(*object));
            match data.op {
                Op::AllocateObject {
                    shape, property_count, ..
                } if is_replaced(node) => {
                    for offset in 0..property_count {
                        properties.insert((node, offset), undefined);
                    }
                    shapes.insert(node, (shape, property_count));
                    removed.insert(node.index());
                }
                Op::AddNamed {
                    offset,
                    shape,
                    property_count,
                } if object.is_some() => {
                    let root = analysis
                        .root(object.expect("checked"))
                        .expect("replaced objects have roots");
                    properties.insert((root, offset), replacements.resolve(data.inputs[1]));
                    shapes.insert(root, (shape, property_count));
                    removed.insert(node.index());
                }
                Op::InitializeNamed { offset } | Op::StoreNamed { offset } if object.is_some() => {
                    let root = analysis
                        .root(object.expect("checked"))
                        .expect("replaced objects have roots");
                    properties.insert((root, offset), replacements.resolve(data.inputs[1]));
                    removed.insert(node.index());
                }
                Op::LoadNamed { offset } if object.is_some() => {
                    let root = analysis
                        .root(object.expect("checked"))
                        .expect("replaced objects have roots");
                    let value = properties.get(&(root, offset)).copied().unwrap_or(undefined);
                    replacements.replace(node, value);
                    removed.insert(node.index());
                }
                Op::CheckShape { .. } | Op::CheckObject | Op::EmptyToUndefined | Op::CellAddress
                    if object.is_some() =>
                {
                    // NB: What a removed check refined is the object itself.
                    if data.op.refined_input().is_some() && data.repr.is_some() {
                        replacements.replace(node, data.inputs[0]);
                    }
                    removed.insert(node.index());
                }
                _ => {}
            }
        }
        exit_properties[block_index] = Some(properties);
        exit_shapes[block_index] = Some(shapes);
    }

    for (phi, block, key) in pending_phis {
        let predecessors = graph.block(block).predecessors.clone();
        for (index, predecessor) in predecessors.iter().enumerate() {
            if predecessor.index() < block.index() {
                continue;
            }
            // NB: A path that does not define the object leaves the phi's
            //     value as it is.
            let value = exit_properties[predecessor.index()]
                .as_ref()
                .and_then(|exit| exit.get(&key).copied())
                .unwrap_or(phi);
            let value = replacements.resolve(value);
            if is_replaced(value) {
                conflicts.extend(analysis.root(value));
            }
            graph.nodes[phi.index()].inputs[index] = value;
        }
    }

    if !conflicts.is_empty() {
        conflicts.sort_unstable();
        conflicts.dedup();
        return Err(conflicts);
    }
    replacements.apply(graph);
    edit::remove_nodes(graph, &removed);
    edit::remove_trivial_phis(graph);
    Ok(())
}

/// A copy of the frame state chain `frame_state` with the objects that were
/// replaced made virtual objects with the shapes and property values they
/// have there, or `None` if it refers to no such object.
fn virtualize_frame_state(
    graph: &mut Graph,
    analysis: &Analysis,
    is_replaced: &dyn Fn(NodeId) -> bool,
    replacements: &Replacements,
    (properties, shapes): (&Properties, &Shapes),
    frame_state: FrameStateId,
) -> Option<FrameStateId> {
    let chain = graph.frame_state_chain(frame_state);
    let refers_to_replaced = chain.iter().any(|id| {
        graph
            .frame_state(*id)
            .values
            .iter()
            .any(|(_, value)| is_replaced(replacements.resolve(*value)))
    });
    if !refers_to_replaced {
        return None;
    }
    // The virtual object of each replaced object, made once for this point.
    let mut objects: HashMap<NodeId, NodeId> = HashMap::default();
    let virtual_object = |graph: &mut Graph, objects: &mut HashMap<NodeId, NodeId>, value: NodeId| -> NodeId {
        let value = replacements.resolve(value);
        if !is_replaced(value) {
            return value;
        }
        let root = analysis.root(value).expect("replaced objects have roots");
        if let Some(object) = objects.get(&root) {
            return *object;
        }
        let (shape, _) = shapes[&root];
        let object = graph.add_node(Node::new(
            Op::VirtualObject { shape },
            Vec::new(),
            Some(Repr::Tagged),
            graph.node(root).pc,
        ));
        objects.insert(root, object);
        object
    };
    let mut parent = None;
    for id in chain.iter().rev() {
        let mut copy = graph.frame_state(*id).clone();
        for (_, value) in &mut copy.values {
            *value = virtual_object(graph, &mut objects, *value);
        }
        copy.parent = parent;
        parent = Some(graph.add_frame_state(copy));
    }
    // The properties of the virtual objects, which may be virtual objects
    // themselves.
    let mut filled = Vec::new();
    while let Some((root, object)) = objects
        .iter()
        .map(|(root, object)| (*root, *object))
        .find(|(_, object)| !filled.contains(object))
    {
        filled.push(object);
        let (_, property_count) = shapes[&root];
        let inputs = (0..property_count)
            .map(|offset| {
                let property = properties
                    .get(&(root, offset))
                    .copied()
                    .expect("replaced objects have every property");
                virtual_object(graph, &mut objects, property)
            })
            .collect::<Vec<_>>();
        graph.nodes[object.index()].inputs = inputs;
    }
    parent
}
