/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the lowering of each node needs from register allocation and the frame layout, declared next to the
//! lowerings in one exhaustive match (`lowering_info()`): where its inputs and its output are, how many temps it
//! takes, what it clobbers out of line, and which parts of the JIT frame it uses. The register allocator, its verifier
//! and the frame layout read it from here, and code generation checks that each lowering takes exactly the temps it
//! declares and uses the saves area only where it says so (see `Codegen::verify_lowerings()`).

use super::Codegen;
use super::DIRECT_CALL_TEMPS;
use super::SlowPathCall;
use super::probes::PUT_BY_ID_RECORD_BYTES;
use super::slow_path_call;
use crate::asm::Gpr;
use crate::asm::PortableMacroAssembler;
use crate::code::Repr;
use crate::inline_vec::InlineVec;
use crate::ir::BinaryOp;
use crate::ir::BlockId;
use crate::ir::BranchCondition;
use crate::ir::Float64UnaryOp;
use crate::ir::Node;
use crate::ir::NodeId;
use crate::ir::Op;

/// A register of the target's calling convention, which the register allocator resolves (see
/// `RegisterSet::fixed()`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FixedRegister {
    /// Argument register `n` of calls.
    Argument(u8),
    /// The register calls return their (first) result in.
    Return,
    /// The register shifts by a register amount take the amount in, on targets that have one (see
    /// `RegisterSet::shift_amount_gpr`).
    ShiftAmount,
}

/// Where a node finds one of its inputs.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum InputConstraint {
    #[default]
    Register,
    Fixed(FixedRegister),
    /// A register, or the constant itself if the input is one, which code
    /// generation uses as an immediate.
    RegisterOrConstant,
    /// Wherever the value is: a register, its stack slot or a constant.
    Anywhere,
    /// The amount of a shift: the constant itself if the input is one, which code generation uses as an immediate,
    /// and otherwise the register the target shifts by, or any register on targets that shift by any (see
    /// `RegisterSet::resolve()`).
    ShiftAmount,
}

/// Where a node puts its value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OutputConstraint {
    None,
    Register,
    /// The register calls return their result in.
    Return,
}

/// The registers a node's code clobbers, beyond its temps, its output and the scratch register.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Clobbers {
    Nothing,
    /// Every floating point register: its out of line code calls the runtime saving only the general purpose
    /// registers that may hold values (see `slow_path_saved_registers()`). Float64 values live across it get stack
    /// slots, like values live across a call.
    Floats,
    /// Every register a call clobbers: it is a call (`NodeProperties::is_call`).
    CallerSaved,
}

/// The part of the operand record (see `Locals::record`) a node's code uses.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Record {
    None,
    Bytes(u32),
    /// The operands of the slow path of its instruction, and the stack arguments of its call, which the frame layout
    /// sizes from the bytecode (see `Codegen::lay_out_frame()`).
    SlowPathOperands,
}

/// What the lowering of a node needs (see `lowering_info()`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct LoweringInfo {
    pub inputs: InlineVec<InputConstraint, 4>,
    pub output: OutputConstraint,
    /// How many general purpose registers it computes with, besides its inputs and its output, which no input is in.
    pub temps: u8,
    pub clobbers: Clobbers,
    /// Whether it computes its output from its first input as two-address instructions do, so that the output may take
    /// the register of that input where it dies.
    pub reuses_first_input: bool,
    /// Whether its code uses the saves area (see `Locals::saves`), wherever it runs.
    pub uses_saves_area: bool,
    /// Whether, in an inlined callee, it runs in the published frames of the inlined calls it is in (see
    /// `Codegen::emit_publish_frames()`), which also uses the saves area.
    pub publishes_frames: bool,
    /// Whether it runs a slow path or a call that may leave compiled code, so that in an inlined callee, its leave
    /// writes its frame state (see `Codegen::leave_frame_of()`).
    pub may_leave_from_inlined_callee: bool,
    pub record: Record,
}

