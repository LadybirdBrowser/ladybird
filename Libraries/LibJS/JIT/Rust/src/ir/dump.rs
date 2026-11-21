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
use super::SuspendKind;
use super::VirtualObjectKind;
use super::value;
use crate::bytecode::FrameLayout;
use crate::bytecode::Operand;
use crate::bytecode::SlotKind;
use crate::code::Repr;
use crate::code::ResumeMode;
use core::fmt;

/// The name of a frame slot, like the bytecode disassembler prints operands.
pub fn slot_name(layout: &FrameLayout, slot: u32) -> SlotName {
    SlotName(layout.slot_kind(Operand::from_raw(slot)))
}

/// The text of `slot_name()`.
#[derive(Clone, Copy)]
pub struct SlotName(SlotKind);

impl fmt::Display for SlotName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self.0 {
            SlotKind::Register(index) => write!(f, "r{index}"),
            SlotKind::Local(index) => write!(f, "l{index}"),
            SlotKind::Constant(index) => write!(f, "c{index}"),
            SlotKind::Argument(index) => write!(f, "a{index}"),
        }
    }
}

/// Writes the text of each item, with ", " between them.
fn write_joined<T>(
    f: &mut fmt::Formatter<'_>,
    items: impl IntoIterator<Item = T>,
    mut write_item: impl FnMut(&mut fmt::Formatter<'_>, T) -> fmt::Result,
) -> fmt::Result {
    for (index, item) in items.into_iter().enumerate() {
        if index != 0 {
            f.write_str(", ")?;
        }
        write_item(f, item)?;
    }
    Ok(())
}

/// The name of a value: its node, or the constant it is.
struct ValueName<'a, 'z> {
    graph: &'a Graph<'z>,
    id: NodeId,
}

fn value_name<'a, 'z>(graph: &'a Graph<'z>, id: NodeId) -> ValueName<'a, 'z> {
    ValueName { graph, id }
}

impl fmt::Display for ValueName<'_, '_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let (graph, id) = (self.graph, self.id);
        match (graph.constant_value(id), graph.node(id).repr) {
            (Some(bits), Some(Repr::Int32)) => write!(f, "#i32:{}", (bits as u32).cast_signed()),
            (Some(bits), Some(Repr::Bool)) => write!(f, "#bool:{}", bits != 0),
            (Some(bits), Some(Repr::Pointer)) => write!(f, "#pointer:{bits:#x}"),
            (Some(bits), _) => write!(f, "#{}", value::describe(bits)),
            (None, _) => write!(f, "v{}", id.0),
        }
    }
}

struct BlockName(BlockId);

impl fmt::Display for BlockName {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "b{}", self.0.0)
    }
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

fn write_shapes(f: &mut fmt::Formatter<'_>, shapes: &[ShapeCheck]) -> fmt::Result {
    write_joined(f, shapes, |f, shape| match shape.dictionary_generation {
        Some(generation) => write!(f, "{:#x}@{generation}", shape.shape.0),
        None => write!(f, "{:#x}", shape.shape.0),
    })
}

struct ConditionText(BranchCondition);

impl fmt::Display for ConditionText {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let name = match self.0 {
            BranchCondition::Bool => "Bool",
            BranchCondition::Nullish => "Nullish",
            BranchCondition::Undefined => "Undefined",
            BranchCondition::Object => "Object",
            BranchCondition::ElementsKind(kind) => return write!(f, "ElementsKind {kind:?}"),
            BranchCondition::IndexInBounds => "IndexInBounds",
            BranchCondition::MagicalLength => "MagicalLength",
            BranchCondition::Extensible => "Extensible",
            BranchCondition::HoleFillableArray => "HoleFillableArray",
            BranchCondition::ArrayIterator => "ArrayIterator",
            BranchCondition::GeneratorObject => "GeneratorObject",
            BranchCondition::PlainArrayPrototypeChain => "PlainArrayPrototypeChain",
            BranchCondition::ArrayWithRealmArrayPrototypeAndNoNamedProperties => {
                "ArrayWithRealmArrayPrototypeAndNoNamedProperties"
            }
            BranchCondition::HoldsValidWatchpointSet => "HoldsValidWatchpointSet",
            BranchCondition::Shape(shape) => {
                f.write_str("Shape ")?;
                return write_shapes(f, core::slice::from_ref(&shape));
            }
            BranchCondition::NonNegativeInt32 => "NonNegativeInt32",
            BranchCondition::Int32(comparison) => return write!(f, "Int32{comparison:?}"),
            BranchCondition::Float64(comparison) => return write!(f, "Float64{comparison:?}"),
            BranchCondition::TaggedEquals { equal: true } => "TaggedEquals",
            BranchCondition::TaggedEquals { equal: false } => "TaggedNotEquals",
            BranchCondition::Int32Value => "Int32Value",
            BranchCondition::Int32OrBoolean => "Int32OrBoolean",
            BranchCondition::Double => "Double",
            BranchCondition::String => "String",
            BranchCondition::ResolvedString => "ResolvedString",
            BranchCondition::Builtin(id) => return write!(f, "Builtin {id}"),
            BranchCondition::PropertyIteratorCacheValid => "PropertyIteratorCacheValid",
            BranchCondition::BindingMutable { index } => return write!(f, "BindingMutable {index}"),
            BranchCondition::NextBindingOfShape { name, flags } => {
                return write!(f, "NextBindingOfShape {name:#x} {flags}");
            }
        };
        f.write_str(name)
    }
}

