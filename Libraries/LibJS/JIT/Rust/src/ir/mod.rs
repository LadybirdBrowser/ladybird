/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The JIT's SSA intermediate representation.
//!
//! A `Graph` is a list of blocks in linear order. Every block has phis, a
//! body of nodes, and exactly one control node that ends it. All nodes live in
//! one arena and are named by `NodeId`. Constants are graph level nodes that
//! belong to no block: they are rematerialized wherever they are used.
//!
//! Frame memory (the interpreter's `ExecutionContext` slots) is not SSA: the
//! graph builder tracks which SSA value each frame slot holds and moves values
//! between frame memory and SSA values with `LoadSlot` and `StoreSlot`.

mod dump;
mod effects;
pub mod value;

pub use effects::Locations;

pub use dump::dump;
pub use dump::node_text;

use crate::bytecode::OpCode;
use crate::code::ExitKind;
use crate::code::Repr;
use crate::code::ResumeMode;
use crate::snapshot::CellId;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord, Default)]
pub struct NodeId(pub u32);

/// The slot `Graph::frame_state_values()` lists the properties of virtual
/// objects with, which are in no frame slot.
pub const VIRTUAL_OBJECT_PROPERTY: u32 = u32::MAX;

/// The slot frame states list the function object of a frame of an inlined
/// closure with (see `code::CALLEE_SLOT`).
pub use crate::code::CALLEE_SLOT;

impl NodeId {
    pub fn from_index(index: usize) -> Self {
        Self(u32::try_from(index).expect("node count fits in u32"))
    }

    pub fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct BlockId(pub u32);

impl BlockId {
    pub fn from_index(index: usize) -> Self {
        Self(u32::try_from(index).expect("block count fits in u32"))
    }

    pub fn index(self) -> usize {
        self.0 as usize
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub struct FrameStateId(pub u32);

impl FrameStateId {
    pub fn index(self) -> usize {
        self.0 as usize
    }
}

/// A field of an execution context that fast paths read.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FrameField {
    LexicalEnvironment,
    VariableEnvironment,
    PrivateEnvironment,
    Realm,
    Executable,
}

impl FrameField {
    pub const ALL: [FrameField; 5] = [
        FrameField::LexicalEnvironment,
        FrameField::VariableEnvironment,
        FrameField::PrivateEnvironment,
        FrameField::Realm,
        FrameField::Executable,
    ];

    /// Whether instructions of the frame's function can change the field:
    /// the environments change, the realm and the executable do not.
    pub fn is_environment(self) -> bool {
        matches!(
            self,
            FrameField::LexicalEnvironment | FrameField::VariableEnvironment | FrameField::PrivateEnvironment
        )
    }
}

/// What `Op::Branch` tests its inputs for.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BranchCondition {
    /// The input is a `Repr::Bool`.
    Bool,
    /// The input is a tagged value: is it `undefined` or `null`?
    Nullish,
    /// The input is a tagged value: is it `undefined`?
    Undefined,
    /// The input is a tagged value: is it an object?
    Object,
    /// The input is a tagged value: is it an int32 that is not negative?
    NonNegativeInt32,
    /// The input is a tagged value: is it an object whose elements are of
    /// this kind, like `Op::CheckElements` checks?
    ElementsKind(ElementsKind),
    /// The `Repr::Int32` input 0, as an unsigned number, is below the
    /// `Repr::Int32` input 1: is it an index within that count, like
    /// `Op::CheckBounds` checks?
    IndexInBounds,
    /// The input is the `Repr::Pointer` of an object: does it have a
    /// magical length, like an array?
    MagicalLength,
    /// The input is the `Repr::Pointer` of an object: is it extensible?
    Extensible,
    /// Input 0 is an object and input 1 its address: does it have the
    /// shape, like `Op::ShapeSwitch` cases test it? Only the refinements of
    /// those cases have this condition; no branch does.
    Shape(ShapeCheck),
    /// The two `Repr::Int32` inputs compare like `Op::Int32Compare`.
    Int32(Comparison),
    /// The two `Repr::Float64` inputs compare like `Op::Float64Compare`.
    Float64(Comparison),
    /// The two tagged inputs compare like `Op::TaggedEquals`.
    TaggedEquals { equal: bool },
    /// The input is a tagged value: is it an int32?
    Int32Value,
    /// The input is a tagged value: is it a double, a number that is no
    /// int32?
    Double,
    /// The input is a tagged value: is it a string?
    String,
    /// The input is the `Repr::Pointer` of a string (see
    /// `Op::StringAddress`): can its characters be read, because it is no
    /// rope or substring whose characters are elsewhere (see
    /// `Op::LoadStringCodeUnit`)?
    ResolvedString,
    /// The input is a tagged value: is it the function object of builtin
    /// function `id` (`Builtin` in the runtime), of any realm?
    Builtin(u8),
    /// Input 0 is a declarative environment (a `Repr::Pointer`): is its
    /// binding `index` mutable? Bindings keep their mutability.
    BindingMutable { index: u32 },
    /// Input 0 is an environment (a `Repr::Pointer`): is the binding its
    /// shape has next, after the ones it has, the binding `name` (an
    /// identifier of the executable, see `ExecutableSnapshot::identifiers`)
    /// with `flags`, which `Op::AppendEnvironmentBinding` can append, because it
    /// is a declarative environment (no module environment) whose shape
    /// gives every binding a different name, and its binding values have
    /// room for it?
    NextBindingOfShape { name: u64, flags: u8 },
    /// Input 0 is an object, input 1 the property iterator cache of a for-in
    /// loop over it (`ObjectPropertyIteratorCacheData`): are the keys the
    /// cache holds still those of the object, because it has the shape (and
    /// packed indexed properties) the cache was made for and its prototype
    /// chain is unchanged?
    PropertyIteratorCacheValid,
}

/// What `typeof` results in.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum TypeofKind {
    Number,
    Undefined,
    Object,
    String,
    Symbol,
    Boolean,
    Bigint,
    Function,
}

/// Which function of an accessor property an access calls.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AccessorPart {
    Getter,
    Setter,
}

/// A shape an object may have, as `CheckShape` and `ShapeSwitch` test it.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
pub struct ShapeCheck {
    pub shape: CellId,
    /// For dictionary shapes, the generation the shape must still have.
    pub dictionary_generation: Option<u32>,
}

/// The type of the elements of a typed array.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum TypedArrayElement {
    Uint8,
    Uint8Clamped,
    Int8,
    Uint16,
    Int16,
    Int32,
    Uint32,
    Float32,
    Float64,
}

impl TypedArrayElement {
    pub const ALL: [TypedArrayElement; 9] = [
        TypedArrayElement::Uint8,
        TypedArrayElement::Uint8Clamped,
        TypedArrayElement::Int8,
        TypedArrayElement::Uint16,
        TypedArrayElement::Int16,
        TypedArrayElement::Int32,
        TypedArrayElement::Uint32,
        TypedArrayElement::Float32,
        TypedArrayElement::Float64,
    ];

    /// Whether the elements are integers that fit in an int32.
    pub fn is_int32(self) -> bool {
        !matches!(
            self,
            TypedArrayElement::Uint32 | TypedArrayElement::Float32 | TypedArrayElement::Float64
        )
    }
}

/// How an object stores its indexed elements, as keyed accesses speculate
/// it does.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub enum ElementsKind {
    /// Packed or holey indexed storage of an object (like an array) with no
    /// indexed accessors and nothing else that interferes with indexed
    /// access.
    Packed,
    Holey,
    TypedArray(TypedArrayElement),
}

impl ElementsKind {
    /// The representation of the elements a load produces: a `Repr::Int32`
    /// or `Repr::Float64` number for typed arrays, and a tagged value
    /// otherwise.
    pub fn loaded_repr(self) -> Repr {
        match self {
            ElementsKind::TypedArray(element) if element.is_int32() => Repr::Int32,
            ElementsKind::TypedArray(_) => Repr::Float64,
            _ => Repr::Tagged,
        }
    }

    /// The representation of the value a store takes: a `Repr::Float64` for
    /// float typed arrays, a `Repr::Int32` (whose bits are those of the
    /// element) for other typed arrays, and a tagged value otherwise.
    pub fn stored_repr(self) -> Repr {
        match self {
            ElementsKind::TypedArray(TypedArrayElement::Float32 | TypedArrayElement::Float64) => Repr::Float64,
            ElementsKind::TypedArray(_) => Repr::Int32,
            _ => Repr::Tagged,
        }
    }
}

/// An arithmetic or bitwise operation of two operands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BinaryOp {
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    BitwiseAnd,
    BitwiseOr,
    BitwiseXor,
    LeftShift,
    RightShift,
    UnsignedRightShift,
}

impl BinaryOp {
    /// Whether the int32 operation can produce a result that is not an
    /// int32 (see `Op::Int32Binary`).
    pub fn int32_can_overflow(self) -> bool {
        matches!(
            self,
            BinaryOp::Add | BinaryOp::Sub | BinaryOp::Mul | BinaryOp::Mod | BinaryOp::UnsignedRightShift
        )
    }

    /// Whether the operation converts its operands with ToInt32.
    pub fn is_bitwise(self) -> bool {
        matches!(
            self,
            BinaryOp::BitwiseAnd
                | BinaryOp::BitwiseOr
                | BinaryOp::BitwiseXor
                | BinaryOp::LeftShift
                | BinaryOp::RightShift
                | BinaryOp::UnsignedRightShift
        )
    }
}

