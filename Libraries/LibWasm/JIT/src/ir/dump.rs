/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The text form of the IR, for `dump-ir` and the IR tests.

use super::{FloatCondition, Func, IntCondition, LoadKind, Op, Shape, Type, Value};
use std::fmt::Write;

/// The value as text: a constant's value, or `vN`.
fn operand(func: &Func, value: Value) -> String {
    let node = func.node(value);
    match node.op {
        Op::Const32 if node.ty == Type::F32 => format!("{}f32", f32::from_bits(node.args[0])),
        Op::Const32 => format!("{}", node.args[0] as i32),
        Op::Const64 if node.ty == Type::F64 => {
            format!("{}f64", f64::from_bits(node.constant().expect("a constant")))
        }
        Op::Const64 => format!("{}i64", node.constant().expect("a constant") as i64),
        Op::Const128 => format!("{:#034x}v128", func.constants[node.args[0] as usize]),
        _ => format!("v{value}"),
    }
}

/// One node as text, without its value name.
pub fn node_text(func: &Func, value: Value) -> String {
    let node = func.node(value);
    let mut text = String::from(node.op.name());
    if node.ty != Type::None {
        write!(text, ".{}", node.ty.name()).unwrap();
    }
    match node.op {
        Op::Compare | Op::BranchCompare => write!(text, " {}", IntCondition::from_flags(node.flags).name()).unwrap(),
        Op::FloatCompare | Op::BranchFloatCompare => {
            write!(text, " {}", FloatCondition::from_flags(node.flags).name()).unwrap();
        }
        Op::Load => write!(text, " {}", LoadKind::from_flags(node.flags).name()).unwrap(),
        Op::Store => write!(text, " {}", 8 << node.flags).unwrap(),
        _ => {}
    }
    let inputs = func.inputs(value);
    if !inputs.is_empty() && node.op != Op::BrTable {
        let inputs = inputs.iter().map(|input| operand(func, *input)).collect::<Vec<_>>();
        write!(text, " {}", inputs.join(", ")).unwrap();
    }
    match node.op {
        Op::Const32 | Op::Const64 | Op::Const128 => write!(text, " {}", operand(func, value)).unwrap(),
        Op::VectorUnary | Op::ExtractLane => write!(text, " simd {:#x}", node.args[1]).unwrap(),
        Op::VectorBinary => write!(text, " simd {:#x}", node.args[2]).unwrap(),
        Op::ReplaceLane => write!(text, " simd {:#x} lane {}", node.args[2] & 0xffff, node.args[2] >> 16).unwrap(),
        Op::Shuffle => write!(text, " {:#034x}", func.constants[node.args[2] as usize]).unwrap(),
        Op::VectorLoad => write!(text, " simd {:#x} +{}", node.flags, node.args[2]).unwrap(),
        Op::VectorTernary => write!(text, " simd {:#x}", node.flags).unwrap(),
        Op::Param => write!(text, " {}", node.args[0]).unwrap(),
        Op::Forward => write!(text, " v{}", node.args[0]).unwrap(),
        Op::InstanceField | Op::LoadPointer => write!(text, " +{}", node.args[1]).unwrap(),
        Op::GlobalGet => write!(text, " global {}", node.args[1]).unwrap(),
        Op::GlobalSet => write!(text, " global {}", node.args[2]).unwrap(),
        Op::Load => write!(text, " +{}", node.args[2]).unwrap(),
        Op::Store => write!(text, " +{}", node.args[2]).unwrap(),
        Op::Call | Op::TailCall if node.args[2] != crate::ir::NONE => {
            write!(text, " function {}", node.args[2]).unwrap();
        }
        Op::Offset | Op::ReadField => write!(text, " +{}", node.args[1]).unwrap(),
        Op::FrameLoad => write!(text, " {:+}", node.args[1] as i32).unwrap(),
        Op::WriteField => write!(text, " +{}", node.args[2]).unwrap(),
        Op::Helper => write!(text, " {}", crate::ir::Helper::from_index(node.args[2]).name()).unwrap(),
        Op::CallResult => write!(text, " v{} #{}", node.args[0], node.args[1]).unwrap(),
        Op::Trap => write!(text, " {}", node.args[0]).unwrap(),
        Op::CheckedAddress => write!(text, " size {} flags {:#x}", 1 << (node.flags & 7), node.flags & !7).unwrap(),
        Op::AtomicRmw => write!(
            text,
            " {} size {}",
            crate::ir::AtomicOp::from_index(node.args[2]).name(),
            1 << node.flags
        )
        .unwrap(),
        Op::AtomicCompareExchange | Op::ElementAddress => write!(text, " size {}", 1 << node.flags).unwrap(),
        Op::Rtt => write!(text, " type {}", node.args[1]).unwrap(),
        Op::I31Get if node.flags != 0 => text.push_str(" signed"),
        Op::RefTest => write!(
            text,
            " {}{}{} depth {}",
            crate::ir::HeapTest::from_flags(node.flags).name(),
            if node.flags & crate::ir::REF_TEST_NULLABLE != 0 {
                " null"
            } else {
                ""
            },
            if node.flags & crate::ir::REF_TEST_CAST != 0 {
                " cast"
            } else {
                ""
            },
            node.args[2]
        )
        .unwrap(),
        Op::BrTable => {
            let block = func
                .blocks
                .iter()
                .position(|block| block.end == value + 1)
                .expect("a br_table ends a block") as u32;
            let successors = func.successors(block);
            let targets = func
                .table_targets(value)
                .iter()
                .map(|target| format!("b{}", successors[*target as usize]))
                .collect::<Vec<_>>();
            write!(text, " {} [{}]", operand(func, node.args[0]), targets.join(", ")).unwrap();
        }
        _ => {}
    }
    if matches!(node.op.shape(), Shape::Fixed(_)) && node.flags & super::FLAG_CANNOT_TRAP != 0 {
        text.push_str(" (cannot trap)");
    }
    text
}