impl LoweringInfo {
    /// Takes its inputs in registers, from which it computes its value (if it has one) in a register, and nothing else.
    fn plain(node: &Node) -> Self {
        Self {
            inputs: InlineVec::from_elem(InputConstraint::Register, node.inputs.len()),
            output: if node.repr.is_some() {
                OutputConstraint::Register
            } else {
                OutputConstraint::None
            },
            temps: 0,
            clobbers: if node.op.properties().is_call {
                Clobbers::CallerSaved
            } else {
                Clobbers::Nothing
            },
            reuses_first_input: false,
            uses_saves_area: false,
            publishes_frames: false,
            may_leave_from_inlined_callee: false,
            record: Record::None,
        }
    }

    fn with_temps(self, temps: u8) -> Self {
        Self { temps, ..self }
    }

    /// Calls the runtime out of line saving only the general purpose registers, in the saves area.
    fn saving_registers(self) -> Self {
        debug_assert_eq!(self.clobbers, Clobbers::Nothing, "calls clobber more");
        Self {
            clobbers: Clobbers::Floats,
            uses_saves_area: true,
            ..self
        }
    }

    /// Takes input `index` as an immediate if it is a constant.
    fn with_immediate(mut self, index: usize) -> Self {
        self.inputs[index] = InputConstraint::RegisterOrConstant;
        self
    }

    /// Shifts by input `index`, which it takes as an immediate if it is a constant.
    fn with_shift_amount(mut self, index: usize) -> Self {
        self.inputs[index] = InputConstraint::ShiftAmount;
        self
    }

    /// Calls a runtime function with the VM in the first argument register and its inputs in the next ones, returning
    /// its value.
    fn runtime_call(node: &Node) -> Self {
        Self {
            inputs: (0..node.inputs.len())
                .map(|index| InputConstraint::Fixed(FixedRegister::Argument(index as u8 + 1)))
                .collect(),
            output: OutputConstraint::Return,
            ..Self::plain(node)
        }
    }
}

