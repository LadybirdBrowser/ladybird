/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Interpreter feedback consumed by the optimizing JIT, as the profiling interpreter records it. Every instruction
//! that collects feedback carries one slot index per feedback kind it records, and each kind has its own array, which
//! `ExecutableFeedbackHead` points the interpreter at.

use core::cell::Cell;

use super::cell::{CellHeader, Gc};

/// Where the interpreter finds an executable's feedback arrays, as offsets into the primitive storage cage, which the
/// interpreter masks every address it forms from them into. The executable owns the arrays.
#[repr(C)]
pub struct ExecutableFeedbackHead {
    pub arith: Cell<u64>,
    pub value_buckets: Cell<u64>,
    pub call: Cell<u64>,
    pub keyed: Cell<u64>,
}

impl ExecutableFeedbackHead {
    pub const fn empty() -> Self {
        Self {
            arith: Cell::new(0),
            value_buckets: Cell::new(0),
            call: Cell::new(0),
            keyed: Cell::new(0),
        }
    }
}

/// The union of the operand kinds seen by an arithmetic, comparison or unary operation, as `ArithFeedback` bits.
/// `INT32_OVERFLOW` means that the operation produced a result that is not an int32 (overflow, negative zero or a
/// fraction) on a path that int32 operands take.
pub mod arith_feedback {
    pub const NONE: u8 = 0;
    pub const INT32: u8 = 1 << 0;
    pub const DOUBLE: u8 = 1 << 1;
    pub const INT32_OVERFLOW: u8 = 1 << 2;
    pub const STRING: u8 = 1 << 3;
    pub const BIG_INT: u8 = 1 << 4;
    pub const OTHER: u8 = 1 << 5;
}

/// The union of the kinds of values produced by an instruction, as `ValueFeedback` bits. The bit for a boxed value is
/// 1 << (low three tag bits | 8 if the tag is a cell tag), and unboxed doubles use bit 0. The bits that do not name a
/// kind below (reserved tags, accessors and other non-value cells) count as `OTHER`.
///
/// The interpreter does not classify values as it runs: it stores the bits of the last value each instruction
/// produced in that instruction's bucket, and the buckets are folded into these bits when the feedback is about to be
/// used and at every garbage collection.
pub mod value_feedback {
    pub const NONE: u16 = 0;
    pub const DOUBLE: u16 = 1 << 0;
    pub const BOOLEAN: u16 = 1 << 1;
    pub const INT32: u16 = 1 << 2;
    pub const EMPTY: u16 = 1 << 3;
    pub const UNDEFINED: u16 = 1 << 6;
    pub const NULL: u16 = 1 << 7;
    pub const OBJECT: u16 = 1 << 9;
    pub const STRING: u16 = 1 << 10;
    pub const SYMBOL: u16 = 1 << 11;
    pub const BIG_INT: u16 = 1 << 13;
    pub const OTHER: u16 = 1 << 15;

    pub const OTHER_MASK: u16 = (1 << 4) | (1 << 5) | (1 << 8) | (1 << 12) | (1 << 14) | (1 << 15);

    /// A value bucket holds the raw bits of a Value. Only its tag is ever looked at, so it is not a GC reference. An
    /// empty bucket has a reserved tag that no value uses.
    pub const EMPTY_VALUE_BUCKET: u64 = 0x7ffc << 48;

    /// The bit for a value with the raw `bits`.
    pub const fn for_bits(bits: u64) -> u16 {
        // NB: Boxed values have all of the 0x7ff8 tag bits set.
        let tag = bits >> 48;
        let index = if tag & 0x7ff8 == 0x7ff8 {
            (tag & 7) | ((tag >> 12) & 8)
        } else {
            0
        };
        1 << index
    }
}