/// The whole function as text.
pub fn dump(func: &Func) -> String {
    let mut out = String::new();
    let types = |types: &[Type]| types.iter().map(|ty| ty.name()).collect::<Vec<_>>().join(", ");
    writeln!(out, "function ({}) -> ({})", types(&func.params), types(&func.results)).unwrap();
    for (index, block) in func.blocks.iter().enumerate() {
        let block_id = index as u32;
        let list = |blocks: &[u32]| {
            blocks
                .iter()
                .map(|block| format!("b{block}"))
                .collect::<Vec<_>>()
                .join(", ")
        };
        write!(out, "b{index}:").unwrap();
        if !func.predecessors(block_id).is_empty() {
            write!(out, " <- {}", list(func.predecessors(block_id))).unwrap();
        }
        if block.loop_index != super::NONE {
            let loop_ = &func.loops[block.loop_index as usize];
            if loop_.header == block_id {
                write!(out, " (loop to b{})", loop_.last).unwrap();
            }
        }
        out.push('\n');
        for phi in func.phis(block_id) {
            if func.node(phi).op == Op::Phi {
                writeln!(out, "  v{phi} = {}", node_text(func, phi)).unwrap();
            }
        }
        for value in block.start..block.end {
            let node = func.node(value);
            if (node.op.is_entry_value() && node.op != Op::Param) || matches!(node.op, Op::Phi | Op::Forward) {
                continue;
            }
            if node.op.is_terminator() {
                write!(out, "  {}", node_text(func, value)).unwrap();
                if !func.successors(block_id).is_empty() && node.op != Op::BrTable {
                    write!(out, " -> {}", list(func.successors(block_id))).unwrap();
                }
                out.push('\n');
            } else if node.ty == Type::None {
                writeln!(out, "  {}", node_text(func, value)).unwrap();
            } else {
                writeln!(out, "  v{value} = {}", node_text(func, value)).unwrap();
            }
        }
    }
    out
}