/// Where a call or a slow path call is in the bytecode: `@pc`, or `@e<executable>:pc` in an inlined executable.
struct Site {
    executable: u32,
    pc: u32,
}

impl fmt::Display for Site {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.executable == 0 {
            write!(f, "@{}", self.pc)
        } else {
            write!(f, "@e{}:{}", self.executable, self.pc)
        }
    }
}

/// The text of " without inlined frames (<bytes> bytes)", or of nothing for none.
struct InlinedFrames(u32);

impl fmt::Display for InlinedFrames {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.0 == 0 {
            return Ok(());
        }
        write!(f, " without inlined frames ({} bytes)", self.0)
    }
}

/// The inputs of a node, like `(v1, #i32:2)`, or nothing if it has none.
struct Inputs<'a, 'z> {
    graph: &'a Graph<'z>,
    inputs: &'a [NodeId],
}

impl Inputs<'_, '_> {
    /// Writes `head`, and the inputs after it with a space between.
    fn after(&self, f: &mut fmt::Formatter<'_>, head: fmt::Arguments<'_>) -> fmt::Result {
        f.write_fmt(head)?;
        if self.inputs.is_empty() {
            return Ok(());
        }
        write!(f, " {self}")
    }
}

impl fmt::Display for Inputs<'_, '_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.inputs.is_empty() {
            return Ok(());
        }
        f.write_str("(")?;
        write_joined(f, self.inputs, |f, input| {
            write!(f, "{}", value_name(self.graph, *input))
        })?;
        f.write_str(")")
    }
}