/// A comparison of two operands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Comparison {
    LessThan,
    LessThanEquals,
    GreaterThan,
    GreaterThanEquals,
    StrictlyEquals,
    StrictlyInequals,
    LooselyEquals,
    LooselyInequals,
}

impl Comparison {
    /// For the equalities and inequalities: whether they test for equality.
    pub fn equality(self) -> Option<bool> {
        match self {
            Comparison::StrictlyEquals | Comparison::LooselyEquals => Some(true),
            Comparison::StrictlyInequals | Comparison::LooselyInequals => Some(false),
            _ => None,
        }
    }

    pub fn is_loose(self) -> bool {
        matches!(self, Comparison::LooselyEquals | Comparison::LooselyInequals)
    }
}

/// What a node does.
///
/// Refinements: a check that exits unless its input 0 passes it (`CheckObject`,
/// `CheckShape`, `CheckValue`, `CheckClosure`, `CheckAccessorFunction`,
/// `CheckElements`, `CheckIdentityComparable`, `CheckAppendableArray`,
/// `CheckBounds`, `CheckNotHole`), and a
/// `Refine` on the edge of a branch, has that input as its value, refined:
/// known to pass. Nodes that rely on the check take the refined value as their
/// input instead of the input itself, so that the check is a data dependency
/// of theirs: passes move or share them exactly when they can move or share
/// the check. Before register allocation, every use of a refined value
/// becomes a use of the value it refines (see `edit::remove_refinements()`).
/// `CheckInt32` refines and unboxes its input at once.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Op {
    /// A NaN-boxed constant.
    Constant(u64),
    /// Reads frame slot `slot` (a flat operand index).
    LoadSlot {
        slot: u32,
    },
    /// Writes input 0 to frame slot `slot`.
    StoreSlot {
        slot: u32,
    },
    /// One input per block predecessor, in predecessor order.
    Phi,
    /// The `Enter` bytecode: fills the registers and locals with the empty
    /// value, copies the constants into the frame and marks it initialized.
    InitializeFrame,
    /// Makes the frame the running execution context, where other code (the
    /// garbage collector, stack walks, runtime functions) sees it. Direct
    /// calls of compiled functions leave their frame unpublished until it
    /// is. `InitializeFrame` and `EnsureFrameInitialized` publish the frame
    /// too. See `passes::frame_initialization`.
    PublishFrame,
    /// Does what `InitializeFrame` does, unless the frame is initialized
    /// already. See `passes::frame_initialization`.
    EnsureFrameInitialized,
    /// The environment the environment at the `Repr::Pointer` input 0 is in
    /// (its outer environment), as a `Repr::Pointer`. Environments keep
    /// their outer environment.
    LoadOuterEnvironment,
    /// Binding `index` of the declarative environment at the
    /// `Repr::Pointer` input 0: its value, or the empty value while it is
    /// uninitialized.
    LoadEnvironmentBinding {
        index: u32,
    },
    /// Stores input 1 into binding `index` of the declarative environment
    /// at the `Repr::Pointer` input 0.
    StoreEnvironmentBinding {
        index: u32,
    },
    /// Appends the binding the shape of the declarative environment at the
    /// `Repr::Pointer` input 0 has next to its bindings, uninitialized,
    /// where a branch on `BranchCondition::NextBindingOfShape` found room.
    AppendEnvironmentBinding,
    /// The tagged cell value of the cell at the `Repr::Pointer` input 0,
    /// which is no object: an environment, as bytecode registers hold it.
    BoxCell,
    /// The `SetLexicalEnvironment` bytecode: input 0, a cell value, becomes
    /// the frame's lexical environment. The node's value is its address, as
    /// a `Repr::Pointer` like `LoadFrameField` gives it.
    SetLexicalEnvironment,
    /// The `LeavePrivateEnvironment` bytecode.
    LeavePrivateEnvironment,
    /// The `IsCallable` bytecode: a boolean value telling whether input 0 is a function object.
    IsCallable,
    /// Truthiness of the tagged input 0 as a `Repr::Bool`, computed by the
    /// runtime's `to_boolean` helper. This is the slow path of `BranchTruthy`.
    ToBoolean,
    /// `String(value)` of the primitive input 0, through the runtime:
    /// ToString, but the descriptive string of a symbol.
    PrimitiveToString,
    /// ToObject of input 0, a primitive other than undefined and null, as
    /// the builtin function input 1 makes it (in its realm), through the
    /// runtime, or the empty value where that is not the running realm.
    ToObject,
    /// ArrayCreate of the length input 0, a non-negative `Repr::Int32`, as
    /// the Array constructor input 1 makes it (in its realm), through the
    /// runtime, or the empty value where that is not the running realm.
    ArrayCreate,
    /// Runs one bytecode instruction through the interpreter's slow path for
    /// its opcode. The frame is the only input: every slot the instruction
    /// reads is in frame memory, and slots it writes are read back from there.
    /// Conditional jumps produce the `Repr::Int32` offset of the next
    /// instruction to run.
    Generic {
        opcode: OpCode,
        /// The instruction's executable (an index into `Snapshot::executables`)
        /// and its offset there.
        executable: u32,
        pc: u32,
    },
    /// The `Call` instruction at `pc` of `executable`, like `Generic`, but
    /// calling its call feedback's `DirectCallTarget` directly: JIT code
    /// builds the callee's frame the way the interpreter's `Call` fast path
    /// does and enters the callee's JIT code (or the interpreter). Other
    /// callees, and calls the fast path cannot make, go through the
    /// `Generic` slow path.
    ///
    /// With `inlined_frame_bytes`, the call is in an inlined callee and runs
    /// without the frames of the inlined calls it is in: the node's value is
    /// the call's result, the callee's frame names the inlined call site for
    /// stack walks (see `SiteKind::Call`), and the call leaves room
    /// for those frames, of `inlined_frame_bytes`, below the callee's frame,
    /// where its slow paths materialize them.
    ///
    /// The node's inputs are the callee, the `this` value and the arguments,
    /// in registers (see `codegen::DIRECT_CALL_MAX_INPUTS`), or with
    /// `stores_operands`, all of the instruction's operands (callee, `this`,
    /// arguments) anywhere, which its code stores to their frame slots before
    /// it reads them from there.
    /// The node's value is the call's result. Without inputs (for operands
    /// holding an arguments object the code never created), the node reads
    /// its operands from the frame and writes the result there, like a
    /// `Generic` node.
    CallDirect {
        executable: u32,
        pc: u32,
        inlined_frame_bytes: u32,
        stores_operands: bool,
    },
    /// The `Call` instruction at `pc` of `executable`, like `Generic`, but
    /// calling its call feedback's `NativeCallTarget` directly, in the
    /// lightweight frame the interpreter's `Call` fast path builds for raw
    /// native functions. Other callees go through the `Generic` slow path.
    /// `inlined_frame_bytes`, `stores_operands` and the inputs are like
    /// `CallDirect`'s, but always with the `this` value.
    CallNative {
        executable: u32,
        pc: u32,
        inlined_frame_bytes: u32,
        stores_operands: bool,
    },
    /// The running frame's passed argument count, as an int32 value.
    ArgumentCount,
    /// The raw pointer in `field` of the running frame's execution context,
    /// as a `Repr::Pointer`. Nodes take the parts of the execution context
    /// they need as inputs: in the compiled function, loaded with this once
    /// per path until an instruction may replace them (see
    /// `GraphBuilder::frame_field()`), and in inlined callees, whose
    /// execution contexts only exist as SSA values, constants.
    LoadFrameField {
        field: FrameField,
    },
    /// `arguments[i]` of the running frame's arguments object, which was
    /// never created, for the index value in input 0: the argument slot
    /// `arguments_base + i`. Exits unless the index is an int32 within the
    /// passed arguments.
    LoadArgument {
        arguments_base: u32,
    },
    /// The `Call` instruction at `pc` of `executable`, `f.apply(this_arg, arguments)`
    /// with `Function.prototype.apply` as its callee and the running frame's
    /// arguments object, which was never created, as its last argument: like
    /// a `Generic` call, but the runtime calls `f` with the frame's arguments.
    CallForwardingArguments {
        executable: u32,
        pc: u32,
    },
    /// Input 0 as a `Repr::Int32`. Exits with `ExitKind::NotInt32` unless
    /// it is an int32.
    CheckInt32,
    /// The number input 0 as a `Repr::Float64`. Exits with
    /// `ExitKind::BadType` unless it is a number.
    CheckNumber,
    /// The tagged value of the `Repr::Int32` input 0.
    BoxInt32,
    /// The tagged boolean of the `Repr::Bool` input 0.
    BoxBool,
    /// `input 0 op input 1` of two `Repr::Int32` inputs, as a `Repr::Int32`.
    /// Exits with `ExitKind::Overflow` where the result is not an int32:
    /// Add, Sub and Mul on overflow (Mul also on -0), Mod for a zero
    /// divisor and on -0, UnsignedRightShift for results above `i32::MAX`.
    /// Div is no int32 operation.
    Int32Binary {
        op: BinaryOp,
    },
    /// `input 0 comparison input 1` of two `Repr::Int32` inputs, as a
    /// `Repr::Bool`. Equalities and inequalities are strict and loose alike.
    Int32Compare {
        comparison: Comparison,
    },
    /// Whether the tagged inputs 0 and 1 have the same bits (or different
    /// ones, if not `equal`), as a `Repr::Bool`. For values where that is
    /// strict equality: anything but numbers, strings and bigints.
    TaggedEquals {
        equal: bool,
    },
    /// Exits with `ExitKind::BadElements` unless input 0 is an object with
    /// elements of `kind`. For `ElementsKind::Holey`, packed ones qualify too.
    CheckElements {
        kind: ElementsKind,
    },
    /// The length of the typed array at the `Repr::Pointer` input 0, as a
    /// `Repr::Int32`, or 0 if its data is not cached (because its buffer is
    /// detached or resizable): how many elements `LoadElementAt` and
    /// `StoreElementAt` may access.
    LoadTypedArrayLength,
    /// The `Repr::Int32` index input 0, which is known to be below the
    /// `Repr::Int32` count input 1 (as an unsigned number) from here on.
    /// Exits with `ExitKind::OutOfBounds` where it is not.
    CheckBounds,
    /// Element input 1 (a `Repr::Int32` index, which must be below the
    /// object's `LoadElementsLength` and `LoadElementsCapacity`, or its
    /// `LoadTypedArrayLength`) of the object at the `Repr::Pointer` input 0,
    /// whose elements are of `kind`, in `kind.loaded_repr()`. A hole of
    /// holey elements is the empty value.
    LoadElementAt {
        kind: ElementsKind,
    },
    /// Stores input 2, in `kind.stored_repr()`, as the element at index
    /// input 1 (like for `LoadElementAt`) of the object at the
    /// `Repr::Pointer` input 0, whose elements are of `kind`.
    StoreElementAt {
        kind: ElementsKind,
    },
    /// The tagged input 0, which is known to be no hole (the empty value
    /// `LoadElementAt` finds there) from here on. Exits with
    /// `ExitKind::OutOfBounds` where it is one.
    CheckNotHole,
    /// The address of the cell in the tagged input 0, as a `Repr::Pointer`.
    /// Nodes accessing an object take it as an extra input, so that one
    /// decoding of the tagged value serves all of them.
    CellAddress,
    /// A new plain object of `shape` (see `ObjectAllocationInfo`), whose
    /// `property_count` named properties are all `undefined`, with room for
    /// `reserve` (at least `property_count`) properties inline, for the
    /// properties `AddNamed` nodes add to it. Allocates inline when the
    /// object fits a size class, and through the runtime otherwise. The node
    /// only allocates: nothing else happens, and nothing can see the object
    /// before it is stored somewhere.
    AllocateObject {
        shape: CellId,
        property_count: u32,
        reserve: u32,
    },
    /// Stores input 1 into named property `offset` of the object in input
    /// 0, made by an `AllocateObject` with a shape that has the property as
    /// a writable data property, and seen by nothing but the code
    /// initializing it (an object literal's own `InitObjectLiteralProperty`
    /// instructions), so its shape is still that one.
    InitializeNamed {
        offset: u32,
    },
    /// An object that escape analysis replaced by the values of its
    /// properties (the inputs, in offset order), as an exit needs it: the
    /// runtime creates it with `shape` when the exit is taken. Frame states
    /// refer to it; it is in no block and has no code. An input may be
    /// another virtual object.
    VirtualObject {
        shape: CellId,
    },
    /// A new array of `count` packed elements, all `undefined`, of the
    /// realm of the compiled code (see `ArrayAllocationInfo`). Like
    /// `AllocateObject`, the node only allocates.
    AllocateArray {
        count: u32,
    },
    /// Stores input 1 into element `index` of the array in input 0, made by
    /// an `AllocateArray` with more elements, and seen by nothing but the
    /// code initializing it (an array literal's own instruction).
    InitializeElement {
        index: u32,
    },
    /// A new closure of the compiled function's `NewFunction` of
    /// `shared_function_data_index`, a copy of its closure template (see
    /// `FunctionAllocationInfo`) with the lexical environment (a cell
    /// pointer) in input 0 and the private environment in input 1. Like
    /// `AllocateObject`, the node only allocates.
    AllocateFunction {
        shared_function_data_index: u32,
    },
    /// A new lexical environment of the compiled function's
    /// `CreateLexicalEnvironment` with environment shape cache `shape_cache`
    /// and `capacity` bindings, from its template (see
    /// `LexicalEnvironmentTemplateSnapshot`), with the environment (a cell
    /// value) in input 0 as its parent. Like `AllocateObject`, the node only
    /// allocates.
    AllocateEnvironment {
        shape_cache: u32,
        capacity: u32,
    },
    /// `Array.prototype.slice.call(arguments, start)` with the running
    /// frame's arguments object, which was never created, and the start in
    /// input 0: an array of the passed arguments from `start` on, made by
    /// `RuntimeInfo::slice_arguments`. Exits unless the start is an int32.
    SliceArguments,
    /// Exits with `ExitKind::BadType` unless input 0 or input 1 is an
    /// object, a symbol, a boolean, undefined or null: a value strictly
    /// equal to exactly the values with the same bits.
    CheckIdentityComparable,
    /// Exits unless input 0 is an object.
    CheckObject,
    /// Exits with `ExitKind::BadCallTarget` unless input 0 is an ECMAScript
    /// function object with the `SharedFunctionInstanceData` `shared_data`:
    /// a closure of the function the code inlines.
    CheckClosure {
        shared_data: CellId,
    },
    /// The lexical environment (or with `private`, the private environment)
    /// of the ECMAScript function at the `CellAddress` input 0, which stays
    /// the same for the function's lifetime, as a `Repr::Pointer` like
    /// `LoadFrameField` gives it.
    LoadFunctionEnvironment {
        private: bool,
    },
    /// Exits with `kind` unless input 0 is the NaN-boxed value `expected`.
    CheckValue {
        expected: u64,
        kind: ExitKind,
    },
    /// Input 0, with the empty value replaced by `undefined`.
    EmptyToUndefined,
    /// `typeof` of input 0: one of the strings of `TypeofStrings`.
    Typeof,
    /// Whether `typeof` of input 0 is the string of `kind` (or not, if not
    /// `equal`), as a `Repr::Bool`.
    TypeofIs {
        kind: TypeofKind,
        equal: bool,
    },
    /// Exits unless the object in input 0 has one of `shapes`. An optional
    /// input 1 is the object's `CellAddress`.
    CheckShape {
        shapes: Vec<ShapeCheck>,
    },
    /// Exits unless the `PrototypeChainValidity` cell `validity` is still valid.
    CheckPrototypeChainValid {
        validity: CellId,
    },
    /// Whether the `[[Prototype]]` chain of input 0 contains input 1, like
    /// the loop of OrdinaryHasInstance: false for values that are not
    /// objects, as a `Repr::Bool`. Exits with `ExitKind::BadType` unless
    /// input 1 is an object, and where an object of the chain does not have
    /// the ordinary `[[GetPrototypeOf]]`.
    HasInPrototypeChain,
    /// Exits with `ExitKind::Invalidated` if the code was invalidated
    /// because something it depends on (see `Graph::dependencies`) stopped
    /// holding. Costs nothing: it is a no-op that the runtime replaces with
    /// a jump to its exit when it invalidates the code. The code only needs
    /// one where it relies on its dependencies after other code ran, which
    /// is the only way they can stop holding.
    AssumeValid,
    /// Loads binding `index` of the global declarative environment
    /// `environment`. Exits with `ExitKind::BadShape` if it is still
    /// uninitialized.
    LoadGlobalBinding {
        environment: CellId,
        index: u32,
    },
    /// Stores input 0 into the mutable binding `index` of the global
    /// declarative environment `environment`. Exits with
    /// `ExitKind::BadShape` if it is still uninitialized.
    StoreGlobalBinding {
        environment: CellId,
        index: u32,
    },
    /// Exits with `ExitKind::UnexpectedValue` unless the named property at
    /// `offset` of the object in input 0 is an accessor whose getter or
    /// setter (`part`) is the function `function`. Accessors can get other
    /// functions in place. Input 1 is the object's `CellAddress`.
    CheckAccessorFunction {
        offset: u32,
        part: AccessorPart,
        function: CellId,
    },
    /// The getter or setter (`part`) of the accessor in the named property
    /// at `offset` of the object in input 0, as an object value. Exits
    /// unless the property holds an accessor with that part. Input 1 is the
    /// object's `CellAddress`.
    LoadAccessorFunction {
        offset: u32,
        part: AccessorPart,
    },
    /// Loads the named property at `offset` of the object in input 0. Exits
    /// if the property holds an accessor. An optional input 1 is the
    /// object's `CellAddress`.
    LoadNamed {
        offset: u32,
    },
    /// Adds a property to the object in input 0 (whose `CellAddress` is
    /// input 2), made by an `AllocateObject` with room for it, which has the
    /// shape `shape` transitions from: stores input 1 into the property at
    /// `offset` (the shape's last one, of `property_count`), then gives the
    /// object `shape`. Checks before it ensure the object's old shape and
    /// that adding the property sets nothing in its prototype chain.
    AddNamed {
        offset: u32,
        shape: CellId,
        property_count: u32,
    },
    /// Stores input 1 into the named property at `offset` of the object in
    /// input 0, which already exists. Exits if the property holds an accessor.
    /// An optional input 2 is the object's `CellAddress`.
    StoreNamed {
        offset: u32,
    },

    // Control nodes. Each block ends with exactly one of these.
    Jump {
        target: BlockId,
    },
    /// Branches on input 0.
    Branch {
        condition: BranchCondition,
        if_true: BlockId,
        if_false: BlockId,
    },
    /// Branches on the truthiness of the tagged input 0. Booleans and int32
    /// values are tested inline; everything else continues at `fallback`.
    BranchTruthy {
        if_true: BlockId,
        if_false: BlockId,
        fallback: BlockId,
    },
    /// Continues at `if_true` if the `Repr::Int32` input 0 equals `pc`, and
    /// at `if_false` otherwise.
    BranchOnPc {
        pc: u32,
        if_true: BlockId,
        if_false: BlockId,
    },
    /// Continues at the block of the first case whose shape the object in
    /// input 0 has, or exits if there is none. An optional input 1 is the
    /// object's `CellAddress`.
    ShapeSwitch {
        cases: Vec<(ShapeCheck, BlockId)>,
    },
    /// Returns input 0 from the function, as is: the `End` bytecode, and the
    /// `Return` bytecode of a value whose empty value became `undefined`.
    Return,
    /// Unconditionally exits to the interpreter (eager exit).
    Exit {
        kind: ExitKind,
    },
    /// Control never reaches the end of this block, for example after a
    /// generic `Throw`.
    Unreachable,

    /// Input 0, refined: the same value, at the head of a block that only
    /// one edge reaches, the edge of a branch on `condition` of the inputs
    /// (the same as the branch's) where the condition is `holds`. Nodes that
    /// rely on the condition take the refinement as their input instead of
    /// input 0, which ties them to the branch as a data dependency: passes
    /// move or share them exactly when they can move or share the
    /// refinement, and refinements stay at the head of their block. Code
    /// generation sees none: refinements are removed after the passes.
    Refine {
        condition: BranchCondition,
        holds: bool,
    },

    /// Runs one bytecode instruction through the interpreter's slow path for
    /// its opcode, with the values of the operands the slow path reads as
    /// inputs, in the order of its layout (see `bytecode::slow_path_layout()`):
    /// the fields that are not outputs, then the array's elements. Its value
    /// is the instruction's first output (`SlowPathOutput` nodes right after
    /// it give the others), or for a conditional jump, the slow path's
    /// control word, which `BranchOnPc` tests.
    ///
    /// With `saves_registers`, the node is in a cold block that the code
    /// built for the instruction branches to where it does not handle the
    /// operands, with the instruction's eager frame state, and it keeps every
    /// value in its register: it saves the registers the call clobbers and
    /// restores them afterwards, so it is no call for the register
    /// allocator. In an inlined callee, its slow path runs in frames
    /// materialized from the frame state first.
    ///
    /// Otherwise it is a call, which runs the instruction in place of any
    /// code of the compiled function for it, with a frame state after the
    /// instruction like a `Generic` node's. Its outputs that live in frame
    /// memory (see `builder::is_ssa_slow_path_output()`) are stored there,
    /// and it has no value if the first one does.
    CallSlowPath {
        opcode: OpCode,
        /// The instruction's executable (an index into `Snapshot::executables`)
        /// and its offset there.
        executable: u32,
        pc: u32,
        saves_registers: bool,
    },
    // Numbers and strings.
    /// The `Repr::Int32` of the tagged input 0, which must be an int32: a
    /// branch on `BranchCondition::Int32Value` decides that first.
    UnboxInt32,
    /// The absolute value of the `Repr::Int32` input 0, which must not be
    /// `i32::MIN`, as a `Repr::Int32`.
    Int32Abs,
    /// The `Repr::Float64` of the `Repr::Int32` input 0.
    Int32ToFloat64,
    /// The `Repr::Float64` of the tagged input 0, which must be a double:
    /// the refinement of a branch on `BranchCondition::Double`.
    UnboxDouble,
    /// The tagged number of the `Repr::Float64` input 0, like
    /// `JS::Value(double)`: an int32 if it is one (but not -0), with NaN
    /// canonicalized.
    BoxFloat64,
    /// `op` of the `Repr::Float64` input 0, as a `Repr::Float64`.
    Float64Unary {
        op: Float64UnaryOp,
    },
    /// `input 0 op input 1` of two `Repr::Float64` inputs, as a
    /// `Repr::Float64`, for Add, Sub, Mul and Div.
    Float64Binary {
        op: BinaryOp,
    },
    /// `input 0 comparison input 1` of two `Repr::Float64` inputs, as a
    /// `Repr::Bool`: false if either is NaN, but for the inequalities.
    /// Equalities are strict and loose alike.
    Float64Compare {
        comparison: Comparison,
    },
    /// ToInt32 of the `Repr::Float64` input 0, as a `Repr::Int32`: its
    /// integral part modulo 2^32, and 0 for NaN and the infinities.
    Float64ToInt32,
    /// `input 0 >>> input 1` of two `Repr::Int32` inputs, as the
    /// `Repr::Int32` of the bits of the unsigned result.
    Uint32ShiftRight,
    /// The `Repr::Float64` of the bits of the `Repr::Int32` input 0, as an
    /// unsigned integer.
    Uint32ToFloat64,
    /// The address of the string input 0, as a `Repr::Pointer`, like
    /// `CellAddress` of objects.
    StringAddress,
    /// The length in code units of the string whose address is input 0, as
    /// a `Repr::Int32`.
    StringLength,
    /// The code unit at the `Repr::Int32` index input 1, which must be in
    /// bounds, of the string whose address is input 0 and whose characters
    /// must be readable (see `BranchCondition::ResolvedString`), as a
    /// `Repr::Int32`.
    LoadStringCodeUnit,
    /// The VM's string of the code unit input 0, a `Repr::Int32` below 128
    /// (see `RuntimeInfo::fast_paths.single_ascii_character_strings`).
    SingleCharacterString,
    /// Whether the strings 0 and 1 are equal, as a tagged boolean, or the
    /// empty value where only the slow path can tell: for strings with the
    /// same length whose characters are not in a word of their own.
    StringsEqual,
    /// The concatenation of the strings 0 and 1, or the empty value where
    /// only the slow path makes it. An empty string concatenates to the
    /// other one; a rope string is allocated inline (see
    /// `RopeAllocationInfo`), but for results short enough to be short
    /// strings, which come from the VM's cache of fly strings (when both
    /// strings are short).
    ConcatenateStrings,
    /// The VM's string of the `Repr::Int32` input 0, from its cache of the
    /// strings of small non-negative integers, or the empty value if the
    /// cache has none.
    IntegerToString,

    // Arrays.
    /// Exits with `ExitKind::SlowPath` unless input 0 is an array that
    /// `Array.prototype.push` appends to without any observable step: an
    /// extensible array with a writable length, no proxy target, with packed
    /// indexed storage or none yet, whose prototype chain is the realm's
    /// default one (`%Array.prototype%` and `%Object.prototype%`, without
    /// indexed properties). The node's value is input 0, refined.
    CheckAppendableArray,
    /// The number of indexed elements of the object at the `Repr::Pointer`
    /// input 0 (its array-like size), as a `Repr::Int32`.
    LoadElementsLength,
    /// How many elements the packed or holey indexed storage of the object
    /// at the `Repr::Pointer` input 0, which has such storage or none, has
    /// room for, as a `Repr::Int32`: 0 without storage, and at most
    /// `i32::MAX`.
    LoadElementsCapacity,
    /// Stores the tagged input 2 as the element of the object at the
    /// `Repr::Pointer` input 0 at the `Repr::Int32` index input 1, which is
    /// its number of elements and below their capacity, and counts the
    /// element. The node's value is the new number of elements, as a
    /// `Repr::Int32`.
    AppendElement,
    /// `Array.prototype.push` of the tagged input 1 to the array input 0, of
    /// a kind `CheckAppendableArray` accepts, through
    /// `RuntimeInfo::array_push`, which grows its elements. It saves the
    /// registers the call clobbers and restores them afterwards, so it is no
    /// call for the register allocator. The node's value is the new length.
    CallArrayPush,

    // For-in loops.
    /// The number of keys of the property iterator cache input 0, as a
    /// `Repr::Int32` (at most `i32::MAX`).
    LoadPropertyIteratorKeyCount,
    /// The key at the `Repr::Int32` index input 1, below their number, of
    /// the property iterator cache input 0.
    LoadPropertyIteratorKey,

    // Slow paths.
    /// Output `index` of the `CallSlowPath` input 0, whose value is output
    /// 0: the operands its instruction writes, in the order of the slow
    /// path's layout (see `bytecode::slow_path_layout()`), operands it reads
    /// and writes included. It comes right after that node, in its block.
    SlowPathOutput {
        index: u8,
    },

    // Keyed access and `length`.
    /// The data property (own, or of an unchanged prototype chain), or
    /// `undefined` for a missing property, that property lookup cache
    /// `cache` of executable `executable` has for the tagged input 0, like
    /// the interpreter's `GetById` handler finds it: for objects, for their
    /// shape, and for strings, numbers and booleans, for the shape of their
    /// prototype. The empty value if the cache has none, or for other
    /// values. Probing the cache is one step: its entries change as the
    /// interpreter runs.
    ProbePropertyCache {
        executable: u32,
        cache: u32,
    },
    /// The value of the own data property (or, for a missing property,
    /// `undefined`) that the string or symbol input 1 names on the object in
    /// input 0, where keyed property lookup cache `cache` of executable
    /// `executable` or the VM's keyed lookup cache has it, or the empty
    /// value.
    ProbeKeyedCache {
        executable: u32,
        cache: u32,
    },
    /// Stores input 1 into the writable own data property of the object in
    /// input 0 (or adds it) that an entry of property lookup cache `cache`
    /// of executable `executable` has for the object's shape, like the
    /// interpreter's `PutById` handler, as a `Repr::Bool` telling whether it
    /// did. Other values than objects are not stored to.
    ProbePropertyStore {
        executable: u32,
        cache: u32,
    },
    /// Stores input 2 into the writable own data property that the string or
    /// symbol input 1 names on the object in input 0 (or adds it), where
    /// keyed property lookup cache `cache` of executable `executable` or the
    /// VM's keyed store cache has it, as a `Repr::Bool` telling whether it
    /// did.
    ProbeKeyedStore {
        executable: u32,
        cache: u32,
    },
    /// The global variable that global variable cache `cache` of the
    /// executable in input 1 is for, like the interpreter's `GetGlobal`
    /// handler finds it with the realm in input 0 (both `Repr::Pointer`
    /// frame fields): a data property of the
    /// global object of the cached shape, or an initialized binding of the
    /// global declarative environment, while the environment has the cached
    /// serial number. The empty value where the cache does not apply.
    ProbeGlobalCache {
        cache: u32,
    },
    /// Stores input 0 into the global variable that global variable cache
    /// `cache` of the executable in input 2 is for, with the realm in input
    /// 1 (both `Repr::Pointer` frame fields), like the interpreter's
    /// `SetGlobal` handler: a writable data
    /// property or an initialized mutable binding. A `Repr::Bool` telling
    /// whether it did.
    ProbeGlobalStore {
        cache: u32,
    },
    /// Whether the object in input 0 has the property that the key in input
    /// 1 names, as its own (`own`, like `Object.prototype.hasOwnProperty`)
    /// or in its prototype chain too (like `in`), as a tagged boolean, where
    /// the VM can tell without running code (from its keyed lookup cache,
    /// the elements, or the runtime's lookup of objects whose lookups run no
    /// code). The empty value otherwise, and for other values than objects.
    ProbeHasProperty {
        own: bool,
    },
}