/// What the lowering of `node` needs. Nodes without code (constants, phis, virtual objects, refinements and legacy
/// arguments guards, which are gone before register allocation) need nothing but what every node does.
pub fn lowering_info(node: &Node) -> LoweringInfo {
    let plain = LoweringInfo::plain(node);
    match &node.op {
        Op::Constant(_) | Op::Phi | Op::VirtualObject { .. } | Op::Refine { .. } | Op::AssumeLegacyArgumentsUnread => {
            plain
        }
        Op::LoadSlot { .. } | Op::StoreSlot { .. } | Op::PublishFrame | Op::Suspend { .. } => plain,
        // A value and a cursor for filling the registers and locals.
        Op::InitializeFrame | Op::EnsureFrameInitialized => plain.with_temps(2),
        Op::LoadOuterEnvironment | Op::LoadEnvironmentBinding { .. } => plain,
        // The environment's binding values.
        Op::StoreEnvironmentBinding { .. } => plain.with_temps(1),
        // The index of the appended binding.
        Op::AppendEnvironmentBinding => plain.with_temps(1),
        Op::BoxCell | Op::SetLexicalEnvironment | Op::LeavePrivateEnvironment | Op::IsCallable => plain,
        Op::ToBoolean => LoweringInfo {
            inputs: InlineVec::from_elem(InputConstraint::Fixed(FixedRegister::Argument(0)), 1),
            output: OutputConstraint::Return,
            ..plain
        },
        // NB: The VM is the first argument.
        Op::PrimitiveToString | Op::CallIntrinsic { .. } | Op::ToObject | Op::ArrayCreate => {
            LoweringInfo::runtime_call(node)
        }
        // NB: The inserted values go on the native stack.
        Op::CallArraySplice => {
            let mut info = LoweringInfo::runtime_call(node);
            for input in info.inputs.iter_mut().skip(3) {
                *input = InputConstraint::RegisterOrConstant;
            }
            info
        }
        Op::Generic { opcode, .. } => LoweringInfo {
            output: if node.repr.is_some() {
                OutputConstraint::Return
            } else {
                OutputConstraint::None
            },
            may_leave_from_inlined_callee: true,
            record: slow_path_record(*opcode, false),
            ..plain
        },
        // NB: The call stores its operands to the frame first.
        Op::CallDirect {
            stores_operands: true,
            inlined_frame_bytes,
            ..
        }
        | Op::CallNative {
            stores_operands: true,
            inlined_frame_bytes,
            ..
        } => LoweringInfo {
            inputs: InlineVec::from_elem(InputConstraint::Anywhere, node.inputs.len()),
            output: OutputConstraint::Return,
            publishes_frames: *inlined_frame_bytes == 0,
            may_leave_from_inlined_callee: true,
            ..plain
        },
        // Direct and native calls with inputs build the callee's frame in
        // temps.
        Op::CallDirect {
            inlined_frame_bytes, ..
        }
        | Op::CallNative {
            inlined_frame_bytes, ..
        } => {
            let info = LoweringInfo {
                publishes_frames: *inlined_frame_bytes == 0,
                may_leave_from_inlined_callee: true,
                ..plain
            };
            if node.inputs.is_empty() {
                info
            } else {
                // NB: Calls in inlined callees return their result.
                LoweringInfo {
                    output: if node.repr.is_some() {
                        OutputConstraint::Return
                    } else {
                        OutputConstraint::None
                    },
                    ..info.with_temps(DIRECT_CALL_TEMPS)
                }
            }
        }
        // Native accessor calls build the callee's frame in temps, and return
        // the result.
        Op::CallNativeAccessor { .. } => LoweringInfo {
            output: OutputConstraint::Return,
            ..plain.with_temps(DIRECT_CALL_TEMPS)
        },
        Op::ArgumentCount | Op::LoadFrameField { .. } | Op::LoadArgument { .. } => plain,
        Op::CallForwardingArguments { .. } => LoweringInfo {
            may_leave_from_inlined_callee: true,
            ..plain
        },
        Op::CheckInt32 | Op::CheckNumber | Op::BoxInt32 | Op::BoxBool => plain,
        // NB: Comparisons and int32 arithmetic take a constant right-hand side as an immediate.
        Op::Int32Binary { op: BinaryOp::Mul } => LoweringInfo {
            reuses_first_input: true,
            ..plain
        },
        // The quotient of a remainder.
        Op::Int32Binary { op: BinaryOp::Mod } => LoweringInfo {
            reuses_first_input: true,
            ..plain.with_immediate(1).with_temps(1)
        },
        Op::Int32Binary {
            op: BinaryOp::LeftShift | BinaryOp::RightShift | BinaryOp::UnsignedRightShift,
        }
        | Op::Int32WrappingBinary {
            op: BinaryOp::UnsignedRightShift,
        } => LoweringInfo {
            reuses_first_input: true,
            ..plain.with_shift_amount(1)
        },
        Op::Int32Binary { .. } | Op::Int32WrappingBinary { .. } => LoweringInfo {
            reuses_first_input: true,
            ..plain.with_immediate(1)
        },
        Op::Int32Compare { .. } | Op::TaggedEquals { .. } => plain.with_immediate(1),
        // The object's address.
        Op::CheckElements { .. } | Op::CheckTypedArray | Op::CheckArray => plain.with_temps(1),
        Op::CheckNonNegativeInt32 | Op::LoadTypedArrayLength | Op::LoadTypedArrayDataOffset => plain,
        Op::TypedArrayDataAddress => LoweringInfo {
            reuses_first_input: true,
            ..plain
        },
        Op::CheckTypedArrayStorage => plain,
        Op::CheckBounds => plain.with_immediate(0).with_immediate(1),
        // The elements or the element address.
        Op::LoadElementAt { .. } => plain.with_immediate(1).with_temps(1),
        // The elements or the element address. Stores of integer and tagged constants store immediates.
        Op::StoreElementAt { kind } => {
            let info = plain.with_immediate(1).with_temps(1);
            if kind.stored_repr() == Repr::Float64 {
                info
            } else {
                info.with_immediate(2)
            }
        }
        Op::CheckNotHole | Op::CheckInitialized | Op::CellAddress => plain,
        // The heap and a value.
        Op::AllocateObject { .. } => plain.with_temps(2).saving_registers(),
        // The object's property storage.
        Op::InitializeNamed { .. } => plain.with_temps(1),
        // The element storage and a value.
        Op::AllocateArray { .. } => plain.with_temps(2).saving_registers(),
        // The array's elements.
        Op::InitializeElement { .. } => plain.with_temps(1),
        // The function and a value.
        Op::AllocateFunction { .. } => plain.with_temps(2).saving_registers(),
        // The environment and a value.
        Op::AllocateEnvironment { .. } => plain.with_temps(2).saving_registers(),
        Op::SliceArguments => LoweringInfo {
            inputs: InlineVec::from_elem(InputConstraint::Fixed(FixedRegister::Argument(2)), 1),
            output: OutputConstraint::Return,
            ..plain
        },
        // The tag of an operand, with a bit that tells two of the tags apart set.
        Op::CheckIdentityComparable => plain.with_temps(1),
        Op::CheckObject => plain,
        // The function object.
        Op::CheckClosure { .. } => plain.with_temps(1),
        Op::LoadFunctionEnvironment { .. } | Op::CheckValue { .. } => plain,
        Op::EmptyToUndefined | Op::Typeof | Op::TypeofIs { .. } => plain,
        Op::CheckShape { .. } | Op::CheckPrototypeChainValid { .. } => plain,
        // The object whose prototype comes next.
        Op::HasInPrototypeChain => plain.with_temps(1),
        Op::AssumeValid | Op::CheckInterrupt | Op::LoadGlobalBinding { .. } => plain,
        // The environment's binding values.
        Op::StoreGlobalBinding { .. } => plain.with_temps(1),
        // The object's property storage, then the accessor.
        Op::CheckAccessorFunction { .. } => plain.with_temps(1),
        Op::LoadAccessorFunction { .. } | Op::LoadNamed { .. } => plain,
        // The object's property storage.
        Op::AddNamed { .. } | Op::StoreNamed { .. } => plain.with_temps(1),
        Op::Jump { .. } => plain,
        Op::Branch { condition, .. } => branch_lowering_info(*condition, plain),
        Op::BranchTruthy { .. } | Op::BranchOnPc { .. } | Op::ShapeSwitch { .. } => plain,
        Op::Return => LoweringInfo {
            inputs: InlineVec::from_elem(InputConstraint::Fixed(FixedRegister::Return), 1),
            ..plain
        },
        Op::Exit { .. } | Op::Unreachable => plain,
        // NB: Slow path calls read their operands from where they are.
        Op::CallSlowPath {
            saves_registers,
            opcode,
            ..
        } => {
            let info = LoweringInfo {
                inputs: InlineVec::from_elem(InputConstraint::Anywhere, node.inputs.len()),
                // NB: A call's value is moved into the return register once its
                //     slow path continued, where a conditional jump's control
                //     word is.
                output: if !saves_registers && node.repr.is_some() {
                    OutputConstraint::Return
                } else {
                    plain.output
                },
                uses_saves_area: true,
                publishes_frames: true,
                may_leave_from_inlined_callee: true,
                record: slow_path_record(*opcode, true),
                ..plain
            };
            if *saves_registers {
                LoweringInfo {
                    clobbers: Clobbers::Floats,
                    ..info
                }
            } else {
                info
            }
        }
        Op::UnboxInt32 | Op::Int32Abs | Op::Int32Min | Op::Int32Max | Op::Uint32Min | Op::Int32ToFloat64 => plain,
        Op::BoolToInt32 => LoweringInfo {
            reuses_first_input: true,
            ..plain
        },
        // NB: Rounding reads its input after writing its output.
        Op::UnboxDouble
        | Op::BoxFloat64
        | Op::Float64Unary {
            op: Float64UnaryOp::Round,
        } => plain,
        // OPTIMIZATION: The SSE instructions of x86 write their result over their first operand, which then needs no
        //     copy where it dies.
        Op::Float64Unary { .. } | Op::Float64Binary { .. } => LoweringInfo {
            reuses_first_input: true,
            ..plain
        },
        Op::Float64Min | Op::Float64Max | Op::CallFloat64Function { .. } | Op::Float64Compare { .. } => plain,
        Op::Float64ToInt32 | Op::Float64ToUint8Clamped | Op::CheckFloat64Int32 => plain,
        Op::Uint32ShiftRight => plain.with_shift_amount(1),
        Op::Uint32ToFloat64 | Op::StringAddress | Op::StringLength | Op::LoadStringCodeUnit => plain,
        Op::SingleCharacterString => plain,
        // The first string's address, then its length or its storage word.
        Op::StringsEqual => plain.with_temps(1),
        // The new rope string, its length and a temp.
        Op::ConcatenateStrings => plain.with_temps(3).saving_registers(),
        Op::IntegerToString => plain,
        // The array, then its prototypes.
        Op::CheckAppendableArray => plain.with_temps(2),
        Op::LoadElementsLength => plain,
        // The object's elements.
        Op::LoadElementsCapacity | Op::AppendElement => plain.with_temps(1),
        Op::CallArrayPush | Op::CallStoreHoleyElement | Op::OpenArrayIteration => plain.saving_registers(),
        Op::LoadArrayIteratorArray | Op::LoadArrayIteratorIndex => plain,
        Op::StoreArrayIteratorIndex | Op::FinishArrayIterator => plain,
        Op::LoadGeneratorCompletionType | Op::LoadGeneratorCompletionValue => plain,
        Op::LoadPropertyIteratorKeyCount | Op::LoadPropertyIteratorKey => plain,
        // NB: The output comes from where the slow path call left it.
        Op::SlowPathOutput { .. } => LoweringInfo {
            inputs: InlineVec::from_elem(InputConstraint::Anywhere, 1),
            ..plain
        },
        // The object, its shape and the cache entry.
        Op::ProbePropertyCache { .. } => plain.with_temps(3).saving_registers(),
        // The object, its shape and the cache entry.
        Op::ProbeKeyedCache { .. } => plain.with_temps(3),
        // The object, its shape, the cache entry and the named property storage of property additions.
        // NB: It calls no runtime function: the subroutine that grows named property storage keeps every register
        //     but the scratch register, saving the ones it uses in the saves area.
        Op::ProbeKeyedStore { .. } => LoweringInfo {
            uses_saves_area: true,
            ..plain.with_temps(4)
        },
        // The object, its shape, the cache entry and the named property storage of property additions.
        Op::ProbePropertyStore { .. } => LoweringInfo {
            record: Record::Bytes(PUT_BY_ID_RECORD_BYTES),
            ..plain.with_temps(4).saving_registers()
        },
        // The cache, the global declarative environment, the global object
        // and a temp.
        Op::ProbeGlobalCache { .. } | Op::ProbeGlobalStore { .. } => plain.with_temps(4),
        // The object or its elements, and the keyed cache's registers.
        Op::ProbeHasProperty { .. } => plain.with_temps(3).saving_registers(),
    }
}