/// Writes the text of a node's op, with `inputs` in place.
fn write_op(f: &mut fmt::Formatter<'_>, layout: &FrameLayout, op: &Op, inputs: &Inputs<'_, '_>) -> fmt::Result {
    let name = op.name();
    match op {
        Op::Constant(bits) => write!(f, "Constant {}", value::describe(*bits)),
        Op::LoadSlot { slot } => inputs.after(f, format_args!("LoadSlot {}", slot_name(layout, *slot))),
        Op::StoreSlot { slot } => inputs.after(f, format_args!("StoreSlot {}", slot_name(layout, *slot))),
        Op::Phi => inputs.after(f, format_args!("Phi")),
        Op::InitializeFrame => inputs.after(f, format_args!("InitializeFrame")),
        Op::EnsureFrameInitialized => inputs.after(f, format_args!("EnsureFrameInitialized")),
        Op::PublishFrame => inputs.after(f, format_args!("PublishFrame")),
        Op::Suspend { continuation, kind } => {
            let kind = match kind {
                SuspendKind::Yield => "",
                SuspendKind::YieldIteratorResult => " iterator-result",
                SuspendKind::Await => " await",
            };
            match continuation {
                Some(pc) => inputs.after(f, format_args!("Suspend pc {pc}{kind}")),
                None => inputs.after(f, format_args!("Suspend completed{kind}")),
            }
        }
        Op::BoxCell => inputs.after(f, format_args!("BoxCell")),
        Op::LoadOuterEnvironment => inputs.after(f, format_args!("LoadOuterEnvironment")),
        Op::LoadEnvironmentBinding { index } => inputs.after(f, format_args!("LoadEnvironmentBinding {index}")),
        Op::StoreEnvironmentBinding { index } => inputs.after(f, format_args!("StoreEnvironmentBinding {index}")),
        Op::AppendEnvironmentBinding => inputs.after(f, format_args!("AppendEnvironmentBinding")),
        Op::SetLexicalEnvironment => inputs.after(f, format_args!("SetLexicalEnvironment")),
        Op::LeavePrivateEnvironment => inputs.after(f, format_args!("LeavePrivateEnvironment")),
        Op::IsCallable => inputs.after(f, format_args!("IsCallable")),
        Op::CheckInt32 => inputs.after(f, format_args!("CheckInt32")),
        Op::CheckNumber => inputs.after(f, format_args!("CheckNumber")),
        Op::CheckIdentityComparable => inputs.after(f, format_args!("CheckIdentityComparable")),
        Op::BoxInt32 => inputs.after(f, format_args!("BoxInt32")),
        Op::BoxBool => inputs.after(f, format_args!("BoxBool")),
        Op::CellAddress => inputs.after(f, format_args!("CellAddress")),
        Op::Int32Binary { op } => inputs.after(f, format_args!("Int32{op:?}")),
        Op::Int32WrappingBinary { op } => inputs.after(f, format_args!("Int32Wrapping{op:?}")),
        Op::Int32Compare { comparison } => inputs.after(f, format_args!("Int32{comparison:?}")),
        Op::CheckElements { kind } => inputs.after(f, format_args!("CheckElements {kind:?}")),
        Op::LoadElementAt { kind } | Op::StoreElementAt { kind } => inputs.after(f, format_args!("{name} {kind:?}")),
        Op::TaggedEquals { equal: true } => inputs.after(f, format_args!("TaggedEquals")),
        Op::TaggedEquals { equal: false } => inputs.after(f, format_args!("TaggedNotEquals")),
        Op::ToBoolean => inputs.after(f, format_args!("ToBoolean")),
        Op::PrimitiveToString | Op::ToObject | Op::ArrayCreate => inputs.after(f, format_args!("{name}")),
        Op::CallIntrinsic { intrinsic } => inputs.after(f, format_args!("CallIntrinsic {intrinsic:?}")),
        Op::Generic { opcode, executable, pc } => {
            let site = Site {
                executable: *executable,
                pc: *pc,
            };
            inputs.after(f, format_args!("Generic {} {site}", opcode.name()))
        }
        Op::ArgumentCount => inputs.after(f, format_args!("ArgumentCount")),
        Op::SliceArguments => inputs.after(f, format_args!("SliceArguments")),
        Op::LoadFrameField { field } => inputs.after(f, format_args!("LoadFrameField {field:?}")),
        Op::LoadArgument { .. } => inputs.after(f, format_args!("LoadArgument")),
        Op::CallForwardingArguments { executable, pc } => {
            let site = Site {
                executable: *executable,
                pc: *pc,
            };
            inputs.after(f, format_args!("CallForwardingArguments {site}"))
        }
        Op::CallDirect {
            executable,
            pc,
            inlined_frame_bytes,
            stores_operands,
            target,
        } => {
            let site = Site {
                executable: *executable,
                pc: *pc,
            };
            let frames = InlinedFrames(*inlined_frame_bytes);
            let stores = if *stores_operands { " storing operands" } else { "" };
            match target {
                Some(target) => inputs.after(
                    f,
                    format_args!("CallDirect {site}{frames}{stores} of constant target {target}"),
                ),
                None => inputs.after(f, format_args!("CallDirect {site}{frames}{stores}")),
            }
        }
        Op::CallNative {
            executable,
            pc,
            inlined_frame_bytes,
            stores_operands,
        } => {
            let site = Site {
                executable: *executable,
                pc: *pc,
            };
            let frames = InlinedFrames(*inlined_frame_bytes);
            let stores = if *stores_operands { " storing operands" } else { "" };
            inputs.after(f, format_args!("CallNative {site}{frames}{stores}"))
        }
        Op::CallNativeAccessor { pc, part, target } => {
            let direct = if target.direct_getter.is_some() { " direct" } else { "" };
            inputs.after(
                f,
                format_args!("CallNativeAccessor @{pc} {part:?} {:#x}{direct}", target.function.0),
            )
        }
        Op::AllocateObject {
            shape,
            property_count,
            reserve,
        } => {
            write!(
                f,
                "AllocateObject {:#x} with {property_count} properties, room for {reserve}",
                shape.0
            )
        }
        Op::InitializeNamed { offset } => inputs.after(f, format_args!("InitializeNamed +{offset}")),
        Op::AllocateArray { count } => write!(f, "AllocateArray with {count} elements"),
        Op::VirtualObject {
            kind: VirtualObjectKind::Object { shape },
            ..
        } => inputs.after(f, format_args!("VirtualObject {:#x}", shape.0)),
        Op::VirtualObject {
            kind: VirtualObjectKind::Closure { sample_slot },
            ..
        } => inputs.after(f, format_args!("VirtualObject closure {:#x}", sample_slot.0)),
        Op::AllocateEnvironment { shape_cache, capacity } => {
            inputs.after(f, format_args!("AllocateEnvironment {shape_cache} capacity {capacity}"))
        }
        Op::AllocateFunction {
            executable,
            shared_function_data_index,
            ..
        } => inputs.after(
            f,
            format_args!("AllocateFunction e{executable}:{shared_function_data_index}"),
        ),
        Op::InitializeElement { index } => inputs.after(f, format_args!("InitializeElement [{index}]")),
        Op::CheckObject => inputs.after(f, format_args!("CheckObject")),
        Op::CheckValue { expected, .. } => inputs.after(f, format_args!("CheckValue {expected:#x}")),
        Op::CheckClosure { shared_data } => inputs.after(f, format_args!("CheckClosure {:#x}", shared_data.0)),
        Op::LoadFunctionEnvironment { private: true } => {
            inputs.after(f, format_args!("LoadFunctionEnvironment private"))
        }
        Op::LoadFunctionEnvironment { private: false } => inputs.after(f, format_args!("LoadFunctionEnvironment")),
        Op::EmptyToUndefined => inputs.after(f, format_args!("EmptyToUndefined")),
        Op::Typeof => inputs.after(f, format_args!("Typeof")),
        Op::TypeofIs { kind, equal } => {
            let negation = if *equal { "" } else { "Not" };
            inputs.after(f, format_args!("TypeofIs{negation} {kind:?}"))
        }
        Op::CheckShape { shapes } => {
            f.write_str("CheckShape [")?;
            write_shapes(f, shapes)?;
            inputs.after(f, format_args!("]"))
        }
        Op::CheckPrototypeChainValid { validity } => write!(f, "CheckPrototypeChainValid {:#x}", validity.0),
        Op::AssumeValid => f.write_str("AssumeValid"),
        Op::CheckInterrupt => f.write_str("CheckInterrupt"),
        Op::AssumeLegacyArgumentsUnread => f.write_str("AssumeLegacyArgumentsUnread"),
        Op::HasInPrototypeChain => inputs.after(f, format_args!("HasInPrototypeChain")),
        Op::LoadGlobalBinding { environment, index } => {
            write!(f, "LoadGlobalBinding {:#x}[{index}]", environment.0)
        }
        Op::StoreGlobalBinding { environment, index } => {
            inputs.after(f, format_args!("StoreGlobalBinding {:#x}[{index}]", environment.0))
        }
        Op::LoadAccessorFunction { offset, part } => {
            inputs.after(f, format_args!("LoadAccessorFunction +{offset} {part:?}"))
        }
        Op::CheckAccessorFunction { offset, part, function } => inputs.after(
            f,
            format_args!("CheckAccessorFunction +{offset} {part:?} {:#x}", function.0),
        ),
        Op::LoadNamed { offset, holds_data } => {
            inputs.after(f, format_args!("LoadNamed +{offset}"))?;
            f.write_str(if *holds_data { " data" } else { "" })
        }
        Op::StoreNamed { offset, holds_data } => {
            inputs.after(f, format_args!("StoreNamed +{offset}"))?;
            f.write_str(if *holds_data { " data" } else { "" })
        }
        Op::AddNamed { offset, shape, .. } => inputs.after(f, format_args!("AddNamed +{offset} to {:#x}", shape.0)),
        Op::ShapeSwitch { shapes, targets } => {
            inputs.after(f, format_args!("ShapeSwitch"))?;
            f.write_str(" [")?;
            write_joined(f, shapes.iter().zip(*targets), |f, (shape, block)| {
                write_shapes(f, core::slice::from_ref(shape))?;
                write!(f, " -> {}", BlockName(*block))
            })?;
            f.write_str("]")
        }
        Op::CallSlowPath {
            opcode, executable, pc, ..
        } => {
            let site = Site {
                executable: *executable,
                pc: *pc,
            };
            inputs.after(f, format_args!("{name} {} {site}", opcode.name()))
        }
        Op::Float64Unary { op } => inputs.after(f, format_args!("Float64{}", op.name())),
        Op::CallFloat64Function { function } => inputs.after(f, format_args!("CallFloat64{}", function.name())),
        Op::Float64Binary { op } => inputs.after(f, format_args!("Float64{op:?}")),
        Op::Float64Compare { comparison } => inputs.after(f, format_args!("Float64{comparison:?}")),
        Op::Refine { condition, holds } => {
            let negation = if *holds { "" } else { "Not" };
            inputs.after(f, format_args!("Refine{negation}{}", ConditionText(*condition)))
        }
        Op::SlowPathOutput { index } => inputs.after(f, format_args!("SlowPathOutput {index}")),
        Op::UnboxInt32
        | Op::Int32Abs
        | Op::Int32Min
        | Op::Uint32Min
        | Op::Int32Max
        | Op::Float64Min
        | Op::Float64Max
        | Op::Int32ToFloat64
        | Op::BoolToInt32
        | Op::UnboxDouble
        | Op::BoxFloat64
        | Op::StringAddress
        | Op::StringLength
        | Op::LoadStringCodeUnit
        | Op::SingleCharacterString
        | Op::Float64ToInt32
        | Op::Float64ToUint8Clamped
        | Op::CheckFloat64Int32
        | Op::Uint32ShiftRight
        | Op::Uint32ToFloat64
        | Op::StringsEqual
        | Op::IntegerToString
        | Op::CheckAppendableArray
        | Op::LoadElementsLength
        | Op::LoadElementsCapacity
        | Op::AppendElement
        | Op::CallArrayPush
        | Op::CallArraySplice
        | Op::CallStoreHoleyElement
        | Op::OpenArrayIteration
        | Op::LoadArrayIteratorArray
        | Op::LoadArrayIteratorIndex
        | Op::StoreArrayIteratorIndex
        | Op::FinishArrayIterator
        | Op::LoadGeneratorCompletionType
        | Op::LoadGeneratorCompletionValue
        | Op::LoadPropertyIteratorKeyCount
        | Op::LoadPropertyIteratorKey => inputs.after(f, format_args!("{name}")),
        Op::CheckBounds
        | Op::CheckNotHole
        | Op::CheckInitialized
        | Op::CheckTypedArray
        | Op::CheckArray
        | Op::CheckPropertyAbsenceCacheable
        | Op::CheckNonNegativeInt32
        | Op::LoadTypedArrayLength
        | Op::LoadTypedArrayDataOffset
        | Op::TypedArrayDataAddress
        | Op::CheckTypedArrayStorage => inputs.after(f, format_args!("{name}")),
        Op::ConcatenateStrings { key: false } => inputs.after(f, format_args!("ConcatenateStrings")),
        Op::ConcatenateStrings { key: true } => inputs.after(f, format_args!("ConcatenateKeyStrings")),
        Op::ProbeHasProperty { own: true } => inputs.after(f, format_args!("ProbeHasOwnProperty")),
        Op::ProbeHasProperty { own: false } => inputs.after(f, format_args!("ProbeHasProperty")),
        Op::ProbeGlobalCache { cache } | Op::ProbeGlobalStore { cache } => {
            inputs.after(f, format_args!("{name} cache {cache}"))
        }
        Op::ProbePropertyCache { executable, cache, .. }
        | Op::ProbeKeyedCache { executable, cache }
        | Op::ProbeKeyedStore { executable, cache }
        | Op::ProbePropertyStore { executable, cache, .. } => {
            if *executable == 0 {
                inputs.after(f, format_args!("{name} cache {cache}"))
            } else {
                inputs.after(f, format_args!("{name} cache e{executable}:{cache}"))
            }
        }
        Op::Jump { target } => write!(f, "Jump {}", BlockName(*target)),
        Op::Branch {
            condition,
            if_true,
            if_false,
        } => {
            inputs.after(f, format_args!("Branch{}", ConditionText(*condition)))?;
            write!(f, " -> {}, {}", BlockName(*if_true), BlockName(*if_false))
        }
        Op::BranchTruthy {
            if_true,
            if_false,
            fallback,
        } => {
            inputs.after(f, format_args!("BranchTruthy"))?;
            write!(
                f,
                " -> {}, {}, fallback {}",
                BlockName(*if_true),
                BlockName(*if_false),
                BlockName(*fallback)
            )
        }
        Op::BranchOnPc { pc, if_true, if_false } => {
            inputs.after(f, format_args!("BranchOnPc @{pc}"))?;
            write!(f, " -> {}, {}", BlockName(*if_true), BlockName(*if_false))
        }
        Op::Return => inputs.after(f, format_args!("Return")),
        Op::Exit { kind } => inputs.after(f, format_args!("Exit {kind:?}")),
        Op::Unreachable => inputs.after(f, format_args!("Unreachable")),
    }
}