/// What `Op::Float64Unary` computes. Floor, Ceil and Round are those of
/// `Math`: Round rounds halves up, and zeros keep the sign of the input.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Float64UnaryOp {
    Abs,
    Negate,
    Floor,
    Ceil,
    Round,
    Sqrt,
}

impl Float64UnaryOp {
    pub fn name(self) -> &'static str {
        match self {
            Float64UnaryOp::Abs => "Abs",
            Float64UnaryOp::Negate => "Negate",
            Float64UnaryOp::Floor => "Floor",
            Float64UnaryOp::Ceil => "Ceil",
            Float64UnaryOp::Round => "Round",
            Float64UnaryOp::Sqrt => "Sqrt",
        }
    }
}

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
    const PURE: Self = Self {
        can_lazy_exit: false,
        is_call: false,
        reads: Locations::NONE,
        writes: Locations::NONE,
        runs_code: false,
        allocates: false,
    };
}

impl Op {
    pub fn properties(&self) -> NodeProperties {
        use NodeProperties as P;
        match self {
            Op::Constant(_) | Op::Refine { .. } => P::PURE,
            Op::LoadSlot { .. } => P {
                reads: Locations::FRAME,
                ..P::PURE
            },
            Op::StoreSlot { .. } => P {
                writes: Locations::FRAME,
                ..P::PURE
            },
            Op::Phi => P::PURE,
            Op::InitializeFrame | Op::EnsureFrameInitialized | Op::PublishFrame => P {
                writes: Locations::FRAME,
                ..P::PURE
            },
            Op::BoxCell | Op::LoadOuterEnvironment => P::PURE,
            Op::LoadEnvironmentBinding { .. } => P {
                reads: Locations::BINDINGS.union(Locations::GLOBAL_BINDINGS),
                ..P::PURE
            },
            Op::StoreEnvironmentBinding { .. } | Op::AppendEnvironmentBinding => P {
                writes: Locations::BINDINGS.union(Locations::GLOBAL_BINDINGS),
                ..P::PURE
            },
            Op::SetLexicalEnvironment | Op::LeavePrivateEnvironment => P {
                reads: Locations::FRAME,
                writes: Locations::FRAME,
                ..P::PURE
            },
            Op::IsCallable => P::PURE,
            Op::ToBoolean => P {
                is_call: true,
                ..P::PURE
            },
            Op::PrimitiveToString | Op::ToObject | Op::ArrayCreate => P {
                is_call: true,
                allocates: true,
                ..P::PURE
            },
            Op::CheckInt32 | Op::CheckNumber | Op::CheckIdentityComparable => P::PURE,
            Op::BoxInt32
            | Op::BoxBool
            | Op::Int32Binary { .. }
            | Op::Int32Compare { .. }
            | Op::TaggedEquals { .. }
            | Op::CellAddress => P::PURE,
            // NB: The elements kind of a typed array never changes.
            Op::CheckElements {
                kind: ElementsKind::TypedArray(_),
            } => P::PURE,
            Op::CheckElements { .. } => P {
                reads: Locations::ELEMENT_COUNTS,
                ..P::PURE
            },
            Op::LoadTypedArrayLength => P {
                reads: Locations::ELEMENT_COUNTS,
                ..P::PURE
            },
            Op::LoadElementAt { .. } => P {
                reads: Locations::ELEMENTS,
                ..P::PURE
            },
            // NB: Stores within the bounds change no element count.
            Op::StoreElementAt { .. } => P {
                writes: Locations::ELEMENTS,
                ..P::PURE
            },
            Op::CheckBounds | Op::CheckNotHole => P::PURE,
            Op::AllocateObject { .. } => P {
                allocates: true,
                ..P::PURE
            },
            Op::InitializeNamed { .. } => P {
                writes: Locations::NAMED_SLOTS,
                ..P::PURE
            },
            Op::SliceArguments => P {
                is_call: true,
                reads: Locations::FRAME,
                allocates: true,
                ..P::PURE
            },
            Op::AllocateFunction { .. } | Op::AllocateEnvironment { .. } => P {
                allocates: true,
                ..P::PURE
            },
            Op::VirtualObject { .. } => P::PURE,
            Op::AllocateArray { .. } => P {
                allocates: true,
                ..P::PURE
            },
            Op::InitializeElement { .. } => P {
                writes: Locations::ELEMENTS,
                ..P::PURE
            },
            Op::CheckObject | Op::CheckValue { .. } | Op::CheckClosure { .. } => P::PURE,
            Op::LoadFunctionEnvironment { .. } => P::PURE,
            Op::EmptyToUndefined | Op::Typeof | Op::TypeofIs { .. } => P::PURE,
            Op::CheckShape { .. } | Op::CheckPrototypeChainValid { .. } => P {
                reads: Locations::SHAPES,
                ..P::PURE
            },
            Op::CheckAccessorFunction { .. } => P {
                reads: Locations::NAMED_SLOTS,
                ..P::PURE
            },
            Op::LoadAccessorFunction { .. } => P {
                reads: Locations::NAMED_SLOTS,
                ..P::PURE
            },
            Op::LoadNamed { .. } => P {
                reads: Locations::NAMED_SLOTS,
                ..P::PURE
            },
            // NB: Reading everything keeps loops whose code may invalidate
            //     the code from hoisting it.
            Op::AssumeValid => P {
                reads: Locations::ALL,
                ..P::PURE
            },
            Op::HasInPrototypeChain => P {
                reads: Locations::SHAPES,
                ..P::PURE
            },
            Op::LoadGlobalBinding { .. } => P {
                reads: Locations::GLOBAL_BINDINGS,
                ..P::PURE
            },
            Op::StoreGlobalBinding { .. } => P {
                reads: Locations::GLOBAL_BINDINGS,
                writes: Locations::GLOBAL_BINDINGS,
                ..P::PURE
            },
            // NB: Stores to a property an object has change no shape.
            Op::StoreNamed { .. } => P {
                reads: Locations::NAMED_SLOTS,
                writes: Locations::NAMED_SLOTS,
                ..P::PURE
            },
            Op::AddNamed { .. } => P {
                reads: Locations::NAMED,
                writes: Locations::NAMED,
                ..P::PURE
            },
            Op::ArgumentCount => P {
                reads: Locations::FRAME,
                ..P::PURE
            },
            Op::LoadFrameField { .. } => P {
                reads: Locations::FRAME,
                ..P::PURE
            },
            Op::LoadArgument { .. } => P {
                reads: Locations::FRAME,
                ..P::PURE
            },
            Op::Generic { .. } | Op::CallDirect { .. } | Op::CallNative { .. } | Op::CallForwardingArguments { .. } => {
                P {
                    can_lazy_exit: true,
                    is_call: true,
                    reads: Locations::ALL,
                    writes: Locations::ALL,
                    runs_code: true,
                    allocates: true,
                }
            }
            Op::Jump { .. }
            | Op::Branch { .. }
            | Op::BranchTruthy { .. }
            | Op::BranchOnPc { .. }
            | Op::Return
            | Op::Unreachable => P::PURE,
            Op::Exit { .. } => P { ..P::PURE },
            Op::ShapeSwitch { .. } => P {
                reads: Locations::SHAPES,
                ..P::PURE
            },
            Op::CallSlowPath {
                saves_registers: false,
                opcode,
                ..
            } => Op::Generic {
                opcode: *opcode,
                executable: 0,
                pc: 0,
            }
            .properties(),
            // NB: The slow path call leaves its outputs where the node reads
            //     them, so it stays right after the call (which the verifier
            //     checks). Its input, the call, ties it there.
            Op::SlowPathOutput { .. } => P::PURE,
            Op::ProbePropertyCache { .. } | Op::ProbeKeyedCache { .. } => P {
                reads: Locations::NAMED,
                ..P::PURE
            },
            Op::ProbeHasProperty { .. } => P {
                reads: Locations::ALL,
                ..P::PURE
            },
            // NB: Global variables are properties of the global object and
            //     bindings of the global declarative environment.
            Op::ProbeGlobalCache { .. } => P {
                reads: Locations::ALL,
                ..P::PURE
            },
            Op::ProbeGlobalStore { .. } => P {
                reads: Locations::ALL,
                writes: Locations::ALL,
                runs_code: true,
                ..P::PURE
            },
            // NB: Additions may grow the object's storage.
            Op::ProbeKeyedStore { .. } | Op::ProbePropertyStore { .. } => P {
                reads: Locations::NAMED,
                writes: Locations::NAMED,
                allocates: true,
                ..P::PURE
            },
            // NB: The slow path saves the registers it clobbers, so the node
            //     is not a call for the register allocator.
            Op::CallSlowPath { .. } => P {
                reads: Locations::ALL,
                writes: Locations::ALL,
                runs_code: true,
                allocates: true,
                ..P::PURE
            },
            Op::UnboxInt32
            | Op::UnboxDouble
            | Op::BoxFloat64
            | Op::Float64Unary { .. }
            | Op::Float64Binary { .. }
            | Op::Float64Compare { .. }
            | Op::Float64ToInt32
            | Op::Uint32ShiftRight
            | Op::Uint32ToFloat64
            | Op::Int32Abs
            | Op::Int32ToFloat64
            | Op::StringAddress => P::PURE,
            // NB: A cache miss and strings whose characters only the slow
            //     path reads stay that way until the slow path ran.
            Op::StringsEqual | Op::IntegerToString => P::PURE,
            Op::ConcatenateStrings => P {
                allocates: true,
                ..P::PURE
            },
            // NB: Strings never change. These take the refinements of the
            //     checks they rely on as inputs.
            Op::StringLength | Op::LoadStringCodeUnit | Op::SingleCharacterString => P::PURE,
            // NB: It reads the shapes and the indexed properties of the
            //     prototypes too.
            Op::CheckAppendableArray => P {
                reads: Locations::ALL,
                ..P::PURE
            },
            Op::LoadElementsLength | Op::LoadElementsCapacity => P {
                reads: Locations::ELEMENT_COUNTS,
                ..P::PURE
            },
            Op::AppendElement => P {
                reads: Locations::ELEMENTS_AND_COUNTS,
                writes: Locations::ELEMENTS_AND_COUNTS,
                ..P::PURE
            },
            // NB: The keys of a cache never change.
            Op::LoadPropertyIteratorKeyCount | Op::LoadPropertyIteratorKey => P::PURE,
            Op::CallArrayPush => P {
                reads: Locations::ELEMENTS_AND_COUNTS,
                writes: Locations::ELEMENTS_AND_COUNTS,
                allocates: true,
                ..P::PURE
            },
        }
    }

