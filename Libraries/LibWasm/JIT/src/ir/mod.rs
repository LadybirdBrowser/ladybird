/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The IR: typed SSA in flat arenas.
//!
//! A `Func` is a list of blocks in linear order, which is the order of the WebAssembly code: every forward edge goes
//! to a later block, and every loop is a contiguous range of blocks that only its header is entered at. Nodes live in
//! one arena and are named by their index (`Value`). Each block owns a contiguous range of nodes, its body, which
//! ends with its terminator, and a list of phis. Nodes are created in order, so a body node's index is also its
//! position in linear order.
//!
//! A `Node` is 16 bytes: an op, a type, eight bits of flags and three 32-bit arguments, which are inputs or immediates
//! as the op says (`Op::shape()`). Ops with more inputs keep them in `Func::operands`.
//!
//! Constants, parameters, the instance and the memory base are values without code at their position: constants are
//! rematerialized at every use, and the others arrive in registers or slots at the entry.

pub mod dump;
pub mod verify;

/// A node, by its index in `Func::nodes`.
pub type Value = u32;

/// A block, by its index in `Func::blocks`.
pub type BlockId = u32;

/// No value or block.
pub const NONE: u32 = u32::MAX;

/// The type of a value.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
#[repr(u8)]
pub enum Type {
    #[default]
    None,
    I32,
    I64,
    F32,
    F64,
    V128,
    /// A reference: zero for null, else what the runtime encodes it as (a `FunctionHeader` for funcref).
    Ref,
    /// A machine pointer, for addresses the code computes (the memory base, instance data).
    Pointer,
}

impl Type {
    pub fn is_float(self) -> bool {
        matches!(self, Type::F32 | Type::F64)
    }

    /// Whether values of this type live in xmm registers: floats and vectors.
    pub fn uses_xmm(self) -> bool {
        matches!(self, Type::F32 | Type::F64 | Type::V128)
    }

    /// The 8-byte slots a value of this type takes in memory.
    pub fn slots(self) -> u32 {
        if self == Type::V128 { 2 } else { 1 }
    }

    pub fn is_64(self) -> bool {
        matches!(self, Type::I64 | Type::F64 | Type::Ref | Type::Pointer)
    }

    pub fn name(self) -> &'static str {
        match self {
            Type::None => "none",
            Type::I32 => "i32",
            Type::I64 => "i64",
            Type::F32 => "f32",
            Type::F64 => "f64",
            Type::V128 => "v128",
            Type::Ref => "ref",
            Type::Pointer => "ptr",
        }
    }
}

/// Where an op finds its inputs.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Shape {
    /// The first `n` arguments are inputs; the others are immediates.
    Fixed(u8),
    /// `args[0]` is the start and `args[1]` the count of the inputs in `Func::operands`.
    Variadic,
}

/// What an op may do besides computing its value.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Effect {
    /// Nothing: equal nodes compute equal values anywhere their inputs are available, and unused ones go away.
    Pure,
    /// Reads state that stores and calls may change; may trap where it reads memory.
    Read,
    /// Writes state, or traps where its inputs say so (division, truncation): never removed or moved.
    Write,
    /// Ends a block.
    Terminator,
}

/// An integer comparison, in `Node::flags` of `Compare` and `BranchCompare`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum IntCondition {
    Eq,
    Ne,
    LtS,
    LtU,
    GtS,
    GtU,
    LeS,
    LeU,
    GeS,
    GeU,
}

impl IntCondition {
    pub const ALL: [IntCondition; 10] = [
        IntCondition::Eq,
        IntCondition::Ne,
        IntCondition::LtS,
        IntCondition::LtU,
        IntCondition::GtS,
        IntCondition::GtU,
        IntCondition::LeS,
        IntCondition::LeU,
        IntCondition::GeS,
        IntCondition::GeU,
    ];

    pub fn from_flags(flags: u8) -> Self {
        Self::ALL[flags as usize]
    }

    pub fn invert(self) -> Self {
        match self {
            IntCondition::Eq => IntCondition::Ne,
            IntCondition::Ne => IntCondition::Eq,
            IntCondition::LtS => IntCondition::GeS,
            IntCondition::LtU => IntCondition::GeU,
            IntCondition::GtS => IntCondition::LeS,
            IntCondition::GtU => IntCondition::LeU,
            IntCondition::LeS => IntCondition::GtS,
            IntCondition::LeU => IntCondition::GtU,
            IntCondition::GeS => IntCondition::LtS,
            IntCondition::GeU => IntCondition::LtU,
        }
    }

