/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Values that bytecode, the interpreter, the runtime and LibJS's C++ API have to agree on. Where a header of LibJS or
//! LibGC has the same definition, the documentation names it.

#![no_std]

/// The NaN-boxed encoding of a JS::Value, from LibGC/NanBoxedValue.h and LibJS/Runtime/Value.h.
pub mod value {
    pub const CANON_NAN_BITS: u64 = 0x7FF8_0000_0000_0000;
    pub const POSITIVE_INFINITY_BITS: u64 = 0x7FF0_0000_0000_0000;
    pub const NEGATIVE_INFINITY_BITS: u64 = 0xFFF0_0000_0000_0000;
    pub const NEGATIVE_ZERO_BITS: u64 = 1 << 63;

    pub const TAG_SHIFT: u64 = 48;
    pub const TAG_EXTRACTION: u64 = 0xFFFF_0000_0000_0000;
    pub const BASE_TAG: u64 = 0x7FF8;
    pub const IS_CELL_BIT: u64 = 0x8000 | BASE_TAG;
    pub const IS_CELL_PATTERN: u64 = 0xFFF8;
    pub const SHIFTED_IS_CELL_PATTERN: u64 = IS_CELL_PATTERN << TAG_SHIFT;

    pub const OBJECT_TAG: u64 = 0b001 | IS_CELL_BIT;
    pub const STRING_TAG: u64 = 0b010 | IS_CELL_BIT;
    pub const SYMBOL_TAG: u64 = 0b011 | IS_CELL_BIT;
    pub const ACCESSOR_TAG: u64 = 0b100 | IS_CELL_BIT;
    pub const BIGINT_TAG: u64 = 0b101 | IS_CELL_BIT;

    pub const BOOLEAN_TAG: u64 = 0b001 | BASE_TAG;
    pub const INT32_TAG: u64 = 0b010 | BASE_TAG;
    pub const EMPTY_TAG: u64 = 0b011 | BASE_TAG;
    pub const UNDEFINED_TAG: u64 = 0b110 | BASE_TAG;
    pub const NULL_TAG: u64 = 0b111 | BASE_TAG;

    pub const IS_NULLISH_EXTRACT_PATTERN: u64 = 0xFFFE;
    pub const IS_NULLISH_PATTERN: u64 = 0x7FFE;

    pub const SHIFTED_BOOLEAN_TAG: u64 = BOOLEAN_TAG << TAG_SHIFT;
    pub const SHIFTED_INT32_TAG: u64 = INT32_TAG << TAG_SHIFT;

    pub const EMPTY_VALUE: u64 = EMPTY_TAG << TAG_SHIFT;
    pub const UNDEFINED_VALUE: u64 = UNDEFINED_TAG << TAG_SHIFT;
    pub const NULL_VALUE: u64 = NULL_TAG << TAG_SHIFT;
    pub const FALSE_VALUE: u64 = SHIFTED_BOOLEAN_TAG;
    pub const TRUE_VALUE: u64 = SHIFTED_BOOLEAN_TAG | 1;
}

/// The reserved registers at the start of every frame.
pub mod register {
    pub const ACCUMULATOR: u32 = 0;
    pub const EXCEPTION: u32 = 1;
    pub const THIS_VALUE: u32 = 2;
    pub const RETURN_VALUE: u32 = 3;
    pub const SAVED_LEXICAL_ENVIRONMENT: u32 = 4;
    pub const RESERVED_REGISTER_COUNT: u32 = 5;
}

macro_rules! define_builtins {
    ($($name:ident: $base:literal . $property:literal, $argument_count:literal;)*) => {
        /// Builtin functions the interpreter recognizes at call sites.
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
        #[repr(u8)]
        pub enum Builtin {
            $($name,)*
        }

        impl Builtin {
            pub const ALL: &[Builtin] = &[$(Builtin::$name,)*];

            /// The name of the intrinsic object the builtin is a property of.
            pub const fn base(self) -> &'static str {
                match self {
                    $(Builtin::$name => $base,)*
                }
            }

            pub const fn property(self) -> &'static str {
                match self {
                    $(Builtin::$name => $property,)*
                }
            }

            pub const fn argument_count(self) -> usize {
                match self {
                    $(Builtin::$name => $argument_count,)*
                }
            }

            pub fn from_u8(value: u8) -> Option<Builtin> {
                Builtin::ALL.get(usize::from(value)).copied()
            }
        }
    };
}

define_builtins! {
    MathAbs: "Math"."abs", 1;
    MathLog: "Math"."log", 1;
    MathPow: "Math"."pow", 2;
    MathExp: "Math"."exp", 1;
    MathCeil: "Math"."ceil", 1;
    MathFloor: "Math"."floor", 1;
    MathImul: "Math"."imul", 2;
    MathRandom: "Math"."random", 0;
    MathRound: "Math"."round", 1;
    MathSqrt: "Math"."sqrt", 1;
    MathSin: "Math"."sin", 1;
    MathCos: "Math"."cos", 1;
    MathTan: "Math"."tan", 1;
    RegExpPrototypeExec: "RegExpPrototype"."exec", 1;
    RegExpPrototypeReplace: "RegExpPrototype"."replace", 2;
    RegExpPrototypeSplit: "RegExpPrototype"."split", 2;
    OrdinaryHasInstance: "InternalBuiltin"."ordinary_has_instance", 1;
    ArrayIteratorPrototypeNext: "ArrayIteratorPrototype"."next", 0;
    MapIteratorPrototypeNext: "MapIteratorPrototype"."next", 0;
    SetIteratorPrototypeNext: "SetIteratorPrototype"."next", 0;
    StringIteratorPrototypeNext: "StringIteratorPrototype"."next", 0;
    StringFromCharCode: "String"."fromCharCode", 1;
    StringPrototypeCharCodeAt: "StringPrototype"."charCodeAt", 1;
    StringPrototypeCharAt: "StringPrototype"."charAt", 1;
}

/// How a property is being set.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum PutKind {
    Normal,
    Getter,
    Setter,
    Prototype,
    /// Always sets an own property, never calls a setter.
    Own,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum EnvironmentMode {
    Lexical,
    Var,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum ArgumentsKind {
    Mapped,
    Unmapped,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum FunctionNamePrefix {
    None,
    Get,
    Set,
}

/// From LibJS/Runtime/Iterator.h.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum IteratorHint {
    Sync,
    Async,
}

/// Completion::Type from LibJS/Runtime/Completion.h.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum CompletionType {
    Empty,
    Normal,
    Break,
    Continue,
    Return,
    Throw,
}

/// The kind of an element of a ClassBlueprint.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ClassElementKind {
    Method,
    Getter,
    Setter,
    Field,
    StaticInitializer,
}