    pub fn name(&self) -> &'static str {
        match self {
            Op::Constant(_) => "Constant",
            Op::LoadSlot { .. } => "LoadSlot",
            Op::StoreSlot { .. } => "StoreSlot",
            Op::Phi => "Phi",
            Op::InitializeFrame => "InitializeFrame",
            Op::EnsureFrameInitialized => "EnsureFrameInitialized",
            Op::PublishFrame => "PublishFrame",
            Op::BoxCell => "BoxCell",
            Op::LoadOuterEnvironment => "LoadOuterEnvironment",
            Op::LoadEnvironmentBinding { .. } => "LoadEnvironmentBinding",
            Op::StoreEnvironmentBinding { .. } => "StoreEnvironmentBinding",
            Op::AppendEnvironmentBinding => "AppendEnvironmentBinding",
            Op::SetLexicalEnvironment => "SetLexicalEnvironment",
            Op::LeavePrivateEnvironment => "LeavePrivateEnvironment",
            Op::IsCallable => "IsCallable",
            Op::ToBoolean => "ToBoolean",
            Op::PrimitiveToString => "PrimitiveToString",
            Op::ToObject => "ToObject",
            Op::ArrayCreate => "ArrayCreate",
            Op::Generic { .. } => "Generic",
            Op::CallDirect { .. } => "CallDirect",
            Op::CallNative { .. } => "CallNative",
            Op::ArgumentCount => "ArgumentCount",
            Op::LoadFrameField { .. } => "LoadFrameField",
            Op::LoadArgument { .. } => "LoadArgument",
            Op::CallForwardingArguments { .. } => "CallForwardingArguments",
            Op::CheckInt32 => "CheckInt32",
            Op::CheckNumber => "CheckNumber",
            Op::BoxInt32 => "BoxInt32",
            Op::BoxBool => "BoxBool",
            Op::Int32Binary { .. } => "Int32Binary",
            Op::Int32Compare { .. } => "Int32Compare",
            Op::TaggedEquals { .. } => "TaggedEquals",
            Op::CheckElements { .. } => "CheckElements",
            Op::LoadTypedArrayLength => "LoadTypedArrayLength",
            Op::CheckBounds => "CheckBounds",
            Op::LoadElementAt { .. } => "LoadElementAt",
            Op::StoreElementAt { .. } => "StoreElementAt",
            Op::CheckNotHole => "CheckNotHole",
            Op::CellAddress => "CellAddress",
            Op::AllocateObject { .. } => "AllocateObject",
            Op::InitializeNamed { .. } => "InitializeNamed",
            Op::AllocateArray { .. } => "AllocateArray",
            Op::VirtualObject { .. } => "VirtualObject",
            Op::AllocateFunction { .. } => "AllocateFunction",
            Op::AllocateEnvironment { .. } => "AllocateEnvironment",
            Op::SliceArguments => "SliceArguments",
            Op::InitializeElement { .. } => "InitializeElement",
            Op::CheckIdentityComparable => "CheckIdentityComparable",
            Op::CheckObject => "CheckObject",
            Op::CheckValue { .. } => "CheckValue",
            Op::CheckClosure { .. } => "CheckClosure",
            Op::LoadFunctionEnvironment { .. } => "LoadFunctionEnvironment",
            Op::EmptyToUndefined => "EmptyToUndefined",
            Op::Typeof => "Typeof",
            Op::TypeofIs { .. } => "TypeofIs",
            Op::CheckShape { .. } => "CheckShape",
            Op::CheckPrototypeChainValid { .. } => "CheckPrototypeChainValid",
            Op::AssumeValid => "AssumeValid",
            Op::HasInPrototypeChain => "HasInPrototypeChain",
            Op::LoadGlobalBinding { .. } => "LoadGlobalBinding",
            Op::StoreGlobalBinding { .. } => "StoreGlobalBinding",
            Op::CheckAccessorFunction { .. } => "CheckAccessorFunction",
            Op::LoadAccessorFunction { .. } => "LoadAccessorFunction",
            Op::LoadNamed { .. } => "LoadNamed",
            Op::StoreNamed { .. } => "StoreNamed",
            Op::AddNamed { .. } => "AddNamed",
            Op::ShapeSwitch { .. } => "ShapeSwitch",
            Op::Jump { .. } => "Jump",
            Op::Branch { .. } => "Branch",
            Op::BranchTruthy { .. } => "BranchTruthy",
            Op::BranchOnPc { .. } => "BranchOnPc",
            Op::Return => "Return",
            Op::Exit { .. } => "Exit",
            Op::Unreachable => "Unreachable",
            Op::Refine { .. } => "Refine",
            Op::CallSlowPath { .. } => "CallSlowPath",
            Op::UnboxInt32 => "UnboxInt32",
            Op::Int32Abs => "Int32Abs",
            Op::Int32ToFloat64 => "Int32ToFloat64",
            Op::UnboxDouble => "UnboxDouble",
            Op::BoxFloat64 => "BoxFloat64",
            Op::Float64Unary { .. } => "Float64Unary",
            Op::Float64Binary { .. } => "Float64Binary",
            Op::Float64Compare { .. } => "Float64Compare",
            Op::Float64ToInt32 => "Float64ToInt32",
            Op::Uint32ShiftRight => "Uint32ShiftRight",
            Op::Uint32ToFloat64 => "Uint32ToFloat64",
            Op::StringsEqual => "StringsEqual",
            Op::ConcatenateStrings => "ConcatenateStrings",
            Op::IntegerToString => "IntegerToString",
            Op::StringAddress => "StringAddress",
            Op::StringLength => "StringLength",
            Op::LoadStringCodeUnit => "LoadStringCodeUnit",
            Op::SingleCharacterString => "SingleCharacterString",
            Op::CheckAppendableArray => "CheckAppendableArray",
            Op::LoadElementsLength => "LoadElementsLength",
            Op::LoadElementsCapacity => "LoadElementsCapacity",
            Op::AppendElement => "AppendElement",
            Op::CallArrayPush => "CallArrayPush",
            Op::SlowPathOutput { .. } => "SlowPathOutput",
            Op::LoadPropertyIteratorKeyCount => "LoadPropertyIteratorKeyCount",
            Op::LoadPropertyIteratorKey => "LoadPropertyIteratorKey",
            Op::ProbePropertyCache { .. } => "ProbePropertyCache",
            Op::ProbeKeyedCache { .. } => "ProbeKeyedCache",
            Op::ProbeKeyedStore { .. } => "ProbeKeyedStore",
            Op::ProbePropertyStore { .. } => "ProbePropertyStore",
            Op::ProbeGlobalCache { .. } => "ProbeGlobalCache",
            Op::ProbeGlobalStore { .. } => "ProbeGlobalStore",
            Op::ProbeHasProperty { .. } => "ProbeHasProperty",
        }
    }

    /// For checks and refinements, whose value is an input known to pass
    /// them: which input that is (see "Refinements" in `Op`).
    pub fn refined_input(&self) -> Option<usize> {
        match self {
            Op::Refine { .. }
            | Op::CheckObject
            | Op::CheckShape { .. }
            | Op::CheckValue { .. }
            | Op::CheckClosure { .. }
            | Op::CheckAccessorFunction { .. }
            | Op::CheckElements { .. }
            | Op::CheckIdentityComparable
            | Op::CheckAppendableArray
            | Op::CheckBounds
            | Op::CheckNotHole => Some(0),
            _ => None,
        }
    }

    pub fn is_control(&self) -> bool {
        matches!(
            self,
            Op::Jump { .. }
                | Op::Branch { .. }
                | Op::BranchTruthy { .. }
                | Op::BranchOnPc { .. }
                | Op::ShapeSwitch { .. }
                | Op::Return
                | Op::Exit { .. }
                | Op::Unreachable
        )
    }

    /// Why this node exits to the interpreter, if it can exit eagerly.
    /// Whether the node may exit to the interpreter before its side effects,
    /// resuming at the current bytecode: whether it has an exit kind.
    pub fn can_eager_exit(&self) -> bool {
        self.exit_kind().is_some()
    }

    pub fn exit_kind(&self) -> Option<ExitKind> {
        match self {
            Op::Exit { kind } => Some(*kind),
            Op::CheckObject => Some(ExitKind::NotObject),
            Op::CheckElements { .. } => Some(ExitKind::BadElements),
            Op::CheckBounds | Op::CheckNotHole => Some(ExitKind::OutOfBounds),
            Op::CheckInt32 => Some(ExitKind::NotInt32),
            Op::CheckNumber | Op::CheckIdentityComparable | Op::HasInPrototypeChain => Some(ExitKind::BadType),
            Op::AssumeValid => Some(ExitKind::Invalidated),
            Op::Int32Binary { op } if op.int32_can_overflow() => Some(ExitKind::Overflow),
            Op::CheckValue { kind, .. } => Some(*kind),
            Op::LoadArgument { .. } | Op::SliceArguments => Some(ExitKind::ArgumentsIndex),
            Op::CheckAppendableArray => Some(ExitKind::SlowPath),
            Op::LoadAccessorFunction { .. } | Op::CheckClosure { .. } => Some(ExitKind::BadCallTarget),
            Op::CheckAccessorFunction { .. } => Some(ExitKind::UnexpectedValue),
            Op::CheckShape { .. }
            | Op::CheckPrototypeChainValid { .. }
            | Op::LoadGlobalBinding { .. }
            | Op::StoreGlobalBinding { .. }
            | Op::LoadNamed { .. }
            | Op::StoreNamed { .. }
            | Op::ShapeSwitch { .. } => Some(ExitKind::BadShape),
            _ => None,
        }
    }

    /// The blocks a control node can continue at.
    pub fn successors(&self) -> Successors {
        let inline = |blocks: &[BlockId]| {
            let mut inline = [BlockId(0); 3];
            inline[..blocks.len()].copy_from_slice(blocks);
            Successors::Inline {
                blocks: inline,
                count: blocks.len() as u8,
            }
        };
        match self {
            Op::Jump { target } => inline(&[*target]),
            Op::Branch { if_true, if_false, .. } | Op::BranchOnPc { if_true, if_false, .. } => {
                inline(&[*if_true, *if_false])
            }
            Op::BranchTruthy {
                if_true,
                if_false,
                fallback,
            } => inline(&[*if_true, *if_false, *fallback]),
            Op::ShapeSwitch { cases } => Successors::Cases(cases.iter().map(|(_, block)| *block).collect()),
            _ => inline(&[]),
        }
    }

    fn successors_mut(&mut self) -> Vec<&mut BlockId> {
        match self {
            Op::Jump { target } => vec![target],
            Op::Branch { if_true, if_false, .. } | Op::BranchOnPc { if_true, if_false, .. } => {
                vec![if_true, if_false]
            }
            Op::BranchTruthy {
                if_true,
                if_false,
                fallback,
            } => vec![if_true, if_false, fallback],
            Op::ShapeSwitch { cases } => cases.iter_mut().map(|(_, block)| block).collect(),
            _ => Vec::new(),
        }
    }
}