/// The callees seen by a call site.
#[repr(C)]
pub struct CallFeedback {
    /// The first callee seen at this site. Weak: cleared when the callee dies.
    pub target: Cell<Option<Gc<CellHeader>>>,
    /// `call_feedback_flags` bits.
    pub flags: Cell<u8>,
    /// How the first callee forwarded the call to another function, a `CallFeedbackForwarding`.
    pub forwarding: Cell<u8>,
    /// The number of arguments the first forwarded call passed.
    pub forwarded_argument_count: Cell<u16>,
    /// The function the first callee forwarded the first call through it to. Weak: cleared when it dies.
    pub forwarded_target: Cell<Option<Gc<CellHeader>>>,
}

pub mod call_feedback_flags {
    pub const NONE: u8 = 0;
    pub const POLYMORPHIC: u8 = 1 << 0;
    /// A callee that is not an ECMAScript function object was seen.
    pub const SAW_NATIVE: u8 = 1 << 1;
    pub const SAW_CONSTRUCT: u8 = 1 << 2;
    /// The calls through the first callee did not all forward to the same function in the same way with the same
    /// number of arguments.
    pub const FORWARDED_POLYMORPHIC: u8 = 1 << 3;
    /// The calls through the first callee forwarded the same way, with the same number of arguments, to other
    /// closures of the ECMAScript function the first forwarded call went to: functions with its code, but their own
    /// environments.
    pub const FORWARDED_CLOSURES: u8 = 1 << 4;
    /// A callee other than the first was seen that is no closure of the first's ECMAScript function: another
    /// function, or a native one. Without it, every callee of a polymorphic site was a closure of the first callee's
    /// function: a function with its code, but its own environment.
    pub const OTHER_FUNCTIONS: u8 = 1 << 5;
}

/// How the first callee of a call site forwarded the call to another function.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CallFeedbackForwarding {
    None,
    /// Function.prototype.apply with an array-like argument list.
    Apply,
    /// Function.prototype.call.
    Call,
    /// A bound function.
    Bound,
    /// A builtin written in JavaScript that calls its first argument back, like Array.prototype.forEach.
    Callback,
}

impl Default for CallFeedback {
    fn default() -> Self {
        Self {
            target: Cell::new(None),
            flags: Cell::new(call_feedback_flags::NONE),
            forwarding: Cell::new(CallFeedbackForwarding::None as u8),
            forwarded_argument_count: Cell::new(0),
            forwarded_target: Cell::new(None),
        }
    }
}

/// The keys and receivers seen by a keyed property access.
#[repr(C)]
pub struct KeyedFeedback {
    /// `keyed_feedback_bits` bits.
    pub bits: Cell<u32>,
    /// The last string or symbol key seen at this site. Weak: cleared when the key dies.
    pub last_key: Cell<Option<Gc<CellHeader>>>,
}

pub mod keyed_feedback_bits {
    pub const NONE: u32 = 0;

    // Key kinds.
    pub const INT32_INDEX: u32 = 1 << 0;
    pub const STRING_KEY: u32 = 1 << 1;
    pub const SYMBOL_KEY: u32 = 1 << 2;
    pub const OTHER_KEY: u32 = 1 << 3;
    /// More than one distinct string or symbol key was seen.
    pub const MULTIPLE_KEYS: u32 = 1 << 4;

    // Element kinds.
    pub const PACKED: u32 = 1 << 5;
    pub const HOLEY: u32 = 1 << 6;
    pub const OTHER_ELEMENTS: u32 = 1 << 7;
    /// An int32 index that was not that of an element the object's indexed storage holds: beyond its size or its
    /// storage, or a hole.
    pub const OUT_OF_BOUNDS: u32 = 1 << 8;

    // One bit per typed array kind, starting at TYPED_ARRAY_SHIFT.
    pub const KEY_KINDS_MASK: u32 = 0x1f;
    pub const TYPED_ARRAY_SHIFT: u32 = 9;

    pub const fn typed_array_bit(kind: u8) -> u32 {
        1 << (TYPED_ARRAY_SHIFT + kind as u32)
    }
}

impl Default for KeyedFeedback {
    fn default() -> Self {
        Self {
            bits: Cell::new(keyed_feedback_bits::NONE),
            last_key: Cell::new(None),
        }
    }
}
