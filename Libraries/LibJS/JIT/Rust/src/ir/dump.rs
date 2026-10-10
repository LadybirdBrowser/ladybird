/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A readable textual form of the IR.

use super::BlockId;
use super::BranchCondition;
use super::Graph;
use super::NodeId;
use super::Op;
use super::ShapeCheck;
use super::value;
use crate::bytecode::FrameLayout;
use crate::bytecode::Operand;
use crate::bytecode::SlotKind;
use crate::code::Repr;
use crate::code::ResumeMode;
use std::fmt::Write;

/// The name of a frame slot, like the bytecode disassembler prints operands.
pub fn slot_name(layout: &FrameLayout, slot: u32) -> String {
    match layout.slot_kind(Operand::from_raw(slot)) {
        SlotKind::Register(index) => format!("r{index}"),
        SlotKind::Local(index) => format!("l{index}"),
        SlotKind::Constant(index) => format!("c{index}"),
        SlotKind::Argument(index) => format!("a{index}"),
    }
}

fn value_name(graph: &Graph, id: NodeId) -> String {
    match (graph.constant_value(id), graph.node(id).repr) {
        (Some(bits), Some(Repr::Int32)) => format!("#i32:{}", (bits as u32).cast_signed()),
        (Some(bits), Some(Repr::Bool)) => format!("#bool:{}", bits != 0),
        (Some(bits), Some(Repr::Pointer)) => format!("#pointer:{bits:#x}"),
        (Some(bits), _) => format!("#{}", value::describe(bits)),
        (None, _) => format!("v{}", id.0),
    }
}

fn block_name(block: BlockId) -> String {
    format!("b{}", block.0)
}

fn repr_name(repr: Repr) -> &'static str {
    match repr {
        Repr::Tagged => "tagged",
        Repr::Int32 => "int32",
        Repr::Float64 => "float64",
        Repr::Bool => "bool",
        Repr::Pointer => "pointer",
    }
}

fn shapes_text<'a>(shapes: impl Iterator<Item = &'a ShapeCheck>) -> String {
    shapes
        .map(|shape| match shape.dictionary_generation {
            Some(generation) => format!("{:#x}@{generation}", shape.shape.0),
            None => format!("{:#x}", shape.shape.0),
        })
        .collect::<Vec<_>>()
        .join(", ")
}

fn condition_text(condition: BranchCondition) -> String {
    match condition {
        BranchCondition::Bool => "Bool".to_string(),
        BranchCondition::Nullish => "Nullish".to_string(),
        BranchCondition::Undefined => "Undefined".to_string(),
        BranchCondition::Object => "Object".to_string(),
        BranchCondition::ElementsKind(kind) => format!("ElementsKind {kind:?}"),
        BranchCondition::IndexInBounds => "IndexInBounds".to_string(),
        BranchCondition::MagicalLength => "MagicalLength".to_string(),
        BranchCondition::Extensible => "Extensible".to_string(),
        BranchCondition::Shape(shape) => format!("Shape {}", shapes_text(std::iter::once(&shape))),
        BranchCondition::NonNegativeInt32 => "NonNegativeInt32".to_string(),
        BranchCondition::Int32(comparison) => format!("Int32{comparison:?}"),
        BranchCondition::Float64(comparison) => format!("Float64{comparison:?}"),
        BranchCondition::TaggedEquals { equal: true } => "TaggedEquals".to_string(),
        BranchCondition::TaggedEquals { equal: false } => "TaggedNotEquals".to_string(),
        BranchCondition::Int32Value => "Int32Value".to_string(),
        BranchCondition::Double => "Double".to_string(),
        BranchCondition::String => "String".to_string(),
        BranchCondition::ResolvedString => "ResolvedString".to_string(),
        BranchCondition::Builtin(id) => format!("Builtin {id}"),
        BranchCondition::PropertyIteratorCacheValid => "PropertyIteratorCacheValid".to_string(),
        BranchCondition::BindingMutable { index } => format!("BindingMutable {index}"),
        BranchCondition::NextBindingOfShape { name, flags } => format!("NextBindingOfShape {name:#x} {flags}"),
    }
}