/// The blocks a control node can continue at (see `Op::successors()`):
/// those of jumps and branches inline, and the cases of shape switches.
#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Successors {
    Inline { blocks: [BlockId; 3], count: u8 },
    Cases(Vec<BlockId>),
}

impl std::ops::Deref for Successors {
    type Target = [BlockId];

    fn deref(&self) -> &[BlockId] {
        match self {
            Successors::Inline { blocks, count } => &blocks[..usize::from(*count)],
            Successors::Cases(blocks) => blocks,
        }
    }
}

impl IntoIterator for Successors {
    type Item = BlockId;
    type IntoIter = SuccessorsIntoIter;

    fn into_iter(self) -> SuccessorsIntoIter {
        SuccessorsIntoIter {
            successors: self,
            next: 0,
        }
    }
}

pub struct SuccessorsIntoIter {
    successors: Successors,
    next: usize,
}

impl Iterator for SuccessorsIntoIter {
    type Item = BlockId;

    fn next(&mut self) -> Option<BlockId> {
        let block = self.successors.get(self.next).copied()?;
        self.next += 1;
        Some(block)
    }
}

#[derive(Debug, Clone)]
pub struct Node {
    pub op: Op,
    pub inputs: Vec<NodeId>,
    /// The representation of the value this node produces, if it produces one.
    pub repr: Option<Repr>,
    /// The interpreter state to rebuild if this node exits.
    pub frame_state: Option<FrameStateId>,
    /// The bytecode offset of the instruction this node was built for.
    pub pc: u32,
}

