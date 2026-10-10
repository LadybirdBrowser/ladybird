/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a compiled graph contains, as coverage keys for the test suites'
//! coverage report (see `CompileOptions::coverage`): one key per node, its
//! op's name and, for ops that come in kinds, its kind, like
//! "Int32Binary.Add", "LoadElementAt.TypedArray(Int8)" or "CallSlowPath.GetById",
//! and one key per place compiled code may exit, like "exit-site.BadShape".

use crate::ir::BranchCondition;
use crate::ir::Graph;
use crate::ir::Op;

/// The coverage keys of the nodes of `graph` that made it into the code.
pub fn coverage_keys(graph: &Graph) -> Vec<String> {
    let mut keys = Vec::new();
    for block in &graph.blocks {
        for node in block.nodes() {
            let op = &graph.node(node).op;
            keys.push(op_key(op));
            if let Some(kind) = op.exit_kind() {
                keys.push(format!("exit-site.{kind:?}"));
            }
        }
    }
    if graph.blocks.iter().any(|block| block.is_loop_header) {
        keys.push("graph.loop".to_string());
    }
    if !graph.osr_entries.is_empty() {
        keys.push("graph.osr-entry".to_string());
    }
    let inlined_depth = graph
        .frame_states
        .iter()
        .enumerate()
        .map(|(index, _)| graph.frame_state_chain(crate::ir::FrameStateId(index as u32)).len())
        .max()
        .unwrap_or(0);
    if inlined_depth > 1 {
        keys.push(format!("graph.inlined-depth.{}", (inlined_depth - 1).min(5)));
    }
    keys
}

fn op_key(op: &Op) -> String {
    let name = op.name();
    match op {
        Op::Int32Binary { op } => format!("{name}.{op:?}"),
        Op::Int32Compare { comparison } => format!("{name}.{comparison:?}"),
        Op::CheckElements { kind } | Op::LoadElementAt { kind } | Op::StoreElementAt { kind } => {
            format!("{name}.{kind:?}")
        }
        Op::Generic { opcode, .. } | Op::CallSlowPath { opcode, .. } => {
            format!("{name}.{}", opcode.name())
        }
        Op::Exit { kind } | Op::CheckValue { kind, .. } => format!("{name}.{kind:?}"),
        Op::TypeofIs { kind, .. } => format!("{name}.{kind:?}"),
        Op::Branch { condition, .. } => match condition {
            BranchCondition::Int32(comparison) => format!("{name}.Int32.{comparison:?}"),
            // NB: The key names the condition, not what else it holds
            //     (like identifiers or binding indices).
            other => {
                let condition = format!("{other:?}");
                let kind = condition.split(['(', ' ', '{']).next().unwrap_or_default();
                format!("{name}.{kind}")
            }
        },
        Op::TaggedEquals { equal } => format!("{name}.{}", if *equal { "Equal" } else { "NotEqual" }),
        Op::CheckShape { shapes } => format!(
            "{name}.{}",
            if shapes.len() > 1 { "Polymorphic" } else { "Monomorphic" }
        ),
        _ => name.to_string(),
    }
}