/// One node as text, like `v3 = Generic GetById @12 (v1) [fs2]` (see `node_text()`).
pub struct NodeText<'a, 'z> {
    graph: &'a Graph<'z>,
    layout: &'a FrameLayout,
    node_id: NodeId,
}

/// One node as text, like `v3 = Generic GetById @12 (v1) [fs2]`.
pub fn node_text<'a, 'z>(graph: &'a Graph<'z>, layout: &'a FrameLayout, node_id: NodeId) -> NodeText<'a, 'z> {
    NodeText { graph, layout, node_id }
}

impl fmt::Display for NodeText<'_, '_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let node = self.graph.node(self.node_id);
        if let Some(repr) = node.repr {
            write!(f, "v{}", self.node_id.0)?;
            if repr != Repr::Tagged {
                write!(f, ":{}", repr_name(repr))?;
            }
            f.write_str(" = ")?;
        }
        let inputs = Inputs {
            graph: self.graph,
            inputs: &node.inputs,
        };
        write_op(f, self.layout, &node.op, &inputs)?;
        if let Some(frame_state) = node.frame_state {
            write!(f, " [fs{}]", frame_state.0)?;
        }
        Ok(())
    }
}

/// `graph` as text, with frame slots named according to `layout` (see `dump()`).
pub struct Dump<'a, 'z> {
    graph: &'a Graph<'z>,
    layout: &'a FrameLayout,
}