impl Node {
    /// A node without a frame state.
    pub fn new(op: Op, inputs: Vec<NodeId>, repr: Option<Repr>, pc: u32) -> Self {
        Self {
            op,
            inputs,
            repr,
            frame_state: None,
            pc,
        }
    }
}

#[derive(Debug, Clone, Default)]
pub struct Block {
    pub predecessors: Vec<BlockId>,
    pub phis: Vec<NodeId>,
    pub body: Vec<NodeId>,
    /// The control node ending this block. Every finished block has one.
    pub control: Option<NodeId>,
    /// The bytecode offset of the first instruction, for blocks that start a
    /// bytecode basic block. Blocks the builder adds on control flow edges
    /// have none.
    pub bytecode_start: Option<u32>,
    pub is_loop_header: bool,
    /// Code that rarely runs, like the slow path of an instruction: code
    /// generation puts cold blocks after all other blocks, and the register
    /// allocator keeps the other blocks' values where they are on the
    /// edges into and out of them.
    pub is_cold: bool,
    /// For merges and loop headers of the compiled function that start a
    /// bytecode block, the frame state at their start, which checks of the
    /// values entering them exit with (see `edit::frame_state_entering()`).
    /// Passes forget it once frame states are final.
    pub start_frame_state: Option<FrameStateId>,
}