/// The text of a node's op, with `inputs` (already formatted) in place.
fn op_text(layout: &FrameLayout, op: &Op, inputs: &str) -> String {
    let with_inputs = |name: &str| {
        if inputs.is_empty() {
            name.to_string()
        } else {
            format!("{name} {inputs}")
        }
    };
    match op {
        Op::Constant(bits) => format!("Constant {}", value::describe(*bits)),
        Op::LoadSlot { slot } => with_inputs(&format!("LoadSlot {}", slot_name(layout, *slot))),
        Op::StoreSlot { slot } => with_inputs(&format!("StoreSlot {}", slot_name(layout, *slot))),
        Op::Phi => with_inputs("Phi"),
        Op::InitializeFrame => with_inputs("InitializeFrame"),
        Op::EnsureFrameInitialized => with_inputs("EnsureFrameInitialized"),
        Op::PublishFrame => with_inputs("PublishFrame"),
        Op::BoxCell => with_inputs("BoxCell"),
        Op::LoadOuterEnvironment => with_inputs("LoadOuterEnvironment"),
        Op::LoadEnvironmentBinding { index } => with_inputs(&format!("LoadEnvironmentBinding {index}")),
        Op::StoreEnvironmentBinding { index } => with_inputs(&format!("StoreEnvironmentBinding {index}")),
        Op::AppendEnvironmentBinding => with_inputs("AppendEnvironmentBinding"),
        Op::SetLexicalEnvironment => with_inputs("SetLexicalEnvironment"),
        Op::LeavePrivateEnvironment => with_inputs("LeavePrivateEnvironment"),
        Op::IsCallable => with_inputs("IsCallable"),
        Op::CheckInt32 => with_inputs("CheckInt32"),
        Op::CheckNumber => with_inputs("CheckNumber"),
        Op::CheckIdentityComparable => with_inputs("CheckIdentityComparable"),
        Op::BoxInt32 => with_inputs("BoxInt32"),
        Op::BoxBool => with_inputs("BoxBool"),
        Op::CellAddress => with_inputs("CellAddress"),
        Op::Int32Binary { op } => with_inputs(&format!("Int32{op:?}")),
        Op::Int32Compare { comparison } => with_inputs(&format!("Int32{comparison:?}")),
        Op::CheckElements { kind } => with_inputs(&format!("CheckElements {kind:?}")),
        Op::LoadElementAt { kind } | Op::StoreElementAt { kind } => with_inputs(&format!("{} {kind:?}", op.name())),
        Op::TaggedEquals { equal: true } => with_inputs("TaggedEquals"),
        Op::TaggedEquals { equal: false } => with_inputs("TaggedNotEquals"),
        Op::ToBoolean => with_inputs("ToBoolean"),
        Op::PrimitiveToString | Op::ToObject | Op::ArrayCreate => with_inputs(op.name()),
        Op::Generic { opcode, executable, pc } => {
            let location = if *executable == 0 {
                format!("@{pc}")
            } else {
                format!("@e{executable}:{pc}")
            };
            with_inputs(&format!("Generic {} {location}", opcode.name()))
        }
        Op::ArgumentCount => with_inputs("ArgumentCount"),
        Op::SliceArguments => with_inputs("SliceArguments"),
        Op::LoadFrameField { field } => with_inputs(&format!("LoadFrameField {field:?}")),
        Op::LoadArgument { .. } => with_inputs("LoadArgument"),
        Op::CallForwardingArguments { executable, pc } => {
            let location = if *executable == 0 {
                format!("@{pc}")
            } else {
                format!("@e{executable}:{pc}")
            };
            with_inputs(&format!("CallForwardingArguments {location}"))
        }
        Op::CallDirect {
            executable,
            pc,
            inlined_frame_bytes,
            stores_operands,
        } => {
            let location = if *executable == 0 {
                format!("@{pc}")
            } else {
                format!("@e{executable}:{pc}")
            };
            let frames = if *inlined_frame_bytes == 0 {
                String::new()
            } else {
                format!(" without inlined frames ({inlined_frame_bytes} bytes)")
            };
            let stores = if *stores_operands { " storing operands" } else { "" };
            with_inputs(&format!("CallDirect {location}{frames}{stores}"))
        }
        Op::CallNative {
            executable,
            pc,
            inlined_frame_bytes,
            stores_operands,
        } => {
            let location = if *executable == 0 {
                format!("@{pc}")
            } else {
                format!("@e{executable}:{pc}")
            };
            let frames = if *inlined_frame_bytes == 0 {
                String::new()
            } else {
                format!(" without inlined frames ({inlined_frame_bytes} bytes)")
            };
            let stores = if *stores_operands { " storing operands" } else { "" };
            with_inputs(&format!("CallNative {location}{frames}{stores}"))
        }
        Op::AllocateObject {
            shape,
            property_count,
            reserve,
        } => {
            format!(
                "AllocateObject {:#x} with {property_count} properties, room for {reserve}",
                shape.0
            )
        }
        Op::InitializeNamed { offset } => with_inputs(&format!("InitializeNamed +{offset}")),
        Op::AllocateArray { count } => format!("AllocateArray with {count} elements"),
        Op::VirtualObject { shape } => with_inputs(&format!("VirtualObject {:#x}", shape.0)),
        Op::AllocateEnvironment { shape_cache, capacity } => {
            with_inputs(&format!("AllocateEnvironment {shape_cache} capacity {capacity}"))
        }
        Op::AllocateFunction {
            shared_function_data_index,
        } => with_inputs(&format!("AllocateFunction {shared_function_data_index}")),
        Op::InitializeElement { index } => with_inputs(&format!("InitializeElement [{index}]")),
        Op::CheckObject => with_inputs("CheckObject"),
        Op::CheckValue { expected, .. } => with_inputs(&format!("CheckValue {expected:#x}")),
        Op::CheckClosure { shared_data } => with_inputs(&format!("CheckClosure {:#x}", shared_data.0)),
        Op::LoadFunctionEnvironment { private } => with_inputs(if *private {
            "LoadFunctionEnvironment private"
        } else {
            "LoadFunctionEnvironment"
        }),
        Op::EmptyToUndefined => with_inputs("EmptyToUndefined"),
        Op::Typeof => with_inputs("Typeof"),
        Op::TypeofIs { kind, equal } => with_inputs(&format!("TypeofIs{} {kind:?}", if *equal { "" } else { "Not" })),
        Op::CheckShape { shapes } => with_inputs(&format!("CheckShape [{}]", shapes_text(shapes.iter()))),
        Op::CheckPrototypeChainValid { validity } => format!("CheckPrototypeChainValid {:#x}", validity.0),
        Op::AssumeValid => "AssumeValid".to_string(),
        Op::HasInPrototypeChain => with_inputs("HasInPrototypeChain"),
        Op::LoadGlobalBinding { environment, index } => format!("LoadGlobalBinding {:#x}[{index}]", environment.0),
        Op::StoreGlobalBinding { environment, index } => {
            with_inputs(&format!("StoreGlobalBinding {:#x}[{index}]", environment.0))
        }
        Op::LoadAccessorFunction { offset, part } => with_inputs(&format!("LoadAccessorFunction +{offset} {part:?}")),
        Op::CheckAccessorFunction { offset, part, function } => {
            with_inputs(&format!("CheckAccessorFunction +{offset} {part:?} {:#x}", function.0))
        }
        Op::LoadNamed { offset } => with_inputs(&format!("LoadNamed +{offset}")),
        Op::StoreNamed { offset } => with_inputs(&format!("StoreNamed +{offset}")),
        Op::AddNamed { offset, shape, .. } => with_inputs(&format!("AddNamed +{offset} to {:#x}", shape.0)),
        Op::ShapeSwitch { cases } => {
            let cases = cases
                .iter()
                .map(|(shape, block)| format!("{} -> {}", shapes_text(std::iter::once(shape)), block_name(*block)))
                .collect::<Vec<_>>();
            format!("{} [{}]", with_inputs("ShapeSwitch"), cases.join(", "))
        }
        Op::CallSlowPath {
            opcode, executable, pc, ..
        } => {
            let location = if *executable == 0 {
                format!("@{pc}")
            } else {
                format!("@e{executable}:{pc}")
            };
            with_inputs(&format!("{} {} {location}", op.name(), opcode.name()))
        }
        Op::Float64Unary { op } => with_inputs(&format!("Float64{}", op.name())),
        Op::Float64Binary { op } => with_inputs(&format!("Float64{op:?}")),
        Op::Float64Compare { comparison } => with_inputs(&format!("Float64{comparison:?}")),
        Op::Refine { condition, holds } => {
            let negation = if *holds { "" } else { "Not" };
            with_inputs(&format!("Refine{negation}{}", condition_text(*condition)))
        }
        Op::SlowPathOutput { index } => with_inputs(&format!("SlowPathOutput {index}")),
        Op::UnboxInt32
        | Op::Int32Abs
        | Op::Int32ToFloat64
        | Op::UnboxDouble
        | Op::BoxFloat64
        | Op::StringAddress
        | Op::StringLength
        | Op::LoadStringCodeUnit
        | Op::SingleCharacterString
        | Op::Float64ToInt32
        | Op::Uint32ShiftRight
        | Op::Uint32ToFloat64
        | Op::StringsEqual
        | Op::ConcatenateStrings
        | Op::IntegerToString
        | Op::CheckAppendableArray
        | Op::LoadElementsLength
        | Op::LoadElementsCapacity
        | Op::AppendElement
        | Op::CallArrayPush
        | Op::LoadPropertyIteratorKeyCount
        | Op::LoadPropertyIteratorKey => with_inputs(op.name()),
        Op::CheckBounds | Op::CheckNotHole | Op::LoadTypedArrayLength => with_inputs(op.name()),
        Op::ProbeHasProperty { own: true } => with_inputs("ProbeHasOwnProperty"),
        Op::ProbeHasProperty { own: false } => with_inputs("ProbeHasProperty"),
        Op::ProbeGlobalCache { cache } | Op::ProbeGlobalStore { cache } => {
            with_inputs(&format!("{} cache {cache}", op.name()))
        }
        Op::ProbePropertyCache { executable, cache }
        | Op::ProbeKeyedCache { executable, cache }
        | Op::ProbeKeyedStore { executable, cache }
        | Op::ProbePropertyStore { executable, cache } => {
            let cache = if *executable == 0 {
                format!("cache {cache}")
            } else {
                format!("cache e{executable}:{cache}")
            };
            with_inputs(&format!("{} {cache}", op.name()))
        }
        Op::Jump { target } => format!("Jump {}", block_name(*target)),
        Op::Branch {
            condition,
            if_true,
            if_false,
        } => {
            let condition = condition_text(*condition);
            format!(
                "{} -> {}, {}",
                with_inputs(&format!("Branch{condition}")),
                block_name(*if_true),
                block_name(*if_false)
            )
        }
        Op::BranchTruthy {
            if_true,
            if_false,
            fallback,
        } => format!(
            "{} -> {}, {}, fallback {}",
            with_inputs("BranchTruthy"),
            block_name(*if_true),
            block_name(*if_false),
            block_name(*fallback)
        ),
        Op::BranchOnPc { pc, if_true, if_false } => format!(
            "{} -> {}, {}",
            with_inputs(&format!("BranchOnPc @{pc}")),
            block_name(*if_true),
            block_name(*if_false)
        ),
        Op::Return => with_inputs("Return"),
        Op::Exit { kind } => with_inputs(&format!("Exit {kind:?}")),
        Op::Unreachable => with_inputs("Unreachable"),
    }
}