/// What a branch on `condition` needs, beyond `plain`.
fn branch_lowering_info(condition: BranchCondition, plain: LoweringInfo) -> LoweringInfo {
    match condition {
        BranchCondition::Bool
        | BranchCondition::Nullish
        | BranchCondition::Undefined
        | BranchCondition::Object
        | BranchCondition::NonNegativeInt32
        | BranchCondition::IndexInBounds
        | BranchCondition::MagicalLength
        | BranchCondition::Extensible
        | BranchCondition::ArrayIterator
        | BranchCondition::GeneratorObject
        | BranchCondition::HoldsValidWatchpointSet
        | BranchCondition::Shape(_)
        | BranchCondition::Float64(_)
        | BranchCondition::Int32Value
        | BranchCondition::Int32OrBoolean
        | BranchCondition::Double
        | BranchCondition::String
        | BranchCondition::ResolvedString => plain,
        // NB: Comparisons take a constant right-hand side as an immediate.
        BranchCondition::Int32(_) | BranchCondition::TaggedEquals { .. } => plain.with_immediate(1),
        // The address of the object.
        BranchCondition::ElementsKind(_) => plain.with_temps(1),
        // The array's prototypes.
        BranchCondition::HoleFillableArray => plain.with_temps(1),
        // The object's prototypes.
        BranchCondition::PlainArrayPrototypeChain => plain.with_temps(1),
        // The object's shape.
        BranchCondition::ArrayWithRealmArrayPrototypeAndNoNamedProperties => plain.with_temps(1),
        // The function object.
        BranchCondition::Builtin(_) => plain.with_temps(1),
        // The binding's index, and a temp.
        BranchCondition::BindingMutable { .. } => plain.with_temps(2),
        // The environment's shape and the binding's index.
        BranchCondition::NextBindingOfShape { .. } => plain.with_temps(2),
        // The object, the property iterator cache, and a temp.
        BranchCondition::PropertyIteratorCacheValid => plain.with_temps(3),
    }
}