impl Block {
    /// The block's nodes, in order: its phis, its body and its control node.
    pub fn nodes(&self) -> impl Iterator<Item = NodeId> + '_ {
        self.phis.iter().chain(&self.body).chain(&self.control).copied()
    }
}

/// The interpreter state of a frame at a point where compiled code may exit.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FrameState {
    /// Index into `Snapshot::executables`.
    pub executable: u32,
    pub pc: u32,
    pub mode: ResumeMode,
    /// The SSA value of every live frame slot (a flat operand index) whose
    /// frame memory does not already hold it, in slot order. Frames of
    /// inlined calls have no memory, so they list every live slot.
    pub values: Vec<(u32, NodeId)>,
    /// The live frame slots whose value frame memory holds: the compiled
    /// function's slots that are in sync, and the slots a slow path in an
    /// inlined callee wrote into the frames pushed for it. With `values`,
    /// every live slot is listed once. The destination of a `ResumeAfter`
    /// frame state is listed with the value it had before the instruction:
    /// where the instruction did not continue (it threw, or its callee runs
    /// in the interpreter and writes it when it returns), that is what the
    /// frame needs. Exits after the instruction keep what it wrote.
    pub in_frame: Vec<u32>,
    /// For frames of inlined calls, the frame state of the caller: resuming
    /// after its `Call` instruction, with the call's result in `dst`.
    pub parent: Option<FrameStateId>,
    /// For a caller at an inlined call that passes the callee other
    /// arguments than its `Call` instruction's (a call forwarded through
    /// `Function.prototype.apply` or `call`, or a bound function): how many.
    pub passed_argument_count: Option<u32>,
}

