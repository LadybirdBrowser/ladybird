/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Enumerated keys: the keyed loads and `hasOwnProperty` probes of a for-in
//! loop's key, where the key comes straight from the loop's property
//! iterator cache, get the cache and the key's index as inputs too. For
//! objects of the shape the cache has, code generation then finds the
//! property at the offset the cache has for the key, without probing a
//! lookup cache.

use crate::ir::Graph;
use crate::ir::Op;
use ak::Zone;

pub fn run(graph: &mut Graph, _work: &Zone) {
    for block in 0..graph.blocks.len() {
        for index in 0..graph.blocks[block].body.len() {
            let node = graph.blocks[block].body[index];
            let data = graph.node(node);
            let probes_key = match data.op {
                Op::ProbeKeyedCache { .. } => true,
                Op::ProbeHasProperty { own } => own,
                _ => false,
            };
            if !probes_key || data.inputs.len() != 2 {
                continue;
            }
            let key = graph.node(graph.unrefined(data.inputs[1]));
            if key.op != Op::LoadPropertyIteratorKey {
                continue;
            }
            let (cache, key_index) = (key.inputs[0], key.inputs[1]);
            graph.nodes[node.index()].inputs.extend([cache, key_index]);
        }
    }
}
