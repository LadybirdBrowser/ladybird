/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Key strings: concatenations that are the keys of property accesses look
//! for the strings that they made before.
//!
//! `object["on" + type]` concatenates two strings into a rope string, which
//! the access resolves right away to find the property key. The rope string
//! is a new string every time, so the caches of the access, which know keys
//! by their strings, never have it, and the access takes its slow path every
//! time. The VM remembers the two strings of a rope that became a key, and
//! the string that it became. A concatenation whose value is the key of a
//! keyed access is marked (`Op::ConcatenateStrings::key`), and then takes
//! the string that the VM remembers for its two strings, if any, which the
//! caches of the access know by now.
//!
//! The key of an access is a phi of the concatenation and of what the slow
//! path of its instruction made, and may be one of several concatenations,
//! so the pass looks through phis and refinements. A string that a loop puts
//! together piece by piece is a key once, and its pieces never are, so the
//! pass does not look through the phis of loop headers, and leaves alone a
//! concatenation that adds to one. The strings of a marked concatenation
//! are looked at as well: the key `a + b + c` only finds its string if
//! `a + b` is the same string every time, which the VM remembers too. A
//! concatenation with other uses than keys is marked all the same: its
//! string has the same characters either way.

use crate::ir::Graph;
use crate::ir::NodeId;
use crate::ir::Op;
use ak::Zone;
use ak::ZoneBitSet;
use ak::ZoneVec;

pub fn run(graph: &mut Graph, work: &Zone) {
    let mut pending = ZoneVec::new_in(work);
    for block in &graph.blocks {
        for node in block.nodes() {
            let node = graph.node(node);
            if matches!(
                node.op,
                Op::ProbeKeyedCache { .. } | Op::ProbeKeyedStore { .. } | Op::ProbeHasProperty { .. }
            ) {
                pending.push(node.inputs[1]);
            }
        }
    }
    let mut loop_phis = ZoneBitSet::new_in(graph.nodes.len(), work);
    for block in graph.blocks.iter().filter(|block| block.is_loop_header) {
        for phi in &block.phis {
            loop_phis.insert(phi.index());
        }
    }
    let mut seen = ZoneBitSet::new_in(graph.nodes.len(), work);
    let mut concatenations: ZoneVec<NodeId> = ZoneVec::new_in(work);
    while let Some(value) = pending.pop() {
        let value = graph.unrefined(value);
        if seen.contains(value.index()) {
            continue;
        }
        seen.insert(value.index());
        let node = graph.node(value);
        match node.op {
            Op::Phi if !loop_phis.contains(value.index()) => pending.extend(&node.inputs),
            Op::ConcatenateStrings { key: false } => {
                let adds_to_loop_phi = node
                    .inputs
                    .iter()
                    .any(|&input| loop_phis.contains(graph.unrefined(input).index()));
                if adds_to_loop_phi {
                    continue;
                }
                concatenations.push(value);
                pending.extend(&node.inputs);
            }
            _ => {}
        }
    }
    for concatenation in concatenations {
        graph.nodes[concatenation.index()].op = Op::ConcatenateStrings { key: true };
    }
}