    /// The condition with the operands swapped.
    pub fn swap(self) -> Self {
        match self {
            IntCondition::Eq => IntCondition::Eq,
            IntCondition::Ne => IntCondition::Ne,
            IntCondition::LtS => IntCondition::GtS,
            IntCondition::LtU => IntCondition::GtU,
            IntCondition::GtS => IntCondition::LtS,
            IntCondition::GtU => IntCondition::LtU,
            IntCondition::LeS => IntCondition::GeS,
            IntCondition::LeU => IntCondition::GeU,
            IntCondition::GeS => IntCondition::LeS,
            IntCondition::GeU => IntCondition::LeU,
        }
    }

    pub fn evaluate(self, lhs: u64, rhs: u64, is_64: bool) -> bool {
        let (signed_lhs, signed_rhs) = if is_64 {
            (lhs as i64, rhs as i64)
        } else {
            (i64::from(lhs as u32 as i32), i64::from(rhs as u32 as i32))
        };
        let (lhs, rhs) = if is_64 {
            (lhs, rhs)
        } else {
            (u64::from(lhs as u32), u64::from(rhs as u32))
        };
        match self {
            IntCondition::Eq => lhs == rhs,
            IntCondition::Ne => lhs != rhs,
            IntCondition::LtS => signed_lhs < signed_rhs,
            IntCondition::LtU => lhs < rhs,
            IntCondition::GtS => signed_lhs > signed_rhs,
            IntCondition::GtU => lhs > rhs,
            IntCondition::LeS => signed_lhs <= signed_rhs,
            IntCondition::LeU => lhs <= rhs,
            IntCondition::GeS => signed_lhs >= signed_rhs,
            IntCondition::GeU => lhs >= rhs,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            IntCondition::Eq => "eq",
            IntCondition::Ne => "ne",
            IntCondition::LtS => "lt_s",
            IntCondition::LtU => "lt_u",
            IntCondition::GtS => "gt_s",
            IntCondition::GtU => "gt_u",
            IntCondition::LeS => "le_s",
            IntCondition::LeU => "le_u",
            IntCondition::GeS => "ge_s",
            IntCondition::GeU => "ge_u",
        }
    }
}

/// A float comparison, in `Node::flags` of `FloatCompare` and `BranchFloatCompare`. All are false for NaN but `Ne`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum FloatCondition {
    Eq,
    Ne,
    Lt,
    Gt,
    Le,
    Ge,
}

impl FloatCondition {
    pub const ALL: [FloatCondition; 6] = [
        FloatCondition::Eq,
        FloatCondition::Ne,
        FloatCondition::Lt,
        FloatCondition::Gt,
        FloatCondition::Le,
        FloatCondition::Ge,
    ];

    pub fn from_flags(flags: u8) -> Self {
        Self::ALL[flags as usize]
    }

    pub fn name(self) -> &'static str {
        match self {
            FloatCondition::Eq => "eq",
            FloatCondition::Ne => "ne",
            FloatCondition::Lt => "lt",
            FloatCondition::Gt => "gt",
            FloatCondition::Le => "le",
            FloatCondition::Ge => "ge",
        }
    }
}

/// In `Node::flags` of a `Load`, besides its `LoadKind`: the load of an atomic access, which has to be ordered with the
/// others where plain loads are not (on AArch64).
pub const LOAD_ATOMIC: u8 = 0x80;

/// How a load reads memory, in `Node::flags` of `Load`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum LoadKind {
    I32,
    I64,
    F32,
    F64,
    I32S8,
    I32U8,
    I32S16,
    I32U16,
    I64S8,
    I64U8,
    I64S16,
    I64U16,
    I64S32,
    I64U32,
}

impl LoadKind {
    pub const ALL: [LoadKind; 14] = [
        LoadKind::I32,
        LoadKind::I64,
        LoadKind::F32,
        LoadKind::F64,
        LoadKind::I32S8,
        LoadKind::I32U8,
        LoadKind::I32S16,
        LoadKind::I32U16,
        LoadKind::I64S8,
        LoadKind::I64U8,
        LoadKind::I64S16,
        LoadKind::I64U16,
        LoadKind::I64S32,
        LoadKind::I64U32,
    ];

    pub fn from_flags(flags: u8) -> Self {
        Self::ALL[(flags & !LOAD_ATOMIC) as usize]
    }

    pub fn size(self) -> u32 {
        match self {
            LoadKind::I32S8 | LoadKind::I32U8 | LoadKind::I64S8 | LoadKind::I64U8 => 1,
            LoadKind::I32S16 | LoadKind::I32U16 | LoadKind::I64S16 | LoadKind::I64U16 => 2,
            LoadKind::I32 | LoadKind::F32 | LoadKind::I64S32 | LoadKind::I64U32 => 4,
            LoadKind::I64 | LoadKind::F64 => 8,
        }
    }

