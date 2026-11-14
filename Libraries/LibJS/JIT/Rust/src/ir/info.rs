/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the compiler knows about each op, apart from how code generation lowers it (see
//! `codegen::lowering_info`): its effects, how it exits, which input it refines, whether it ends a block and which
//! inputs it takes. `Op::info()` decides all of them for every op in one exhaustive match, so that a new op cannot
//! be added without deciding each, and the passes and the verifier read them from there.

use super::BranchCondition;
use super::ElementsKind;
use super::Locations;
use super::Op;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::snapshot::HelperResult;
use crate::snapshot::Intrinsic;

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct NodeProperties {
    /// May need to exit to the interpreter after it returns, resuming after
    /// the current bytecode.
    pub can_lazy_exit: bool,
    /// Calls out of JIT code; every caller-saved register is clobbered.
    pub is_call: bool,
    /// The locations it may read and write, beyond its inputs and outputs.
    pub reads: Locations,
    pub writes: Locations,
    /// May run other code (which may write any location), or invalidate
    /// the compiled code (see `Dependency`).
    pub runs_code: bool,
    pub allocates: bool,
}

impl NodeProperties {
    pub(super) const PURE: Self = Self {
        can_lazy_exit: false,
        is_call: false,
        reads: Locations::NONE,
        writes: Locations::NONE,
        runs_code: false,
        allocates: false,
    };

    /// Reads `reads` and has no other effect.
    const fn reading(reads: Locations) -> Self {
        Self { reads, ..Self::PURE }
    }

    /// Writes `writes` and has no other effect.
    const fn writing(writes: Locations) -> Self {
        Self { writes, ..Self::PURE }
    }

    /// Calls out of JIT code and has no other effect: it only computes its value.
    const CALL: Self = Self {
        is_call: true,
        ..Self::PURE
    };

    /// Calls out of JIT code, which may allocate.
    const ALLOCATING_CALL: Self = Self {
        is_call: true,
        allocates: true,
        ..Self::PURE
    };

    /// Stores or adds a named property through a cache.
    const STORE_PROBE: Self = Self {
        reads: Locations::NAMED,
        writes: Locations::NAMED,
        allocates: true,
        ..Self::PURE
    };

    /// Runs code of the program: a call that may do anything and exit after it returns.
    const RUNS_ANYTHING: Self = Self {
        can_lazy_exit: true,
        is_call: true,
        reads: Locations::ALL,
        writes: Locations::ALL,
        runs_code: true,
        allocates: true,
    };
}

/// The inputs an op takes, which the verifier checks every node against.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Inputs {
    /// Exactly these, in this order.
    Exactly(&'static [Repr]),
    /// These, then any number of `rest`.
    AtLeast(&'static [Repr], Repr),
    /// The receiver and the arguments of the intrinsic's `HelperCall`.
    HelperCall(Intrinsic),
    /// Not checked by their representations: phis take one input per predecessor, of their own representation (which
    /// the verifier checks with the predecessors), constants take none, and `StoreSlot` stores any value.
    Unchecked,
}

impl Inputs {
    const NONE: Self = Self::Exactly(&[]);
    const TAGGED: Self = Self::Exactly(&[Repr::Tagged]);
    const TAGGED_2: Self = Self::Exactly(&[Repr::Tagged, Repr::Tagged]);
    const TAGGED_3: Self = Self::Exactly(&[Repr::Tagged, Repr::Tagged, Repr::Tagged]);
    const INT32: Self = Self::Exactly(&[Repr::Int32]);
    const INT32_2: Self = Self::Exactly(&[Repr::Int32, Repr::Int32]);
    const FLOAT64: Self = Self::Exactly(&[Repr::Float64]);
    const FLOAT64_2: Self = Self::Exactly(&[Repr::Float64, Repr::Float64]);
    const BOOL: Self = Self::Exactly(&[Repr::Bool]);
    const POINTER: Self = Self::Exactly(&[Repr::Pointer]);
    const POINTER_2: Self = Self::Exactly(&[Repr::Pointer, Repr::Pointer]);
    const POINTER_INT32: Self = Self::Exactly(&[Repr::Pointer, Repr::Int32]);
    /// An object and its `CellAddress`.
    const OBJECT: Self = Self::Exactly(&[Repr::Tagged, Repr::Pointer]);
    /// An object, a value and the object's `CellAddress`.
    const OBJECT_VALUE: Self = Self::Exactly(&[Repr::Tagged, Repr::Tagged, Repr::Pointer]);
    /// Any number of tagged values.
    const TAGGED_ANY: Self = Self::AtLeast(&[], Repr::Tagged);

    /// The representations of `count` inputs, if they are of this signature, or `None` if `count` is wrong. A
    /// `HelperCall` of an intrinsic without one, and `Unchecked`, accept anything.
    pub fn reprs(self, count: usize) -> Result<Option<Vec<Repr>>, usize> {
        match self {
            Inputs::Exactly(reprs) if reprs.len() == count => Ok(Some(reprs.to_vec())),
            Inputs::Exactly(reprs) => Err(reprs.len()),
            Inputs::AtLeast(first, rest) if count >= first.len() => Ok(Some(
                first
                    .iter()
                    .copied()
                    .chain(std::iter::repeat_n(rest, count - first.len()))
                    .collect(),
            )),
            Inputs::AtLeast(first, _) => Err(first.len()),
            Inputs::HelperCall(intrinsic) => {
                let Some(call) = intrinsic.helper_call() else {
                    return Ok(None);
                };
                let reprs = std::iter::once(call.receiver)
                    .chain(call.arguments.iter().copied())
                    .map(|operand| operand.repr())
                    .collect::<Vec<_>>();
                if reprs.len() == count {
                    Ok(Some(reprs))
                } else {
                    Err(reprs.len())
                }
            }
            Inputs::Unchecked => Ok(None),
        }
    }
}

/// What the compiler knows about an op (see `Op::info()`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct OpInfo {
    pub properties: NodeProperties,
    /// Why the node exits to the interpreter, if it can exit eagerly: before its side effects, resuming at the
    /// current bytecode.
    pub exit_kind: Option<ExitKind>,
    /// For checks and refinements, whose value is an input known to pass them: which input that is (see
    /// "Refinements" in `Op`).
    pub refined_input: Option<usize>,
    /// Whether it ends its block (see `Op::successors()`).
    pub is_control: bool,
    /// Whether its value is never the empty value (see `Graph::is_known_non_empty()`, which decides constants, slots
    /// and phis itself). False is always safe.
    pub never_empty: bool,
    pub inputs: Inputs,
}

impl OpInfo {
    /// An op without effects or exits, taking `inputs`.
    const fn pure(inputs: Inputs) -> Self {
        Self {
            properties: NodeProperties::PURE,
            exit_kind: None,
            refined_input: None,
            is_control: false,
            never_empty: false,
            inputs,
        }
    }

    /// A check of input 0 that exits with `kind` where it fails, and has that input, refined, as its value.
    const fn check(kind: ExitKind, inputs: Inputs) -> Self {
        Self {
            exit_kind: Some(kind),
            refined_input: Some(0),
            ..Self::pure(inputs)
        }
    }

    /// A control node taking `inputs`.
    const fn control(inputs: Inputs) -> Self {
        Self {
            is_control: true,
            ..Self::pure(inputs)
        }
    }

    const fn with_properties(self, properties: NodeProperties) -> Self {
        Self { properties, ..self }
    }

    const fn exiting(self, kind: ExitKind) -> Self {
        Self {
            exit_kind: Some(kind),
            ..self
        }
    }

    const fn never_empty(self) -> Self {
        Self {
            never_empty: true,
            ..self
        }
    }
}

impl Op {
    /// What the compiler knows about the op.
    pub fn info(&self) -> OpInfo {
        use Inputs as I;
        use NodeProperties as P;
        match self {
            Op::Constant(_) | Op::Phi => OpInfo::pure(I::Unchecked),
            Op::LoadSlot { .. } => OpInfo::pure(I::NONE).with_properties(P::reading(Locations::FRAME)),
            Op::StoreSlot { .. } => OpInfo::pure(I::Unchecked).with_properties(P::writing(Locations::FRAME)),
            Op::InitializeFrame | Op::EnsureFrameInitialized | Op::PublishFrame => {
                OpInfo::pure(I::NONE).with_properties(P::writing(Locations::FRAME))
            }
            // NB: It hands the frame's slots to the code that resumes it.
            Op::Suspend { .. } => OpInfo::pure(I::NONE).with_properties(P {
                reads: Locations::FRAME,
                writes: Locations::FRAME,
                ..P::PURE
            }),
            Op::LoadOuterEnvironment | Op::BoxCell => OpInfo::pure(I::POINTER),
            Op::LoadEnvironmentBinding { .. } => OpInfo::pure(I::POINTER)
                .with_properties(P::reading(Locations::BINDINGS.union(Locations::GLOBAL_BINDINGS))),
            Op::StoreEnvironmentBinding { .. } => OpInfo::pure(I::Exactly(&[Repr::Pointer, Repr::Tagged]))
                .with_properties(P::writing(Locations::BINDINGS.union(Locations::GLOBAL_BINDINGS))),
            Op::AppendEnvironmentBinding => OpInfo::pure(I::POINTER)
                .with_properties(P::writing(Locations::BINDINGS.union(Locations::GLOBAL_BINDINGS))),
            Op::SetLexicalEnvironment => OpInfo::pure(I::TAGGED).with_properties(P {
                reads: Locations::FRAME,
                writes: Locations::FRAME,
                ..P::PURE
            }),
            Op::LeavePrivateEnvironment => OpInfo::pure(I::NONE).with_properties(P {
                reads: Locations::FRAME,
                writes: Locations::FRAME,
                ..P::PURE
            }),
            Op::IsCallable => OpInfo::pure(I::TAGGED).never_empty(),
            Op::ToBoolean => OpInfo::pure(I::TAGGED).with_properties(P::CALL),
            Op::PrimitiveToString => OpInfo::pure(I::TAGGED).with_properties(P::ALLOCATING_CALL),
            Op::CallIntrinsic { intrinsic } => {
                let call = intrinsic
                    .helper_call()
                    .expect("intrinsics called through a helper have one");
                // NB: Helpers that answer with a number or a boolean make no
                //     cells.
                OpInfo::pure(I::HelperCall(*intrinsic)).with_properties(P {
                    is_call: true,
                    allocates: matches!(call.result, HelperResult::String | HelperResult::Value),
                    reads: call.reads,
                    writes: call.writes,
                    ..P::PURE
                })
            }
            Op::CallArraySplice => OpInfo::pure(I::AtLeast(&[Repr::Tagged, Repr::Int32, Repr::Int32], Repr::Tagged))
                .with_properties(P {
                    is_call: true,
                    allocates: true,
                    reads: Locations::NAMED.union(Locations::ELEMENTS_AND_COUNTS),
                    writes: Locations::ELEMENTS_AND_COUNTS,
                    ..P::PURE
                }),
            Op::ToObject => OpInfo::pure(I::TAGGED_2).with_properties(P::ALLOCATING_CALL),
            Op::ArrayCreate => {
                OpInfo::pure(I::Exactly(&[Repr::Int32, Repr::Tagged])).with_properties(P::ALLOCATING_CALL)
            }
            Op::Generic { .. } => OpInfo::pure(I::NONE).with_properties(P::RUNS_ANYTHING),
            // NB: Calls return values, never the empty value.
            Op::CallDirect { .. } | Op::CallNative { .. } => OpInfo::pure(I::TAGGED_ANY)
                .with_properties(P::RUNS_ANYTHING)
                .never_empty(),
            Op::CallNativeAccessor { part, .. } => {
                let inputs = match part {
                    super::AccessorPart::Getter => I::TAGGED,
                    super::AccessorPart::Setter => I::TAGGED_2,
                };
                OpInfo::pure(inputs).with_properties(P::RUNS_ANYTHING)
            }
            Op::ArgumentCount => OpInfo::pure(I::NONE)
                .with_properties(P::reading(Locations::FRAME))
                .never_empty(),
            Op::LoadFrameField { .. } => OpInfo::pure(I::NONE).with_properties(P::reading(Locations::FRAME)),
            Op::LoadArgument { .. } => OpInfo::pure(I::TAGGED)
                .with_properties(P::reading(Locations::FRAME))
                .exiting(ExitKind::ArgumentsIndex)
                .never_empty(),
            Op::CallForwardingArguments { .. } => OpInfo::pure(I::NONE).with_properties(P::RUNS_ANYTHING),
            Op::CheckInt32 => OpInfo::pure(I::TAGGED).exiting(ExitKind::NotInt32),
            Op::CheckNumber => OpInfo::pure(I::TAGGED).exiting(ExitKind::BadType),
            Op::BoxInt32 => OpInfo::pure(I::INT32).never_empty(),
            Op::BoxBool => OpInfo::pure(I::BOOL).never_empty(),
            Op::Int32Binary { op } => {
                let info = OpInfo::pure(I::INT32_2);
                if op.int32_can_overflow() {
                    info.exiting(ExitKind::Overflow)
                } else {
                    info
                }
            }
            Op::Int32WrappingBinary { .. } | Op::Int32Compare { .. } => OpInfo::pure(I::INT32_2),
            Op::TaggedEquals { .. } => OpInfo::pure(I::TAGGED_2),
            // NB: The elements kind of a typed array never changes.
            Op::CheckElements {
                kind: ElementsKind::TypedArray(_),
            } => OpInfo::check(ExitKind::BadElements, I::TAGGED),
            Op::CheckElements { .. } => {
                OpInfo::check(ExitKind::BadElements, I::TAGGED).with_properties(P::reading(Locations::ELEMENT_COUNTS))
            }
            Op::CheckTypedArray => OpInfo::check(ExitKind::BadElements, I::TAGGED),
            Op::CheckArray => OpInfo::check(ExitKind::BadType, I::TAGGED),
            Op::CheckNonNegativeInt32 => OpInfo::check(ExitKind::Overflow, I::INT32),
            Op::LoadTypedArrayLength | Op::LoadTypedArrayDataOffset => {
                OpInfo::pure(I::POINTER).with_properties(P::reading(Locations::ELEMENT_COUNTS))
            }
            Op::TypedArrayDataAddress => OpInfo::pure(I::POINTER),
            Op::CheckTypedArrayStorage => OpInfo::check(ExitKind::OutOfBounds, I::POINTER),
            Op::CheckBounds => OpInfo::check(ExitKind::OutOfBounds, I::INT32_2),
            Op::LoadElementAt { .. } => OpInfo::pure(I::POINTER_INT32).with_properties(P::reading(Locations::ELEMENTS)),
            // NB: Stores within the bounds change no element count.
            Op::StoreElementAt { kind } => {
                let inputs = match kind.stored_repr() {
                    Repr::Float64 => I::Exactly(&[Repr::Pointer, Repr::Int32, Repr::Float64]),
                    Repr::Int32 => I::Exactly(&[Repr::Pointer, Repr::Int32, Repr::Int32]),
                    _ => I::Exactly(&[Repr::Pointer, Repr::Int32, Repr::Tagged]),
                };
                OpInfo::pure(inputs).with_properties(P::writing(Locations::ELEMENTS))
            }
            Op::CheckNotHole => OpInfo::check(ExitKind::OutOfBounds, I::TAGGED),
            Op::CheckInitialized => OpInfo::check(ExitKind::Uninitialized, I::TAGGED),
            Op::CellAddress => OpInfo::pure(I::TAGGED),
            Op::AllocateObject { .. } => OpInfo::pure(I::NONE)
                .with_properties(P {
                    allocates: true,
                    ..P::PURE
                })
                .never_empty(),
            Op::InitializeNamed { .. } => OpInfo::pure(I::TAGGED_2).with_properties(P::writing(Locations::NAMED_SLOTS)),
            // NB: Virtual objects are in no block, so the verifier sees no inputs of theirs.
            Op::VirtualObject { .. } => OpInfo::pure(I::Unchecked),
            Op::AllocateArray { .. } => OpInfo::pure(I::NONE)
                .with_properties(P {
                    allocates: true,
                    ..P::PURE
                })
                .never_empty(),
            Op::InitializeElement { .. } => OpInfo::pure(I::TAGGED_2).with_properties(P::writing(Locations::ELEMENTS)),
            Op::AllocateFunction { .. } => OpInfo::pure(I::POINTER_2)
                .with_properties(P {
                    allocates: true,
                    ..P::PURE
                })
                .never_empty(),
            Op::AllocateEnvironment { .. } => OpInfo::pure(I::TAGGED)
                .with_properties(P {
                    allocates: true,
                    ..P::PURE
                })
                .never_empty(),
            Op::SliceArguments => OpInfo::pure(I::TAGGED)
                .with_properties(P {
                    is_call: true,
                    reads: Locations::FRAME,
                    allocates: true,
                    ..P::PURE
                })
                .exiting(ExitKind::ArgumentsIndex)
                .never_empty(),
            Op::CheckIdentityComparable => OpInfo::check(ExitKind::BadType, I::TAGGED_2),
            Op::CheckObject => OpInfo::check(ExitKind::NotObject, I::TAGGED),
            Op::CheckClosure { .. } => OpInfo::check(ExitKind::BadCallTarget, I::TAGGED),
            Op::LoadFunctionEnvironment { .. } => OpInfo::pure(I::POINTER),
            Op::CheckValue { kind, .. } => OpInfo::check(*kind, I::TAGGED),
            Op::EmptyToUndefined | Op::Typeof => OpInfo::pure(I::TAGGED).never_empty(),
            Op::TypeofIs { .. } => OpInfo::pure(I::TAGGED),
            Op::CheckShape { .. } => {
                OpInfo::check(ExitKind::BadShape, I::OBJECT).with_properties(P::reading(Locations::SHAPES))
            }
            Op::CheckPrototypeChainValid { .. } => OpInfo::pure(I::NONE)
                .with_properties(P::reading(Locations::SHAPES))
                .exiting(ExitKind::BadShape),
            Op::HasInPrototypeChain => OpInfo::pure(I::TAGGED_2)
                .with_properties(P::reading(Locations::SHAPES))
                .exiting(ExitKind::BadType),
            // NB: Reading everything keeps loops whose code may invalidate
            //     the code from hoisting it.
            Op::AssumeValid | Op::AssumeLegacyArgumentsUnread => OpInfo::pure(I::NONE)
                .with_properties(P::reading(Locations::ALL))
                .exiting(ExitKind::Invalidated),
            Op::CheckInterrupt => OpInfo::pure(I::NONE)
                .with_properties(P::writing(Locations::INTERRUPT))
                .exiting(ExitKind::Interrupted),
            Op::LoadGlobalBinding { .. } => OpInfo::pure(I::NONE)
                .with_properties(P::reading(Locations::GLOBAL_BINDINGS))
                .exiting(ExitKind::BadShape),
            Op::StoreGlobalBinding { .. } => OpInfo::pure(I::TAGGED)
                .with_properties(P {
                    reads: Locations::GLOBAL_BINDINGS,
                    writes: Locations::GLOBAL_BINDINGS,
                    ..P::PURE
                })
                .exiting(ExitKind::BadShape),
            Op::CheckAccessorFunction { .. } => {
                OpInfo::check(ExitKind::UnexpectedValue, I::OBJECT).with_properties(P::reading(Locations::NAMED_SLOTS))
            }
            Op::LoadAccessorFunction { .. } => OpInfo::pure(I::OBJECT)
                .with_properties(P::reading(Locations::NAMED_SLOTS))
                .exiting(ExitKind::BadCallTarget),
            Op::LoadNamed { holds_data, .. } => {
                let info = OpInfo::pure(I::OBJECT).with_properties(P::reading(Locations::NAMED_SLOTS));
                if *holds_data {
                    info
                } else {
                    info.exiting(ExitKind::BadShape)
                }
            }
            Op::AddNamed { .. } => OpInfo::pure(I::OBJECT_VALUE).with_properties(P {
                reads: Locations::NAMED,
                writes: Locations::NAMED,
                ..P::PURE
            }),
            // NB: Stores to a property an object has change no shape.
            Op::StoreNamed { holds_data, .. } => {
                let info = OpInfo::pure(I::OBJECT_VALUE).with_properties(P {
                    reads: Locations::NAMED_SLOTS,
                    writes: Locations::NAMED_SLOTS,
                    ..P::PURE
                });
                if *holds_data {
                    info
                } else {
                    info.exiting(ExitKind::BadShape)
                }
            }
            Op::Jump { .. } | Op::Unreachable => OpInfo::control(I::NONE),
            Op::Branch { condition, .. } => OpInfo::control(I::Exactly(condition.input_reprs())),
            Op::BranchTruthy { .. } | Op::Return => OpInfo::control(I::TAGGED),
            Op::BranchOnPc { .. } => OpInfo::control(I::INT32),
            Op::ShapeSwitch { .. } => OpInfo::control(I::OBJECT)
                .with_properties(P::reading(Locations::SHAPES))
                .exiting(ExitKind::BadShape),
            Op::Exit { kind } => OpInfo::control(I::NONE).exiting(*kind),
            Op::Refine { condition, .. } => OpInfo {
                refined_input: Some(0),
                ..OpInfo::pure(I::Exactly(condition.input_reprs()))
            },
            Op::CallSlowPath {
                saves_registers: false,
                opcode,
                ..
            } => OpInfo::pure(I::TAGGED_ANY).with_properties(
                Op::Generic {
                    opcode: *opcode,
                    executable: 0,
                    pc: 0,
                }
                .properties(),
            ),
            // NB: The slow path saves the registers it clobbers, so the node
            //     is not a call for the register allocator.
            Op::CallSlowPath { .. } => OpInfo::pure(I::TAGGED_ANY).with_properties(P {
                reads: Locations::ALL,
                writes: Locations::ALL,
                runs_code: true,
                allocates: true,
                ..P::PURE
            }),
            Op::UnboxInt32 | Op::UnboxDouble => OpInfo::pure(I::TAGGED),
            Op::Int32Abs | Op::Int32ToFloat64 => OpInfo::pure(I::INT32),
            Op::Int32Min | Op::Int32Max | Op::Uint32Min | Op::Uint32ShiftRight => OpInfo::pure(I::INT32_2),
            Op::BoolToInt32 => OpInfo::pure(I::BOOL),
            Op::BoxFloat64 => OpInfo::pure(I::FLOAT64).never_empty(),
            Op::Float64Unary { .. } | Op::Float64ToInt32 | Op::Float64ToUint8Clamped => OpInfo::pure(I::FLOAT64),
            Op::Float64Binary { .. } | Op::Float64Min | Op::Float64Max | Op::Float64Compare { .. } => {
                OpInfo::pure(I::FLOAT64_2)
            }
            Op::CallFloat64Function { function } => {
                let inputs = match function.argument_count() {
                    1 => I::FLOAT64,
                    _ => I::FLOAT64_2,
                };
                OpInfo::pure(inputs).with_properties(P::CALL)
            }
            Op::CheckFloat64Int32 => OpInfo::pure(I::FLOAT64).exiting(ExitKind::NotInt32),
            Op::Uint32ToFloat64 => OpInfo::pure(I::INT32),
            Op::StringAddress => OpInfo::pure(I::TAGGED),
            // NB: Strings never change. These take the refinements of the
            //     checks they rely on as inputs.
            Op::StringLength => OpInfo::pure(I::POINTER),
            Op::LoadStringCodeUnit => OpInfo::pure(I::POINTER_INT32),
            Op::SingleCharacterString => OpInfo::pure(I::INT32),
            // NB: A cache miss and strings whose characters only the slow
            //     path reads stay that way until the slow path ran.
            Op::StringsEqual => OpInfo::pure(I::TAGGED_2),
            Op::ConcatenateStrings => OpInfo::pure(I::TAGGED_2).with_properties(P {
                allocates: true,
                ..P::PURE
            }),
            Op::IntegerToString => OpInfo::pure(I::INT32),
            // NB: It reads the shapes and the indexed properties of the
            //     prototypes too.
            Op::CheckAppendableArray => {
                OpInfo::check(ExitKind::SlowPath, I::TAGGED).with_properties(P::reading(Locations::ALL))
            }
            Op::LoadElementsLength | Op::LoadElementsCapacity => {
                OpInfo::pure(I::POINTER).with_properties(P::reading(Locations::ELEMENT_COUNTS))
            }
            Op::AppendElement => {
                OpInfo::pure(I::Exactly(&[Repr::Pointer, Repr::Int32, Repr::Tagged])).with_properties(P {
                    reads: Locations::ELEMENTS_AND_COUNTS,
                    writes: Locations::ELEMENTS_AND_COUNTS,
                    ..P::PURE
                })
            }
            Op::CallArrayPush => OpInfo::pure(I::TAGGED_2).with_properties(P {
                reads: Locations::ELEMENTS_AND_COUNTS,
                writes: Locations::ELEMENTS_AND_COUNTS,
                allocates: true,
                ..P::PURE
            }),
            Op::CallStoreHoleyElement => OpInfo::pure(I::Exactly(&[Repr::Pointer, Repr::Int32, Repr::Tagged]))
                .with_properties(P {
                    reads: Locations::ELEMENTS_AND_COUNTS,
                    writes: Locations::ELEMENTS_AND_COUNTS,
                    allocates: true,
                    ..P::PURE
                }),
            // NB: The runtime watches the intrinsics anew with new watchpoint sets once the old ones fired.
            Op::OpenArrayIteration => OpInfo::pure(I::TAGGED).with_properties(P {
                reads: Locations::ALL,
                allocates: true,
                ..P::PURE
            }),
            Op::LoadArrayIteratorArray
            | Op::LoadArrayIteratorIndex
            | Op::LoadGeneratorCompletionType
            | Op::LoadGeneratorCompletionValue => {
                OpInfo::pure(I::POINTER).with_properties(P::reading(Locations::ITERATORS))
            }
            Op::StoreArrayIteratorIndex => {
                OpInfo::pure(I::POINTER_INT32).with_properties(P::writing(Locations::ITERATORS))
            }
            Op::FinishArrayIterator => OpInfo::pure(I::POINTER).with_properties(P::writing(Locations::ITERATORS)),
            // NB: The keys of a cache never change.
            Op::LoadPropertyIteratorKeyCount => OpInfo::pure(I::TAGGED),
            Op::LoadPropertyIteratorKey => OpInfo::pure(I::Exactly(&[Repr::Tagged, Repr::Int32])),
            // NB: The slow path call leaves its outputs where the node reads
            //     them, so it stays right after the call (which the verifier
            //     checks). Its input, the call, ties it there.
            Op::SlowPathOutput { .. } => OpInfo::pure(I::TAGGED),
            Op::ProbePropertyCache { .. } => OpInfo::pure(I::TAGGED).with_properties(P::reading(Locations::NAMED)),
            Op::ProbeKeyedCache { .. } => OpInfo::pure(I::TAGGED_2).with_properties(P::reading(Locations::NAMED)),
            // NB: Additions may grow the object's storage.
            Op::ProbePropertyStore { .. } => OpInfo::pure(I::TAGGED_2).with_properties(P::STORE_PROBE),
            Op::ProbeKeyedStore { .. } => OpInfo::pure(I::TAGGED_3).with_properties(P::STORE_PROBE),
            // NB: Global variables are properties of the global object and
            //     bindings of the global declarative environment.
            Op::ProbeGlobalCache { .. } => OpInfo::pure(I::POINTER_2).with_properties(P::reading(Locations::ALL)),
            Op::ProbeGlobalStore { .. } => OpInfo::pure(I::Exactly(&[Repr::Tagged, Repr::Pointer, Repr::Pointer]))
                .with_properties(P {
                    reads: Locations::ALL,
                    writes: Locations::ALL,
                    runs_code: true,
                    ..P::PURE
                }),
            Op::ProbeHasProperty { .. } => OpInfo::pure(I::TAGGED_2).with_properties(P::reading(Locations::ALL)),
        }
    }

    pub fn properties(&self) -> NodeProperties {
        self.info().properties
    }

    /// Why this node exits to the interpreter, if it can exit eagerly.
    pub fn exit_kind(&self) -> Option<ExitKind> {
        self.info().exit_kind
    }

    /// Whether the node may exit to the interpreter before its side effects,
    /// resuming at the current bytecode: whether it has an exit kind.
    pub fn can_eager_exit(&self) -> bool {
        self.exit_kind().is_some()
    }

    /// For checks and refinements, whose value is an input known to pass
    /// them: which input that is (see "Refinements" in `Op`).
    pub fn refined_input(&self) -> Option<usize> {
        self.info().refined_input
    }

    pub fn is_control(&self) -> bool {
        self.info().is_control
    }
}

impl BranchCondition {
    /// The representations of the inputs a branch on the condition (and a `Refine` of it) takes.
    pub fn input_reprs(self) -> &'static [Repr] {
        match self {
            BranchCondition::Bool => &[Repr::Bool],
            BranchCondition::Nullish
            | BranchCondition::Undefined
            | BranchCondition::Object
            | BranchCondition::NonNegativeInt32
            | BranchCondition::ElementsKind(_)
            | BranchCondition::Int32Value
            | BranchCondition::Int32OrBoolean
            | BranchCondition::Double
            | BranchCondition::String
            | BranchCondition::Builtin(_) => &[Repr::Tagged],
            BranchCondition::IndexInBounds | BranchCondition::Int32(_) => &[Repr::Int32, Repr::Int32],
            BranchCondition::MagicalLength
            | BranchCondition::Extensible
            | BranchCondition::HoleFillableArray
            | BranchCondition::ArrayIterator
            | BranchCondition::GeneratorObject
            | BranchCondition::PlainArrayPrototypeChain
            | BranchCondition::ArrayWithRealmArrayPrototypeAndNoNamedProperties
            | BranchCondition::HoldsValidWatchpointSet
            | BranchCondition::ResolvedString
            | BranchCondition::BindingMutable { .. }
            | BranchCondition::NextBindingOfShape { .. } => &[Repr::Pointer],
            BranchCondition::Shape(_) => &[Repr::Tagged, Repr::Pointer],
            BranchCondition::Float64(_) => &[Repr::Float64, Repr::Float64],
            BranchCondition::TaggedEquals { .. } | BranchCondition::PropertyIteratorCacheValid => {
                &[Repr::Tagged, Repr::Tagged]
            }
        }
    }
}