/// One node as text, like `v3 = Generic GetById @12 (v1) [fs2]`.
pub fn node_text(graph: &Graph, layout: &FrameLayout, node_id: NodeId) -> String {
    let node = graph.node(node_id);
    let mut out = String::new();
    if let Some(repr) = node.repr {
        write!(out, "v{}", node_id.0).unwrap();
        if repr != Repr::Tagged {
            write!(out, ":{}", repr_name(repr)).unwrap();
        }
        out.push_str(" = ");
    }
    let inputs = if node.inputs.is_empty() {
        String::new()
    } else {
        let inputs = node
            .inputs
            .iter()
            .map(|input| value_name(graph, *input))
            .collect::<Vec<_>>();
        format!("({})", inputs.join(", "))
    };
    out.push_str(&op_text(layout, &node.op, &inputs));
    if let Some(frame_state) = node.frame_state {
        write!(out, " [fs{}]", frame_state.0).unwrap();
    }
    out
}

/// Prints `graph` as text, naming frame slots according to `layout`.
pub fn dump(graph: &Graph, layout: &FrameLayout) -> String {
    let mut out = String::new();
    for (index, block) in graph.blocks.iter().enumerate() {
        let id = BlockId::from_index(index);
        write!(out, "{}", block_name(id)).unwrap();
        if let Some(pc) = block.bytecode_start {
            write!(out, " (pc {pc})").unwrap();
        }
        if !block.predecessors.is_empty() {
            let predecessors = block
                .predecessors
                .iter()
                .map(|block| block_name(*block))
                .collect::<Vec<_>>();
            write!(out, " <- {}", predecessors.join(", ")).unwrap();
        }
        if block.is_loop_header {
            out.push_str(" loop");
        }
        if block.is_cold {
            out.push_str(" cold");
        }
        out.push_str(":\n");
        for node_id in graph.block_nodes(id) {
            out.push_str("  ");
            out.push_str(&node_text(graph, layout, node_id));
            out.push('\n');
        }
    }
    for (pc, block) in &graph.osr_entries {
        writeln!(out, "osr entry at pc {pc}: b{}", block.0).unwrap();
    }
    if !graph.frame_states.is_empty() {
        out.push_str("frame states:\n");
    }
    for (index, frame_state) in graph.frame_states.iter().enumerate() {
        write!(out, "  fs{index}: ").unwrap();
        if frame_state.executable != 0 {
            write!(out, "e{} ", frame_state.executable).unwrap();
        }
        write!(out, "pc {}", frame_state.pc).unwrap();
        match frame_state.mode {
            ResumeMode::ResumeAt => out.push_str(" at"),
            ResumeMode::ResumeAfter { dst } if dst == Operand::INVALID => out.push_str(" after"),
            ResumeMode::ResumeAfter { dst } => write!(out, " after -> {}", slot_name(layout, dst)).unwrap(),
        }
        let values = frame_state
            .values
            .iter()
            .map(|(slot, value)| format!("{}={}", slot_name(layout, *slot), value_name(graph, *value)))
            .collect::<Vec<_>>();
        write!(out, " {{{}}}", values.join(", ")).unwrap();
        if !frame_state.in_frame.is_empty() {
            let in_frame = frame_state
                .in_frame
                .iter()
                .map(|slot| slot_name(layout, *slot))
                .collect::<Vec<_>>();
            write!(out, " frame [{}]", in_frame.join(", ")).unwrap();
        }
        if let Some(parent) = frame_state.parent {
            write!(out, " in fs{}", parent.0).unwrap();
        }
        out.push('\n');
    }
    out
}