    pub fn name(self) -> &'static str {
        match self {
            LoadKind::I32 => "i32",
            LoadKind::I64 => "i64",
            LoadKind::F32 => "f32",
            LoadKind::F64 => "f64",
            LoadKind::I32S8 => "i32.s8",
            LoadKind::I32U8 => "i32.u8",
            LoadKind::I32S16 => "i32.s16",
            LoadKind::I32U16 => "i32.u16",
            LoadKind::I64S8 => "i64.s8",
            LoadKind::I64U8 => "i64.u8",
            LoadKind::I64S16 => "i64.s16",
            LoadKind::I64U16 => "i64.u16",
            LoadKind::I64S32 => "i64.s32",
            LoadKind::I64U32 => "i64.u32",
        }
    }
}

/// The runtime functions `Helper` calls, and their operands.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum Helper {
    /// memory data, delta in pages: the previous size, or u64::MAX.
    MemoryGrow,
    /// table data, delta, initial element: the previous size, or u64::MAX.
    TableGrow,
    /// memory data, destination, value, count.
    MemoryFill,
    /// destination memory data, source memory data, destination, source, count.
    MemoryCopy,
    /// memory data, segment, destination, source, count.
    MemoryInit,
    /// table data, destination, value, count.
    TableFill,
    /// destination table data, source table data, destination, source, count.
    TableCopy,
    /// table data, segment, destination, source, count.
    TableInit,
    /// address, expected value, timeout in nanoseconds, size: 0 when woken, 1 if the value differed, 2 on timeout,
    /// or WAIT_CANNOT_BLOCK when the runtime has the trap.
    AtomicWait,
    /// address, count: how many waiters it woke.
    AtomicNotify,
    /// rtt, data size in bytes, array length: the new zeroed GC object, or zero (when the runtime has the trap).
    Allocate,
    /// array, start, the low and high 64 bits of the value, count.
    ArrayFill,
    /// destination array, destination start, source array, source start, count.
    ArrayCopy,
    /// array, data segment, destination, source, count.
    ArrayInitData,
    /// array, element segment, destination, source, count.
    ArrayInitElem,
    /// segment, start, count, bytes per item, TRAP_* code: traps with the code unless the items are in the segment.
    SegmentRange,
    /// tag identity: a new exception with that tag and a zeroed payload, for the code to fill in.
    AllocateException,
}

impl Helper {
    pub const ALL: [Helper; 17] = [
        Helper::MemoryGrow,
        Helper::TableGrow,
        Helper::MemoryFill,
        Helper::MemoryCopy,
        Helper::MemoryInit,
        Helper::TableFill,
        Helper::TableCopy,
        Helper::TableInit,
        Helper::AtomicWait,
        Helper::AtomicNotify,
        Helper::Allocate,
        Helper::ArrayFill,
        Helper::ArrayCopy,
        Helper::ArrayInitData,
        Helper::ArrayInitElem,
        Helper::SegmentRange,
        Helper::AllocateException,
    ];

    pub fn from_index(index: u32) -> Self {
        Self::ALL[index as usize]
    }

    /// Whether the helper returns 0 or the TRAP_* code to trap with, rather than a value.
    pub fn may_trap(self) -> bool {
        !matches!(
            self,
            Helper::MemoryGrow
                | Helper::TableGrow
                | Helper::AtomicWait
                | Helper::AtomicNotify
                | Helper::Allocate
                | Helper::AllocateException
        )
    }

    /// Whether a value of zero means the runtime trapped, with TRAP_RUNTIME.
    pub fn traps_on_zero(self) -> bool {
        self == Helper::Allocate
    }

    /// The value, besides zero for `traps_on_zero()`, that means the runtime trapped, with TRAP_RUNTIME.
    pub fn trap_value(self) -> Option<u64> {
        (self == Helper::AtomicWait).then_some(libwasm_module::abi::WAIT_CANNOT_BLOCK)
    }

    pub fn name(self) -> &'static str {
        match self {
            Helper::MemoryGrow => "memory_grow",
            Helper::TableGrow => "table_grow",
            Helper::MemoryFill => "memory_fill",
            Helper::MemoryCopy => "memory_copy",
            Helper::MemoryInit => "memory_init",
            Helper::TableFill => "table_fill",
            Helper::TableCopy => "table_copy",
            Helper::TableInit => "table_init",
            Helper::AtomicWait => "atomic_wait",
            Helper::AtomicNotify => "atomic_notify",
            Helper::Allocate => "allocate",
            Helper::ArrayFill => "array_fill",
            Helper::ArrayCopy => "array_copy",
            Helper::ArrayInitData => "array_init_data",
            Helper::ArrayInitElem => "array_init_elem",
            Helper::SegmentRange => "segment_range",
            Helper::AllocateException => "allocate_exception",
        }
    }
}