/// Prints `graph` as text, naming frame slots according to `layout`.
pub fn dump<'a, 'z>(graph: &'a Graph<'z>, layout: &'a FrameLayout) -> Dump<'a, 'z> {
    Dump { graph, layout }
}

impl fmt::Display for Dump<'_, '_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let (graph, layout) = (self.graph, self.layout);
        for (index, block) in graph.blocks.iter().enumerate() {
            let id = BlockId::from_index(index);
            write!(f, "{}", BlockName(id))?;
            if let Some(pc) = block.bytecode_start {
                write!(f, " (pc {pc})")?;
            }
            if !block.predecessors.is_empty() {
                f.write_str(" <- ")?;
                write_joined(f, &block.predecessors, |f, block| write!(f, "{}", BlockName(*block)))?;
            }
            if block.is_loop_header {
                f.write_str(" loop")?;
            }
            if block.is_cold {
                f.write_str(" cold")?;
            }
            f.write_str(":\n")?;
            for node_id in graph.block_nodes(id) {
                writeln!(f, "  {}", node_text(graph, layout, node_id))?;
            }
        }
        for (pc, block) in &graph.osr_entries {
            writeln!(f, "osr entry at pc {pc}: b{}", block.0)?;
        }
        if !graph.frame_states.is_empty() {
            f.write_str("frame states:\n")?;
        }
        for (index, frame_state) in graph.frame_states.iter().enumerate() {
            write!(f, "  fs{index}: ")?;
            if frame_state.executable != 0 {
                write!(f, "e{} ", frame_state.executable)?;
            }
            write!(f, "pc {}", frame_state.pc)?;
            match frame_state.mode {
                ResumeMode::ResumeAt => f.write_str(" at")?,
                ResumeMode::ResumeAfter { dst } if dst == Operand::INVALID => f.write_str(" after")?,
                ResumeMode::ResumeAfter { dst } => write!(f, " after -> {}", slot_name(layout, dst))?,
            }
            f.write_str(" {")?;
            write_joined(f, &frame_state.values, |f, (slot, value)| {
                write!(f, "{}={}", slot_name(layout, *slot), value_name(graph, *value))
            })?;
            f.write_str("}")?;
            if !frame_state.in_frame.is_empty() {
                f.write_str(" frame [")?;
                write_joined(f, &frame_state.in_frame, |f, slot| {
                    write!(f, "{}", slot_name(layout, *slot))
                })?;
                f.write_str("]")?;
            }
            if let Some(parent) = frame_state.parent {
                write!(f, " in fs{}", parent.0)?;
            }
            f.write_str("\n")?;
        }
        Ok(())
    }
}