/// The part of the operand record the slow path of `opcode` uses, called by a `CallSlowPath` node (`on_values`) or a
/// `Generic` one.
fn slow_path_record(opcode: crate::bytecode::OpCode, on_values: bool) -> Record {
    match slow_path_call(opcode) {
        Some(SlowPathCall::Record) => Record::SlowPathOperands,
        // NB: Binary slow paths called on values write their result into the record.
        Some(SlowPathCall::BinaryValues) if on_values => Record::Bytes(8),
        Some(SlowPathCall::BinaryValues | SlowPathCall::JumpValues | SlowPathCall::JitCall) | None => Record::None,
    }
}

impl<M: PortableMacroAssembler> Codegen<'_, M> {
    /// Temp `index` of `node`, which its lowering declares (see `LoweringInfo::temps`). Lowerings take every temp
    /// they declare through this, even on paths that do not use it, so that the declarations can be checked.
    pub(super) fn temp(&self, node: NodeId, index: usize) -> Gpr {
        let requested = &self.requested_temps[node.index()];
        requested.set(requested.get().max(index as u8 + 1));
        Gpr(self.allocation.node(node).temps[index])
    }

    /// Notes that the code being emitted uses the saves area.
    pub(super) fn note_saves_area_use(&self) {
        if self.verify
            && let Some(node) = self.emitting
            && !self.uses_saves_area(node)
        {
            self.undeclared_saves_area_uses.borrow_mut().push(node);
        }
    }

    /// Checks that the lowering of every node took exactly the temps it declares, and that only nodes that declare
    /// it used the saves area, whose size depends on that.
    pub(super) fn verify_lowerings(&self) -> Result<(), String> {
        let mut errors = Vec::new();
        for block in 0..self.graph.blocks.len() {
            for node in self.graph.block_nodes(BlockId::from_index(block)) {
                let declared = lowering_info(self.graph.node(node)).temps;
                let requested = self.requested_temps[node.index()].get();
                if requested != declared {
                    errors.push(format!(
                        "v{} ({}) takes {requested} temps, but declares {declared}",
                        node.0,
                        self.graph.node(node).op.name()
                    ));
                }
            }
        }
        let mut undeclared = self.undeclared_saves_area_uses.borrow().clone();
        undeclared.dedup();
        for node in undeclared {
            errors.push(format!(
                "v{} ({}) uses the saves area, but does not declare it",
                node.0,
                self.graph.node(node).op.name()
            ));
        }
        if errors.is_empty() {
            Ok(())
        } else {
            Err(errors.join("\n"))
        }
    }
}