#[derive(Debug, Clone, Default)]
pub struct Graph {
    pub nodes: Vec<Node>,
    /// Blocks in linear order. Block 0 is the entry, and every block comes
    /// after all its predecessors except those reaching it over a loop back
    /// edge, and cold ones once the passes moved cold blocks last.
    pub blocks: Vec<Block>,
    pub frame_states: Vec<FrameState>,
    /// Cells the graph compares against or reads from (shapes, prototype
    /// holders, validity cells, inlined functions). The code is only valid
    /// while they live.
    pub embedded_cells: Vec<CellId>,
    /// What the code assumes keeps holding where it has `Op::AssumeValid`
    /// nodes (see `Dependency`).
    pub dependencies: Vec<crate::code::Dependency>,
    /// The shapes from the snapshot that were stable, which check
    /// elimination may make the code depend on.
    pub stable_shapes: Vec<CellId>,
    /// The most bytes of interpreter stack that materializing the frames of
    /// inlined calls, and the frames of direct and native calls on top of
    /// them, take at once. Compiled code checks at entry that they fit.
    pub materialized_frame_bytes: u64,
    /// On-stack replacement entries: blocks without predecessors where code
    /// already running in the interpreter enters compiled code, with the pc of
    /// the loop back edge each one starts at. The frame holds every value.
    pub osr_entries: Vec<(u32, BlockId)>,
    /// The register-saving `CallSlowPath` nodes that may run before the
    /// frame is initialized, and so initialize it first if it is not.
    pub slow_paths_initializing_frame: Vec<NodeId>,
    /// Where exits of the compiled function were taken before, as (pc,
    /// kind), for passes that must not repeat a failed speculation.
    pub exit_sites: Vec<(u32, ExitKind)>,
}

impl Graph {
    /// Records that the code relies on `dependency`, which makes the
    /// runtime discard the code once it no longer holds.
    pub fn depend_on(&mut self, dependency: crate::code::Dependency) {
        if !self.dependencies.contains(&dependency) {
            self.dependencies.push(dependency);
        }
    }

    pub fn node(&self, id: NodeId) -> &Node {
        &self.nodes[id.index()]
    }

    /// Whether `node` never is the empty value. `arguments` are the slots
    /// of the compiled function's arguments, which always hold values.
    pub fn is_known_non_empty(&self, node: NodeId, arguments: core::ops::Range<u32>) -> bool {
        let mut pending = vec![node];
        let mut seen = Vec::new();
        while let Some(node) = pending.pop() {
            if seen.contains(&node) {
                continue;
            }
            seen.push(node);
            let data = self.node(node);
            if data.repr.is_some_and(|repr| repr != Repr::Tagged) {
                continue;
            }
            match &data.op {
                Op::Constant(bits) if *bits != value::EMPTY => {}
                Op::LoadSlot { slot } if arguments.contains(&crate::bytecode::Operand::from_raw(*slot).raw()) => {}
                // NB: Calls return values, never the empty value.
                Op::CallDirect { .. } | Op::CallNative { .. } => {}
                Op::ArgumentCount
                | Op::LoadArgument { .. }
                | Op::IsCallable
                | Op::EmptyToUndefined
                | Op::Typeof
                | Op::AllocateObject { .. }
                | Op::AllocateArray { .. }
                | Op::AllocateFunction { .. }
                | Op::AllocateEnvironment { .. }
                | Op::SliceArguments
                | Op::BoxInt32
                | Op::BoxBool
                | Op::BoxFloat64 => {}
                Op::Phi => pending.extend(data.inputs.iter().copied()),
                _ => return false,
            }
        }
        true
    }

    pub fn block(&self, id: BlockId) -> &Block {
        &self.blocks[id.index()]
    }

    pub fn frame_state(&self, id: FrameStateId) -> &FrameState {
        &self.frame_states[id.index()]
    }

    /// A frame state and its callers' frame states, innermost first.
    pub fn frame_state_chain(&self, id: FrameStateId) -> Vec<FrameStateId> {
        let mut chain = vec![id];
        while let Some(parent) = self.frame_state(*chain.last().expect("the chain is not empty")).parent {
            chain.push(parent);
        }
        chain
    }

    /// The values of every frame of a frame state chain, innermost frame first.
    ///
    /// They are followed by the properties of the virtual objects among them
    /// (see `virtual_objects()`), in order, with the slot
    /// `VIRTUAL_OBJECT_PROPERTY`: exits need those values too.
    /// The values `node` uses: its inputs, then the values of its frame
    /// state.
    pub fn used_values(&self, node: NodeId) -> impl Iterator<Item = NodeId> + '_ {
        let data = self.node(node);
        let frame_state_values = data
            .frame_state
            .map(|frame_state| self.frame_state_values(frame_state))
            .unwrap_or_default();
        data.inputs
            .iter()
            .copied()
            .chain(frame_state_values.into_iter().map(|(_, value)| value))
    }

    pub fn frame_state_values(&self, id: FrameStateId) -> Vec<(u32, NodeId)> {
        let mut values = self.chain_values(id);
        for object in self.virtual_objects_among(&values) {
            values.extend(
                self.node(object)
                    .inputs
                    .iter()
                    .map(|property| (VIRTUAL_OBJECT_PROPERTY, *property)),
            );
        }
        values
    }

    /// The virtual objects the values of a frame state chain refer to,
    /// directly or through other virtual objects, each once, in the order
    /// they are first referred to.
    pub fn virtual_objects(&self, id: FrameStateId) -> Vec<NodeId> {
        self.virtual_objects_among(&self.chain_values(id))
    }

    fn chain_values(&self, id: FrameStateId) -> Vec<(u32, NodeId)> {
        let mut values = Vec::new();
        let mut frame_state = Some(id);
        while let Some(current) = frame_state {
            let data = self.frame_state(current);
            values.extend_from_slice(&data.values);
            frame_state = data.parent;
        }
        values
    }

    fn virtual_objects_among(&self, values: &[(u32, NodeId)]) -> Vec<NodeId> {
        let is_virtual_object = |value: NodeId| matches!(self.node(value).op, Op::VirtualObject { .. });
        // NB: Most frame states refer to no virtual object.
        if !values.iter().any(|(_, value)| is_virtual_object(*value)) {
            return Vec::new();
        }
        let mut objects = Vec::new();
        let mut pending = values.iter().rev().map(|(_, value)| *value).collect::<Vec<_>>();
        while let Some(value) = pending.pop() {
            if !is_virtual_object(value) || objects.contains(&value) {
                continue;
            }
            objects.push(value);
            pending.extend(self.node(value).inputs.iter().rev().copied());
        }
        objects
    }

    /// The blocks code starts at: block 0, and the on-stack replacement
    /// entries.
    pub fn entries(&self) -> impl Iterator<Item = BlockId> + '_ {
        std::iter::once(BlockId(0)).chain(self.osr_entries.iter().map(|(_, entry)| *entry))
    }

    /// The control node ending `block`, which every block has once built.
    pub fn control_id(&self, block: BlockId) -> NodeId {
        self.block(block).control.expect("finished blocks have a control node")
    }

    pub fn control(&self, block: BlockId) -> &Node {
        self.node(self.control_id(block))
    }

    pub fn successors(&self, block: BlockId) -> Successors {
        self.control(block).op.successors()
    }

    pub fn add_node(&mut self, node: Node) -> NodeId {
        let id = NodeId::from_index(self.nodes.len());
        self.nodes.push(node);
        id
    }

    pub fn add_block(&mut self, block: Block) -> BlockId {
        let id = BlockId::from_index(self.blocks.len());
        self.blocks.push(block);
        id
    }

    pub fn add_frame_state(&mut self, frame_state: FrameState) -> FrameStateId {
        let id = FrameStateId(u32::try_from(self.frame_states.len()).expect("frame state count fits in u32"));
        self.frame_states.push(frame_state);
        id
    }

    /// The nodes of a block in execution order: phis, body, control.
    pub fn block_nodes(&self, block: BlockId) -> impl Iterator<Item = NodeId> + '_ {
        self.block(block).nodes()
    }

    /// Makes the edges from `block` to `from` lead to `to` instead.
    pub fn replace_successor(&mut self, block: BlockId, from: BlockId, to: BlockId) {
        let control = self.blocks[block.index()]
            .control
            .expect("finished blocks have a control node");
        for successor in self.nodes[control.index()].op.successors_mut() {
            if *successor == from {
                *successor = to;
            }
        }
    }

    /// Reorders the blocks so that `order[i]` becomes block `i`. Blocks not
    /// in `order` are dropped; nothing may refer to them.
    pub fn reorder_blocks(&mut self, order: &[BlockId]) {
        let mut new_index = vec![None; self.blocks.len()];
        for (index, block) in order.iter().enumerate() {
            new_index[block.index()] = Some(BlockId::from_index(index));
        }
        let remap = |block: &mut BlockId| {
            *block = new_index[block.index()].expect("reordering keeps every referenced block");
        };
        let mut old_blocks = std::mem::take(&mut self.blocks)
            .into_iter()
            .map(Some)
            .collect::<Vec<_>>();
        self.blocks = order
            .iter()
            .map(|block| {
                old_blocks[block.index()]
                    .take()
                    .expect("blocks appear once in the order")
            })
            .collect();
        for block in &mut self.blocks {
            block.predecessors.iter_mut().for_each(remap);
            if let Some(control) = block.control {
                for successor in self.nodes[control.index()].op.successors_mut() {
                    remap(successor);
                }
            }
        }
        for (_, block) in &mut self.osr_entries {
            remap(block);
        }
    }

    /// The bits of `id` if it is a constant, or a refinement of one.
    pub fn constant_value(&self, id: NodeId) -> Option<u64> {
        match self.node(self.unrefined(id)).op {
            Op::Constant(bits) => Some(bits),
            _ => None,
        }
    }

    /// The value `id` refines, through any number of refinements (see
    /// `Op::refined_input()`), or `id` itself.
    pub fn unrefined(&self, mut id: NodeId) -> NodeId {
        while let Some(index) = self.node(id).op.refined_input() {
            id = self.node(id).inputs[index];
        }
        id
    }
}