/// How many bytes a store writes, in `Node::flags` of `Store`. The value's type says the rest.
pub fn store_size(flags: u8) -> u32 {
    1 << flags
}

/// `Node::flags` bit of ops that may trap (division, truncation) once their inputs show they cannot.
pub const FLAG_CANNOT_TRAP: u8 = 0x80;

/// `Node::flags` bit of `GlobalGet` for globals no code can change.
pub const FLAG_IMMUTABLE: u8 = 0x40;

/// `Node::flags` bit of `CalleeInstance` for a header that is never null, like an imported function's.
pub const FLAG_NOT_NULL: u8 = 0x40;

/// `Node::flags` bits of `CheckedAddress`, besides log2 of the access size in the low bits: an atomic access, which
/// must be aligned, and an access of a memory64, whose index and offset may add up past 2^64.
pub const CHECKED_ATOMIC: u8 = 0x08;
pub const CHECKED_MEMORY64: u8 = 0x10;

/// The read-modify-write operations of `AtomicRmw`, in its args[2].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u32)]
pub enum AtomicOp {
    Add,
    Sub,
    And,
    Or,
    Xor,
    Exchange,
}

impl AtomicOp {
    pub const ALL: [AtomicOp; 6] = [
        AtomicOp::Add,
        AtomicOp::Sub,
        AtomicOp::And,
        AtomicOp::Or,
        AtomicOp::Xor,
        AtomicOp::Exchange,
    ];

    pub fn from_index(index: u32) -> Self {
        Self::ALL[index as usize]
    }

    pub fn name(self) -> &'static str {
        match self {
            AtomicOp::Add => "add",
            AtomicOp::Sub => "sub",
            AtomicOp::And => "and",
            AtomicOp::Or => "or",
            AtomicOp::Xor => "xor",
            AtomicOp::Exchange => "xchg",
        }
    }
}

/// What `RefTest` checks a reference against, in the low bits of its flags.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
pub enum HeapTest {
    /// Every reference but null: any, func, extern and exn.
    NonNull,
    /// An i31 or a GC object: eq.
    Eq,
    I31,
    /// A GC object of a struct type.
    Struct,
    /// A GC object of an array type.
    Array,
    /// Only null: none, nofunc, noextern and noexn.
    Nothing,
    /// A GC object whose type is the identity input or one of its subtypes, which are args[2] deep.
    ConcreteObject,
    /// A function whose type is the identity input or one of its subtypes, which are args[2] deep.
    ConcreteFunction,
}

impl HeapTest {
    pub const ALL: [HeapTest; 8] = [
        HeapTest::NonNull,
        HeapTest::Eq,
        HeapTest::I31,
        HeapTest::Struct,
        HeapTest::Array,
        HeapTest::Nothing,
        HeapTest::ConcreteObject,
        HeapTest::ConcreteFunction,
    ];

    pub fn from_flags(flags: u8) -> Self {
        Self::ALL[(flags & 0xf) as usize]
    }

    pub fn name(self) -> &'static str {
        match self {
            HeapTest::NonNull => "non_null",
            HeapTest::Eq => "eq",
            HeapTest::I31 => "i31",
            HeapTest::Struct => "struct",
            HeapTest::Array => "array",
            HeapTest::Nothing => "nothing",
            HeapTest::ConcreteObject => "object",
            HeapTest::ConcreteFunction => "function",
        }
    }
}

/// `Node::flags` bits of `RefTest`, besides the `HeapTest`: null matches too; and the test is a cast, which traps
/// where the reference does not match and has no value.
pub const REF_TEST_NULLABLE: u8 = 0x10;
pub const REF_TEST_CAST: u8 = 0x80;
/// `Node::flags` bit of a `Call` in a region of an exception handler, which checks the context's `exception` once
/// it returns (see `builder/exceptions.rs`).
pub const FLAG_CHECKS_EXCEPTION: u8 = 0x01;

