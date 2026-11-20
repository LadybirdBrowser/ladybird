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

use core::fmt;

use crate::ir::BranchCondition;
use crate::ir::Graph;
use crate::ir::Op;

/// Writes the coverage keys of the nodes of `graph` that made it into the code to `out`, one on each line.
pub fn write_coverage_keys(graph: &Graph, out: &mut dyn fmt::Write) -> fmt::Result {
    for block in &graph.blocks {
        for node in block.nodes() {
            let op = &graph.node(node).op;
            writeln!(out, "{}", OpKey(op))?;
            if let Some(kind) = op.exit_kind() {
                writeln!(out, "exit-site.{kind:?}")?;
            }
        }
    }
    if graph.blocks.iter().any(|block| block.is_loop_header) {
        writeln!(out, "graph.loop")?;
    }
    if !graph.osr_entries.is_empty() {
        writeln!(out, "graph.osr-entry")?;
    }
    let inlined_depth = graph
        .frame_states
        .iter()
        .enumerate()
        .map(|(index, _)| graph.frame_state_chain(crate::ir::FrameStateId(index as u32)).count())
        .max()
        .unwrap_or(0);
    if inlined_depth > 1 {
        writeln!(out, "graph.inlined-depth.{}", (inlined_depth - 1).min(5))?;
    }
    Ok(())
}

/// The coverage key of an op.
struct OpKey<'a, 'z>(&'a Op<'z>);

impl fmt::Display for OpKey<'_, '_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let op = self.0;
        let name = op.name();
        match op {
            Op::Int32Binary { op } | Op::Int32WrappingBinary { op } => write!(f, "{name}.{op:?}"),
            Op::Int32Compare { comparison } => write!(f, "{name}.{comparison:?}"),
            Op::CheckElements { kind } | Op::LoadElementAt { kind } | Op::StoreElementAt { kind } => {
                write!(f, "{name}.{kind:?}")
            }
            Op::Generic { opcode, .. } | Op::CallSlowPath { opcode, .. } => {
                write!(f, "{name}.{}", opcode.name())
            }
            Op::Exit { kind } | Op::CheckValue { kind, .. } => write!(f, "{name}.{kind:?}"),
            Op::TypeofIs { kind, .. } => write!(f, "{name}.{kind:?}"),
            Op::Branch { condition, .. } => match condition {
                BranchCondition::Int32(comparison) => write!(f, "{name}.Int32.{comparison:?}"),
                // NB: The key names the condition, not what else it holds
                //     (like identifiers or binding indices).
                other => {
                    write!(f, "{name}.")?;
                    let mut first_word = FirstWord { out: f, done: false };
                    fmt::Write::write_fmt(&mut first_word, format_args!("{other:?}"))
                }
            },
            Op::TaggedEquals { equal } => write!(f, "{name}.{}", if *equal { "Equal" } else { "NotEqual" }),
            Op::CheckShape { shapes } => write!(
                f,
                "{name}.{}",
                if shapes.len() > 1 { "Polymorphic" } else { "Monomorphic" }
            ),
            _ => f.write_str(name),
        }
    }
}

/// Passes text on up to the first `(`, ` ` or `{`, and drops the rest.
struct FirstWord<'a, 'b> {
    out: &'a mut fmt::Formatter<'b>,
    done: bool,
}

impl fmt::Write for FirstWord<'_, '_> {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        if self.done {
            return Ok(());
        }
        match text.find(['(', ' ', '{']) {
            Some(end) => {
                self.done = true;
                self.out.write_str(&text[..end])
            }
            None => self.out.write_str(text),
        }
    }
}