macro_rules! ops {
    ($($(#[$meta:meta])* $name:ident: $shape:expr, $effect:ident, $text:literal;)*) => {
        /// The operation of a node. See `ARCHITECTURE.md` for what each one does.
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        #[repr(u16)]
        pub enum Op {
            $($(#[$meta])* $name,)*
        }

        const SHAPES: &[Shape] = &[$($shape,)*];
        const EFFECTS: &[Effect] = &[$(Effect::$effect,)*];

        impl Op {
            #[inline]
            pub fn shape(self) -> Shape {
                SHAPES[self as usize]
            }

            #[inline]
            pub fn base_effect(self) -> Effect {
                EFFECTS[self as usize]
            }

            pub fn name(self) -> &'static str {
                match self {
                    $(Op::$name => $text,)*
                }
            }
        }
    };
}

use Shape::{Fixed, Variadic};

ops! {
    // Values without code at their position.
    Const32: Fixed(0), Pure, "const";
    Const64: Fixed(0), Pure, "const";
    /// args[0]: the index of the constant in `Func::constants`.
    Const128: Fixed(0), Pure, "const";
    Param: Fixed(0), Pure, "param";
    Instance: Fixed(0), Pure, "instance";
    MemoryBase: Fixed(0), Pure, "memory_base";
    Phi: Variadic, Pure, "phi";
    /// A phi that turned out trivial, forwarding to args[0]. Gone after cleanup.
    Forward: Fixed(0), Pure, "forward";

    // Integer arithmetic, on I32 or I64 as the node's type says.
    Add: Fixed(2), Pure, "add";
    Sub: Fixed(2), Pure, "sub";
    Mul: Fixed(2), Pure, "mul";
    And: Fixed(2), Pure, "and";
    Or: Fixed(2), Pure, "or";
    Xor: Fixed(2), Pure, "xor";
    Shl: Fixed(2), Pure, "shl";
    ShrS: Fixed(2), Pure, "shr_s";
    ShrU: Fixed(2), Pure, "shr_u";
    Rotl: Fixed(2), Pure, "rotl";
    Rotr: Fixed(2), Pure, "rotr";
    DivS: Fixed(2), Write, "div_s";
    DivU: Fixed(2), Write, "div_u";
    RemS: Fixed(2), Write, "rem_s";
    RemU: Fixed(2), Write, "rem_u";
    Clz: Fixed(1), Pure, "clz";
    Ctz: Fixed(1), Pure, "ctz";
    Popcnt: Fixed(1), Pure, "popcnt";
    Eqz: Fixed(1), Pure, "eqz";
    Extend8S: Fixed(1), Pure, "extend8_s";
    Extend16S: Fixed(1), Pure, "extend16_s";
    Extend32S: Fixed(1), Pure, "extend32_s";
    ExtendI32S: Fixed(1), Pure, "extend_i32_s";
    ExtendI32U: Fixed(1), Pure, "extend_i32_u";
    Wrap: Fixed(1), Pure, "wrap";
    Compare: Fixed(2), Pure, "compare";

    // Float arithmetic, on F32 or F64.
    FAdd: Fixed(2), Pure, "fadd";
    FSub: Fixed(2), Pure, "fsub";
    FMul: Fixed(2), Pure, "fmul";
    FDiv: Fixed(2), Pure, "fdiv";
    FMin: Fixed(2), Pure, "fmin";
    FMax: Fixed(2), Pure, "fmax";
    FCopysign: Fixed(2), Pure, "fcopysign";
    FAbs: Fixed(1), Pure, "fabs";
    FNeg: Fixed(1), Pure, "fneg";
    FSqrt: Fixed(1), Pure, "fsqrt";
    FCeil: Fixed(1), Pure, "fceil";
    FFloor: Fixed(1), Pure, "ffloor";
    FTrunc: Fixed(1), Pure, "ftrunc";
    FNearest: Fixed(1), Pure, "fnearest";
    FloatCompare: Fixed(2), Pure, "fcompare";

    // Conversions to the node's type from the input's.
    TruncS: Fixed(1), Write, "trunc_s";
    TruncU: Fixed(1), Write, "trunc_u";
    TruncSatS: Fixed(1), Pure, "trunc_sat_s";
    TruncSatU: Fixed(1), Pure, "trunc_sat_u";
    ConvertS: Fixed(1), Pure, "convert_s";
    ConvertU: Fixed(1), Pure, "convert_u";
    Demote: Fixed(1), Pure, "demote";
    Promote: Fixed(1), Pure, "promote";
    Reinterpret: Fixed(1), Pure, "reinterpret";

    /// args: condition, value if nonzero, value if zero.
    Select: Fixed(3), Pure, "select";

    // Vectors. The SIMD sub-opcode says which operation; the node's type is its result's.
    /// args: operand, sub-opcode. Splats, unary lane operations and the reductions to an i32.
    VectorUnary: Fixed(1), Pure, "vector_unary";
    /// args: lhs, rhs, sub-opcode. Binary lane operations, comparisons, and shifts by an i32.
    VectorBinary: Fixed(2), Pure, "vector_binary";
    /// args: three operands; flags: the sub-opcode's low byte (bitselect, or a relaxed operation from 0x100 on).
    VectorTernary: Fixed(3), Pure, "vector_ternary";
    /// args: vector, sub-opcode, lane.
    ExtractLane: Fixed(1), Pure, "extract_lane";
    /// args: vector, value, sub-opcode and lane << 16.
    ReplaceLane: Fixed(2), Pure, "replace_lane";
    /// args: lhs, rhs, the index of the lane indices in `Func::constants`.
    Shuffle: Fixed(2), Pure, "shuffle";
    /// args: base, index, offset; flags: the load's SIMD sub-opcode.
    VectorLoad: Fixed(2), Read, "vector_load";

    // Instance data.
    /// args: instance, byte offset. A pointer field of the instance data that never changes.
    InstanceField: Fixed(1), Pure, "instance_field";
    /// args: pointer, byte offset; flags: 4 for a u32, else a pointer. Data of the instance that never changes, like
    /// `globals[i]` or a type identity.
    LoadPointer: Fixed(1), Pure, "load_pointer";
    /// args: the global's storage, global index.
    GlobalGet: Fixed(1), Read, "global_get";
    /// args: the global's storage, value, global index.
    GlobalSet: Fixed(2), Write, "global_set";

    // Memory 0, a memory32 with guard pages: the access is at base + zero-extended index + offset.
    /// args: base, index, offset; flags: LoadKind.
    Load: Fixed(2), Read, "load";
    /// operands: base, index, value; args[2]: offset; flags: log2 of the size.
    Store: Variadic, Write, "store";

    /// args: pointer, byte offset. The address that far from the pointer.
    Offset: Fixed(1), Pure, "offset";
    /// args: an interpreter frame's fp, byte offset (a signed 32-bit value); flags: 1 for a v128 local, whose slot
    /// has the address of its storage. A local or operand stack value of the frame an OSR compile continues.
    FrameLoad: Fixed(1), Read, "frame_load";
    /// args: pointer, byte offset; flags: the bytes it reads, 4 or 8. A field of runtime data that may change, like
    /// the size of a memory or table, read where it is.
    ReadField: Fixed(1), Read, "read_field";
    /// args: pointer, value, byte offset; flags: the bytes it writes, 4 or 8.
    WriteField: Fixed(2), Write, "write_field";

    // Tables. A table's `TableData` is a pointer of the instance's that never changes.
    /// args: table data, index; flags: the TRAP_* code for an index out of bounds. Element `index`.
    TableElement: Fixed(2), Read, "table_element";
    /// args: table data, index, value; flags: the TRAP_* code for an index out of bounds.
    TableStore: Fixed(3), Write, "table_store";

    // Memories without guard pages, and atomics.
    /// args: memory data, index (i32, or i64 for a memory64), offset (i64); flags: log2 of the access size,
    /// CHECKED_ATOMIC, CHECKED_MEMORY64. The address of the access at index + offset, after checking that it is in
    /// the memory, and for atomics then that it is aligned.
    CheckedAddress: Fixed(3), Read, "checked_address";
    /// args: address, value, the `AtomicOp`; flags: log2 of the size. Applies the op atomically and returns the old
    /// value, zero-extended.
    AtomicRmw: Fixed(2), Write, "atomic_rmw";
    /// args: address, expected value, replacement; flags: log2 of the size. Returns the old value, zero-extended.
    AtomicCompareExchange: Fixed(3), Write, "atomic_cmpxchg";
    /// A sequentially consistent fence.
    AtomicFence: Fixed(0), Write, "atomic_fence";

    // GC objects. A field or element is accessed with `Load`, `VectorLoad` and `Store` at its address, or at the
    // object with the field's offset where the data is inline.
    /// args: instance, the module's type index. The `Rtt` of the type, which never changes.
    Rtt: Fixed(1), Pure, "rtt";
    /// args: i32. The i31 reference of its low 31 bits.
    I31New: Fixed(1), Pure, "ref_i31";
    /// args: i31 reference (not null); flags: 1 to sign-extend. The i32 value.
    I31Get: Fixed(1), Pure, "i31_get";
    /// args: array, index; flags: log2 of the element size. The address of the element, after checking the array for
    /// null and the index against its length.
    ElementAddress: Fixed(2), Read, "element_address";
    /// args: reference, the type identity it is tested against (a constant for abstract heap types), the depth of
    /// that type; flags: `HeapTest`, REF_TEST_NULLABLE, REF_TEST_CAST. Whether the reference matches, as an i32; a
    /// cast has no value and traps instead where it does not.
    RefTest: Fixed(2), Pure, "ref_test";

    // Calls.
    /// args: callee FunctionHeader, the calling instance. The callee's instance: its function header's, or for a host
    /// function, which has none, the caller's, through which its stub finds the runtime; flags: FLAG_NOT_NULL. Pure
    /// only with FLAG_NOT_NULL: a function reference may be null anywhere the null check that guards its call does
    /// not dominate, and a pure node could be hoisted above that check and fault instead of trapping.
    CalleeInstance: Fixed(2), Read, "callee_instance";
    /// args: function reference, the type identity it must have; flags: the TRAP_* code for null. Traps unless the
    /// reference is not null and of that type or a subtype.
    CheckFunction: Fixed(2), Write, "check_function";
    /// args: reference; flags: the TRAP_* code. Traps if the reference is null.
    CheckNull: Fixed(1), Write, "check_null";
    /// operands: callee FunctionHeader, its instance, its memory base, arguments; args[2]: the function index, or
    /// NONE for an indirect call. The value is the first result.
    Call: Variadic, Write, "call";
    /// operands: the instance, then up to six operands; args[2]: the `Helper`. A call of a function of the runtime
    /// that takes its operands in the `ExecutionContext`'s `arguments` and returns a u64.
    Helper: Variadic, Write, "helper";
    /// args: call, result index. A further result of the call right before it.
    CallResult: Fixed(0), Pure, "call_result";

    // Terminators. Successors are in the block (`Func::successors()`).
    Jump: Fixed(0), Terminator, "jump";
    /// args: condition. To the first successor if nonzero, else the second.
    Branch: Fixed(1), Terminator, "branch";
    /// args: lhs, rhs; flags: IntCondition. To the first successor if the comparison holds.
    BranchCompare: Fixed(2), Terminator, "branch_compare";
    /// args: lhs, rhs; flags: FloatCondition.
    BranchFloatCompare: Fixed(2), Terminator, "branch_fcompare";
    /// args: index, start of the targets in `Func::operands`, their count before the default (which comes last).
    /// Each target is the index of a successor of the block.
    BrTable: Fixed(1), Terminator, "br_table";
    /// operands: the results.
    Return: Variadic, Terminator, "return";
    /// args: the TRAP_* code.
    Trap: Fixed(0), Terminator, "trap";
    /// operands and args like `Call`. A call whose results are the function's.
    TailCall: Variadic, Terminator, "tail_call";
    /// args: an exception reference, not null. Throws it to the function's callers: the runtime unwinds it to the
    /// first call that handles exceptions, or out of compiled code.
    Throw: Fixed(1), Terminator, "throw";
}

impl Op {
    pub fn is_terminator(self) -> bool {
        self.base_effect() == Effect::Terminator
    }

    /// Whether nodes of this op never have code where they are.
    pub fn is_entry_value(self) -> bool {
        matches!(
            self,
            Op::Const32 | Op::Const64 | Op::Const128 | Op::Param | Op::Instance | Op::MemoryBase
        )
    }

    pub fn is_constant(self) -> bool {
        matches!(self, Op::Const32 | Op::Const64 | Op::Const128)
    }

    pub fn is_commutative(self) -> bool {
        matches!(
            self,
            Op::Add | Op::Mul | Op::And | Op::Or | Op::Xor | Op::FAdd | Op::FMul
        )
    }
}

/// One node: 16 bytes.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
#[repr(C)]
pub struct Node {
    pub op: Op,
    pub ty: Type,
    pub flags: u8,
    pub args: [u32; 3],
}

const _: () = assert!(size_of::<Node>() == 16);

impl Node {
    pub fn new(op: Op, ty: Type, flags: u8, args: [u32; 3]) -> Self {
        Self { op, ty, flags, args }
    }

    /// What this node may do besides computing its value.
    pub fn effect(&self) -> Effect {
        match self.op {
            Op::GlobalGet if self.flags & FLAG_IMMUTABLE != 0 => Effect::Pure,
            Op::CalleeInstance if self.flags & FLAG_NOT_NULL != 0 => Effect::Pure,
            Op::DivS | Op::DivU | Op::RemS | Op::RemU | Op::TruncS | Op::TruncU
                if self.flags & FLAG_CANNOT_TRAP != 0 =>
            {
                Effect::Pure
            }
            Op::RefTest if self.flags & REF_TEST_CAST != 0 => Effect::Write,
            op => op.base_effect(),
        }
    }

    /// The constant bits of a scalar constant node. Vector constants are in `Func::constants`.
    pub fn constant(&self) -> Option<u64> {
        match self.op {
            Op::Const32 => Some(u64::from(self.args[0])),
            Op::Const64 => Some(u64::from(self.args[0]) | (u64::from(self.args[1]) << 32)),
            _ => None,
        }
    }
}

/// A block: its phis, its body and its edges.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Block {
    /// The body: nodes `start..end`, the last of which is the terminator.
    pub start: u32,
    pub end: u32,
    /// The first phi; each phi's `args[2]` links to the next one.
    pub first_phi: Value,
    pub predecessors_start: u32,
    pub predecessor_count: u32,
    pub successors_start: u32,
    pub successor_count: u32,
    /// The innermost loop this block is in, as an index into `Func::loops`.
    pub loop_index: u32,
}

impl Default for Block {
    fn default() -> Self {
        Self {
            start: 0,
            end: 0,
            first_phi: NONE,
            predecessors_start: 0,
            predecessor_count: 0,
            successors_start: 0,
            successor_count: 0,
            loop_index: NONE,
        }
    }
}

/// A loop: the contiguous blocks `header..=last`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Loop {
    pub header: BlockId,
    pub last: BlockId,
    /// The loop around this one.
    pub parent: u32,
    pub depth: u32,
}

/// A function in the IR.
#[derive(Debug, Default)]
pub struct Func {
    pub blocks: Vec<Block>,
    pub nodes: Vec<Node>,
    /// The inputs of variadic nodes, and the targets of `BrTable`s.
    pub operands: Vec<u32>,
    /// Vector constants and shuffles' lane indices.
    pub constants: Vec<u128>,
    pub predecessors: Vec<BlockId>,
    pub successors: Vec<BlockId>,
    pub loops: Vec<Loop>,
    /// The type of each parameter and result.
    pub params: Vec<Type>,
    pub results: Vec<Type>,
}

impl Func {
    pub fn clear(&mut self) {
        self.blocks.clear();
        self.nodes.clear();
        self.operands.clear();
        self.constants.clear();
        self.predecessors.clear();
        self.successors.clear();
        self.loops.clear();
        self.params.clear();
        self.results.clear();
    }

    pub fn node(&self, value: Value) -> &Node {
        &self.nodes[value as usize]
    }

    /// The inputs of a node.
    #[inline]
    pub fn inputs(&self, value: Value) -> &[u32] {
        let node = &self.nodes[value as usize];
        match node.op.shape() {
            Shape::Fixed(count) => &node.args[..count as usize],
            Shape::Variadic => {
                let start = node.args[0] as usize;
                &self.operands[start..start + node.args[1] as usize]
            }
        }
    }

    /// The value a forwarded phi stands for, or the value itself.
    pub fn resolve(&self, mut value: Value) -> Value {
        while self.nodes[value as usize].op == Op::Forward {
            value = self.nodes[value as usize].args[0];
        }
        value
    }

    /// The value a phi always has, if its inputs are only itself and one other value.
    pub fn trivial_phi_input(&self, phi: Value) -> Option<Value> {
        let mut same = NONE;
        for &input in self.inputs(phi) {
            let input = self.resolve(input);
            if input == phi || input == same {
                continue;
            }
            if same != NONE {
                return None;
            }
            same = input;
        }
        (same != NONE).then_some(same)
    }

    /// Makes `phi` stand for `value` from now on.
    pub fn forward(&mut self, phi: Value, value: Value) {
        let node = &mut self.nodes[phi as usize];
        node.op = Op::Forward;
        node.args[0] = value;
        node.args[1] = 0;
    }

    pub fn predecessors(&self, block: BlockId) -> &[BlockId] {
        let block = &self.blocks[block as usize];
        let start = block.predecessors_start as usize;
        &self.predecessors[start..start + block.predecessor_count as usize]
    }

    pub fn successors(&self, block: BlockId) -> &[BlockId] {
        let block = &self.blocks[block as usize];
        let start = block.successors_start as usize;
        &self.successors[start..start + block.successor_count as usize]
    }

    pub fn terminator(&self, block: BlockId) -> Value {
        self.blocks[block as usize].end - 1
    }

    /// The phis of a block, forwarded ones included.
    pub fn phis(&self, block: BlockId) -> PhiIter<'_> {
        PhiIter {
            func: self,
            next: self.blocks[block as usize].first_phi,
        }
    }

    /// The targets of a `BrTable`, the default last, as indices into the block's successors.
    pub fn table_targets(&self, value: Value) -> &[BlockId] {
        let node = &self.nodes[value as usize];
        debug_assert_eq!(node.op, Op::BrTable);
        let start = node.args[1] as usize;
        &self.operands[start..start + node.args[2] as usize + 1]
    }

    /// Whether `block` is the header of a loop.
    pub fn is_loop_header(&self, block: BlockId) -> bool {
        let loop_index = self.blocks[block as usize].loop_index;
        loop_index != NONE && self.loops[loop_index as usize].header == block
    }
}

pub struct PhiIter<'a> {
    func: &'a Func,
    next: Value,
}

impl Iterator for PhiIter<'_> {
    type Item = Value;

    fn next(&mut self) -> Option<Value> {
        if self.next == NONE {
            return None;
        }
        let phi = self.next;
        self.next = self.func.nodes[phi as usize].args[2];
        Some(phi)
    }
}
