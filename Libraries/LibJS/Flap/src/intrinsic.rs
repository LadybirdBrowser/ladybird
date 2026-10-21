/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiler-wide identities and properties for Flap intrinsics.

use crate::identity::InlineFunctionId;
use crate::types::ParameterMode;

macro_rules! intrinsic_sources {
    ($name:literal) => {
        &[$name]
    };
    ($name:literal, [$($source:literal),*]) => {
        &[$($source),*]
    };
}

macro_rules! signature {
    (@make $fallible:expr, [$($mode:ident $ty:ident),* $(,)?], $result:expr) => {
        {
            static PARAMETERS: &[IntrinsicParameter] =
                &[$(intrinsic_parameter(ParameterMode::$mode, IntrinsicValueType::$ty)),*];
            IntrinsicCallSignature {
                parameters: PARAMETERS,
                has_else_parameter: $fallible,
                result: $result,
            }
        }
    };
    ([$($mode:ident $ty:ident),* $(,)?]) => {
        signature!(@make false, [$($mode $ty),*], None)
    };
    ([$($mode:ident $ty:ident),* $(,)?] -> $result:ident) => {
        signature!(@make false, [$($mode $ty),*], Some(IntrinsicResultType::Single(IntrinsicValueType::$result)))
    };
    ([$($mode:ident $ty:ident),* $(,)?] -> ($first:ident, $second:ident)) => {
        signature!(@make false, [$($mode $ty),*], Some(IntrinsicResultType::Pair(IntrinsicValueType::$first, IntrinsicValueType::$second)))
    };
    (fallible [$($mode:ident $ty:ident),* $(,)?]) => {
        signature!(@make true, [$($mode $ty),*], None)
    };
    (fallible [$($mode:ident $ty:ident),* $(,)?] -> $result:ident) => {
        signature!(@make true, [$($mode $ty),*], Some(IntrinsicResultType::Single(IntrinsicValueType::$result)))
    };
}

macro_rules! select_signature {
    ($argument_count:expr) => {
        None
    };
    ($argument_count:expr; $($signature:expr),+ $(,)?) => {
        [$($signature),+]
            .into_iter()
            .find(|signature| signature.parameters.len() == $argument_count)
    };
}

macro_rules! intrinsic_names {
    ($operation:ty {
        $($value:expr => $name:literal
            $(from [$($source:literal),*])?
            $(signatures [$($signature:expr),+ $(,)?])?;
        )+
    }) => {
        impl $operation {
            const NAMES: &'static [(Self, &'static str, &'static [&'static str])] = &[
                $(($value, $name, intrinsic_sources!($name $(, [$($source),*])?)),)+
            ];

            pub(crate) fn from_name(name: &str) -> Option<Self> {
                Self::NAMES
                    .iter()
                    .find_map(|(operation, _, sources)| sources.contains(&name).then_some(*operation))
            }

            pub(crate) fn name(self) -> &'static str {
                Self::NAMES
                    .iter()
                    .find_map(|(operation, name, _)| (*operation == self).then_some(*name))
                    .expect("intrinsic operation must have a name")
            }

            pub(crate) fn call_signature(
                self,
                argument_count: usize,
            ) -> Option<IntrinsicCallSignature> {
                let _ = argument_count;
                $(if self == $value {
                    return select_signature!(argument_count $(; $($signature),+)?);
                })+
                unreachable!("intrinsic operation must have signature metadata")
            }
        }
    };
}

macro_rules! define_named_intrinsic_enum {
    (
        $(#[$attribute:meta])*
        $visibility:vis enum $operation:ident {
            $($variant:ident => $name:literal
                $(from [$($source:literal),*])?
                $(signatures [$($signature:expr),+ $(,)?])?;
            )+
        }
    ) => {
        $(#[$attribute])*
        $visibility enum $operation {
            $($variant),+
        }

        intrinsic_names!($operation {
            $(Self::$variant => $name
                $(from [$($source),*])?
                $(signatures [$($signature),+])?;
            )+
        });
    };
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub(crate) enum ModRef {
    #[default]
    None,
    Read,
    Write,
    ReadWrite,
}

impl ModRef {
    pub(crate) fn reads(self) -> bool {
        matches!(self, Self::Read | Self::ReadWrite)
    }

    pub(crate) fn writes(self) -> bool {
        matches!(self, Self::Write | Self::ReadWrite)
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub(crate) struct IntrinsicEffects {
    pub(crate) memory: ModRef,
    pub(crate) machine_state: ModRef,
    pub(crate) may_call: bool,
    pub(crate) may_trap: bool,
}

impl IntrinsicEffects {
    pub(crate) const PURE: Self = Self {
        memory: ModRef::None,
        machine_state: ModRef::None,
        may_call: false,
        may_trap: false,
    };

    pub(crate) const UNKNOWN: Self = Self {
        memory: ModRef::ReadWrite,
        machine_state: ModRef::ReadWrite,
        may_call: true,
        may_trap: true,
    };

    pub(crate) fn can_be_eliminated(self) -> bool {
        !self.memory.writes() && !self.machine_state.writes() && !self.may_call && !self.may_trap
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FieldWidth {
    U8,
    U16,
    U32,
    U64,
}

impl FieldWidth {
    pub(crate) fn bytes(self) -> u8 {
        match self {
            Self::U8 => 1,
            Self::U16 => 2,
            Self::U32 => 4,
            Self::U64 => 8,
        }
    }
}

impl From<FieldWidth> for MemoryWidth {
    fn from(width: FieldWidth) -> Self {
        match width {
            FieldWidth::U8 => Self::Byte,
            FieldWidth::U16 => Self::HalfWord,
            FieldWidth::U32 => Self::Word,
            FieldWidth::U64 => Self::DoubleWord,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FieldAccessKind {
    Load { width: FieldWidth, nonzero: bool },
    LoadCellPointer { nonzero: bool },
    Store(FieldWidth),
}

impl FieldAccessKind {
    pub(crate) fn width(self) -> FieldWidth {
        match self {
            Self::Load { width, .. } | Self::Store(width) => width,
            Self::LoadCellPointer { .. } => FieldWidth::U64,
        }
    }

    pub(crate) fn writes(self) -> bool {
        matches!(self, Self::Store(_))
    }

    pub(crate) fn nonzero(self) -> bool {
        matches!(
            self,
            Self::Load { nonzero: true, .. } | Self::LoadCellPointer { nonzero: true }
        )
    }

    pub(crate) fn memory_operation(self) -> MemoryOperation {
        match self {
            Self::Load {
                width: FieldWidth::U64,
                nonzero: true,
            } => MemoryOperation::Load64NonZero,
            Self::LoadCellPointer { nonzero: true } => MemoryOperation::LoadNonnullCellPointer,
            Self::LoadCellPointer { nonzero: false } => MemoryOperation::LoadCellPointer,
            Self::Load { width, .. } => MemoryOperation::Load {
                width: width.into(),
                signed: false,
            },
            Self::Store(width) => MemoryOperation::Store(width.into()),
        }
    }

    pub(crate) fn paired_operation(self) -> Option<MemoryOperation> {
        match self {
            Self::Load {
                width: FieldWidth::U64, ..
            }
            | Self::LoadCellPointer { .. } => Some(MemoryOperation::LoadPair(PairWidth::DoubleWord)),
            Self::Load {
                width: FieldWidth::U32,
                nonzero: false,
            } => Some(MemoryOperation::LoadPair(PairWidth::Word)),
            Self::Store(FieldWidth::U64) => Some(MemoryOperation::StorePair(PairWidth::DoubleWord)),
            Self::Store(FieldWidth::U32) => Some(MemoryOperation::StorePair(PairWidth::Word)),
            _ => None,
        }
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct FieldPair {
    pub(crate) id: u32,
    pub(crate) order: u8,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct FieldAccess {
    pub(crate) kind: FieldAccessKind,
    pub(crate) pair: Option<FieldPair>,
}

impl FieldAccess {
    pub(crate) fn effects(self) -> IntrinsicEffects {
        self.kind.memory_operation().effects()
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum BinaryOperation {
    Add,
    Subtract,
    Multiply,
    And,
    Or,
    Xor,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ShiftOperation {
    Left,
    RightLogical,
    RightArithmetic,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum IntegerBinaryOperation {
    Binary(BinaryOperation),
    Shift(ShiftOperation),
}

intrinsic_names!(IntegerBinaryOperation {
    Self::Binary(BinaryOperation::Add) => "add_integer" from [] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Binary(BinaryOperation::Subtract) => "sub_integer" from [] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Binary(BinaryOperation::Multiply) => "mul_integer" from [] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Binary(BinaryOperation::And) => "and_integer" from ["and"] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Binary(BinaryOperation::Or) => "or_integer" from ["or"] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Binary(BinaryOperation::Xor) => "xor_integer" from ["xor"] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Shift(ShiftOperation::Left) => "shl_integer" from ["shl"] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Shift(ShiftOperation::RightLogical) => "shr_integer" from ["shr"] signatures [signature!([InOut AnyGpr, In AnyGpr])];
    Self::Shift(ShiftOperation::RightArithmetic) => "sar_integer" from ["sar"] signatures [signature!([InOut AnyGpr, In AnyGpr])];
});

impl IntegerBinaryOperation {
    pub(crate) fn supports_narrow_result(self) -> bool {
        !matches!(
            self,
            Self::Binary(BinaryOperation::Add | BinaryOperation::Subtract | BinaryOperation::Multiply)
        )
    }

    pub(crate) fn is_shift(self) -> bool {
        matches!(self, Self::Shift(_))
    }
}

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum CheckedIntegerOperation {
        Add => "checked_add" signatures [signature!(fallible [In I32, In I32] -> I32)];
        Subtract => "checked_sub" signatures [signature!(fallible [In I32, In I32] -> I32)];
        Multiply => "checked_mul" signatures [signature!(fallible [In I32, In I32] -> I32)];
        Negate => "neg32_overflow" signatures [signature!(fallible [InOut I32])];
    }
}

impl CheckedIntegerOperation {
    pub(crate) fn unchecked(self) -> Option<IntegerBinaryOperation> {
        Some(match self {
            Self::Add => IntegerBinaryOperation::Binary(BinaryOperation::Add),
            Self::Subtract => IntegerBinaryOperation::Binary(BinaryOperation::Subtract),
            Self::Multiply => IntegerBinaryOperation::Binary(BinaryOperation::Multiply),
            Self::Negate => return None,
        })
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ComparisonRelation {
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum IntegerWidth {
    U8,
    U16,
    U32,
    U64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum MemoryWidth {
    Byte,
    HalfWord,
    Word,
    DoubleWord,
    /// An f32 in a floating-point register.
    Float,
    /// An f64 in a floating-point register.
    Double,
    /// A 128-bit vector, in a floating-point register.
    Vector,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum PairWidth {
    Word,
    DoubleWord,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum IntegerSignedness {
    Signed,
    Unsigned,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatComparison {
    Less,
    Greater,
    LessOrEqual,
    GreaterOrEqual,
    Unordered,
    Equal,
}

macro_rules! binary_condition {
    ($name:ident { $first:ident, $second:ident }) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub(crate) enum $name {
            $first,
            $second,
        }

        impl $name {
            pub(crate) fn select<T>(self, first: T, second: T) -> T {
                match self {
                    Self::$first => first,
                    Self::$second => second,
                }
            }

            pub(crate) fn inverted(self) -> Self {
                self.select(Self::$second, Self::$first)
            }
        }
    };
}

binary_condition!(EqualityCondition { Equal, NotEqual });
binary_condition!(TestCondition { Set, Clear });
binary_condition!(ZeroCondition { Zero, NonZero });
binary_condition!(SignCondition { Negative, NotNegative });

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum BranchOperation {
    Equality {
        width: IntegerWidth,
        condition: EqualityCondition,
    },
    Ordered {
        width: IntegerWidth,
        relation: ComparisonRelation,
        signedness: IntegerSignedness,
    },
    Float(FloatComparison),
    Zero {
        width: IntegerWidth,
        condition: ZeroCondition,
    },
    Sign {
        width: IntegerWidth,
        condition: SignCondition,
    },
    Bits {
        width: MemoryWidth,
        condition: TestCondition,
    },
    Bit(TestCondition),
    Tag(EqualityCondition),
    Singleton(EqualityCondition),
    Memory {
        width: PairWidth,
        condition: EqualityCondition,
    },
    AnyEqual,
    ValueBitsNegative,
    OrderedFloat(OrderedFloatComparison),
}

intrinsic_names!(BranchOperation {
    Self::Equality { width: IntegerWidth::U64, condition: EqualityCondition::Equal } => "branch_eq" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Equality { width: IntegerWidth::U64, condition: EqualityCondition::NotEqual } => "branch_ne" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::Less, signedness: IntegerSignedness::Signed } => "branch_lt_signed" signatures [signature!([In I32, In I32, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::Greater, signedness: IntegerSignedness::Signed } => "branch_gt_signed" signatures [signature!([In I32, In I32, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::LessOrEqual, signedness: IntegerSignedness::Signed } => "branch_le_signed" signatures [signature!([In I32, In I32, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::GreaterOrEqual, signedness: IntegerSignedness::Signed } => "branch_ge_signed" signatures [signature!([In I32, In I32, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::Less, signedness: IntegerSignedness::Unsigned } => "branch_lt_unsigned" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::Greater, signedness: IntegerSignedness::Unsigned } => "branch_gt_unsigned" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::LessOrEqual, signedness: IntegerSignedness::Unsigned } => "branch_le_unsigned" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Ordered { width: IntegerWidth::U64, relation: ComparisonRelation::GreaterOrEqual, signedness: IntegerSignedness::Unsigned } => "branch_ge_unsigned" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Float(FloatComparison::Less) => "branch_fp_less" signatures [signature!([In F64, In F64, In Label])];
    Self::Float(FloatComparison::Greater) => "branch_fp_greater" signatures [signature!([In F64, In F64, In Label])];
    Self::Float(FloatComparison::LessOrEqual) => "branch_fp_less_or_equal" signatures [signature!([In F64, In F64, In Label])];
    Self::Float(FloatComparison::GreaterOrEqual) => "branch_fp_greater_or_equal" signatures [signature!([In F64, In F64, In Label])];
    Self::Float(FloatComparison::Unordered) => "branch_fp_unordered" signatures [signature!([In F64, In F64, In Label])];
    Self::Float(FloatComparison::Equal) => "branch_fp_equal" signatures [signature!([In F64, In F64, In Label])];
    Self::Zero { width: IntegerWidth::U64, condition: ZeroCondition::Zero } => "branch_zero" signatures [signature!([In AnyGpr, In Label])];
    Self::Zero { width: IntegerWidth::U32, condition: ZeroCondition::Zero } => "branch_zero32" signatures [signature!([In AnyGpr, In Label])];
    Self::Zero { width: IntegerWidth::U64, condition: ZeroCondition::NonZero } => "branch_nonzero" signatures [signature!([In AnyGpr, In Label])];
    Self::Zero { width: IntegerWidth::U32, condition: ZeroCondition::NonZero } => "branch_nonzero32" signatures [signature!([In AnyGpr, In Label])];
    Self::Sign { width: IntegerWidth::U64, condition: SignCondition::Negative } => "branch_negative" signatures [signature!([In AnyGpr, In Label])];
    Self::Sign { width: IntegerWidth::U64, condition: SignCondition::NotNegative } => "branch_not_negative" signatures [signature!([In I32, In Label])];
    Self::Bits { width: MemoryWidth::DoubleWord, condition: TestCondition::Set } => "branch_bits_set" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Bits { width: MemoryWidth::DoubleWord, condition: TestCondition::Clear } => "branch_bits_clear" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::Bit(TestCondition::Set) => "branch_bit_set" signatures [signature!([In AnyGpr, In AnyGpr, In Label])];
    Self::AnyEqual => "branch_any_eq" signatures [signature!([In AnyGpr, In AnyGpr, In AnyGpr, In Label])];
    Self::ValueBitsNegative => "branch_value_bits_negative" signatures [signature!([In Value, In Label])];
    Self::OrderedFloat(OrderedFloatComparison::Equal) => "branch_float_equal" signatures [signature!([In F64, In F64, In Label])];
    Self::OrderedFloat(OrderedFloatComparison::NotEqual) => "branch_float_not_equal" signatures [signature!([In F64, In F64, In Label])];
    Self::OrderedFloat(OrderedFloatComparison::Less) => "branch_float_less" signatures [signature!([In F64, In F64, In Label])];
    Self::OrderedFloat(OrderedFloatComparison::LessOrEqual) => "branch_float_less_or_equal" signatures [signature!([In F64, In F64, In Label])];
    Self::OrderedFloat(OrderedFloatComparison::Greater) => "branch_float_greater" signatures [signature!([In F64, In F64, In Label])];
    Self::OrderedFloat(OrderedFloatComparison::GreaterOrEqual) => "branch_float_greater_or_equal" signatures [signature!([In F64, In F64, In Label])];
});

impl BranchOperation {
    pub(crate) fn inverted(self) -> Option<Self> {
        let ordered = |relation| match relation {
            ComparisonRelation::Less => ComparisonRelation::GreaterOrEqual,
            ComparisonRelation::LessOrEqual => ComparisonRelation::Greater,
            ComparisonRelation::Greater => ComparisonRelation::LessOrEqual,
            ComparisonRelation::GreaterOrEqual => ComparisonRelation::Less,
        };
        Some(match self {
            Self::Equality { width, condition } => Self::Equality {
                width,
                condition: condition.inverted(),
            },
            Self::Ordered {
                width,
                relation,
                signedness,
            } => Self::Ordered {
                width,
                relation: ordered(relation),
                signedness,
            },
            Self::Zero { width, condition } => Self::Zero {
                width,
                condition: condition.inverted(),
            },
            Self::Sign { width, condition } => Self::Sign {
                width,
                condition: condition.inverted(),
            },
            Self::Bits { width, condition } => Self::Bits {
                width,
                condition: condition.inverted(),
            },
            Self::Bit(condition) => Self::Bit(condition.inverted()),
            Self::Tag(condition) => Self::Tag(condition.inverted()),
            Self::Singleton(condition) => Self::Singleton(condition.inverted()),
            Self::Memory { width, condition } => Self::Memory {
                width,
                condition: condition.inverted(),
            },
            Self::ValueBitsNegative => Self::Sign {
                width: IntegerWidth::U64,
                condition: SignCondition::NotNegative,
            },
            Self::OrderedFloat(comparison) => Self::OrderedFloat(match comparison {
                OrderedFloatComparison::Equal => OrderedFloatComparison::NotEqual,
                OrderedFloatComparison::NotEqual => OrderedFloatComparison::Equal,
                _ => return None,
            }),
            Self::Float(_) | Self::AnyEqual => return None,
        })
    }
}

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum AssertionOperation {
        Assert => "assert" from [];
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ValueOperation {
    Reuse,
    AssumeInt32,
    AssumeUint32,
    TruncateUint8,
    TruncateUint16,
    ReinterpretUint64AsValue,
    BoxInt32 { clean: bool },
    BoxFloat64,
    BoxNumber,
    ValueToFloat64,
    UnboxInt32 { rematerialized: bool },
    UnboxBoolean,
    ExtractTag { rematerialized: bool },
    ToInt32,
    Int32ToInt64,
    ToUint32,
    UnboxObject,
}

impl ValueOperation {
    pub(crate) fn is_rematerialized(self) -> bool {
        matches!(
            self,
            Self::UnboxInt32 { rematerialized: true } | Self::ExtractTag { rematerialized: true }
        )
    }
}

intrinsic_names!(ValueOperation {
    Self::Reuse => "reuse";
    Self::AssumeInt32 => "assume_i32" signatures [signature!([In U32] -> I32)];
    Self::AssumeUint32 => "assume_u32" signatures [signature!([In I32] -> U32)];
    Self::TruncateUint8 => "truncate_u8" signatures [signature!([In I32] -> U8)];
    Self::TruncateUint16 => "truncate_u16" signatures [signature!([In I32] -> U16)];
    Self::ReinterpretUint64AsValue => "reinterpret_u64_as_value" signatures [signature!([In U64] -> Value)];
    Self::BoxInt32 { clean: true } => "box_i32" signatures [signature!([In I32] -> Value)];
    Self::BoxInt32 { clean: false } => "box_int32" signatures [signature!([Out Value, In AnyGpr])];
    Self::BoxFloat64 => "box_f64" signatures [signature!([In F64] -> Value)];
    Self::BoxNumber => "box_number" signatures [signature!([In F64] -> Value)];
    Self::ValueToFloat64 => "unbox_f64" signatures [signature!([In Value] -> F64), signature!([Out F64, In Value])];
    Self::UnboxInt32 { rematerialized: false } => "unbox_i32" signatures [signature!([In Value] -> I32), signature!([Out I32, In Value])];
    Self::UnboxInt32 { rematerialized: true } => "rematerialize_unbox_i32" from [];
    Self::UnboxBoolean => "unbox_bool";
    Self::ExtractTag { rematerialized: false } => "extract_tag" signatures [signature!([In Value] -> ValueTag), signature!([Out ValueTag, In Value])];
    Self::ExtractTag { rematerialized: true } => "rematerialize_extract_tag" from [];
    Self::Int32ToInt64 => "i64" signatures [signature!([In I32] -> I64)];
    Self::ToInt32 => "i32" signatures [signature!([In AnyGpr] -> I32)];
    Self::ToUint32 => "u32" signatures [signature!([In AnyGpr] -> U32)];
    Self::UnboxObject => "unbox_object" signatures [signature!([Out AnyGpr, In AnyGpr])];
});

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum LowLevelOperation {
        LoadLabel => "load_label" signatures [signature!([Out BytecodeOffset, In BytecodeOffset])];
        Modulo64 => "mod_i64" signatures [signature!([In I64, In I64] -> I64)];
        DivideModulo => "divmod" signatures [signature!([In I32, In I32] -> (I32, I32)), signature!([Out I32, Out I32, In I32, In I32])];
        Move => "mov" signatures [signature!([Out AnyGpr, In AnyGpr])];
        LoadEffectiveAddress => "lea" signatures [signature!([Out AnyGpr, In Memory])];
        LoadVm => "load_vm" signatures [signature!([] -> Vm), signature!([Out AnyGpr])];
        Increment32Memory => "inc32_mem" signatures [signature!([In Memory])];
        Or8Memory => "or8_mem" signatures [signature!([In Memory, In AnyGpr])];
        Or32Memory => "or32_mem" signatures [signature!([In Memory, In AnyGpr])];
        Subtract32Memory => "sub32_mem" signatures [signature!([In Memory, In AnyGpr])];
        ClearBit => "clear_bit" signatures [signature!([InOut AnyGpr, In AnyGpr])];
        ToggleBit => "toggle_bit" signatures [signature!([InOut AnyGpr, In AnyGpr])];
        Negate => "neg" signatures [signature!([InOut AnyGpr])];
        BitwiseNotInt32 => "bitwise_not_i32" from [];
        Add => "add" signatures [signature!([InOut AnyGpr, In AnyGpr])];
        Subtract => "sub" signatures [signature!([InOut AnyGpr, In AnyGpr])];
    }
}

define_named_intrinsic_enum! {
    // Integer and floating-point operations that machine-independent code has no other form for. Integer operands
    // and results are bit patterns: 32-bit ones are zero-extended u32s, whatever their signedness. Truncating a
    // float does not check the range: out-of-range inputs give 0x8000000000000000.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum NumericOperation {
        DivideSigned32 => "div_s32" signatures [signature!([In U32, In U32] -> U32)];
        DivideUnsigned32 => "div_u32" signatures [signature!([In U32, In U32] -> U32)];
        RemainderSigned32 => "rem_s32" signatures [signature!([In U32, In U32] -> U32)];
        RemainderUnsigned32 => "rem_u32" signatures [signature!([In U32, In U32] -> U32)];
        DivideSigned64 => "div_s64" signatures [signature!([In U64, In U64] -> U64)];
        DivideUnsigned64 => "div_u64" signatures [signature!([In U64, In U64] -> U64)];
        RemainderSigned64 => "rem_s64" signatures [signature!([In U64, In U64] -> U64)];
        RemainderUnsigned64 => "rem_u64" signatures [signature!([In U64, In U64] -> U64)];
        CountLeadingZeros32 => "clz32" signatures [signature!([In U32] -> U32)];
        CountTrailingZeros32 => "ctz32" signatures [signature!([In U32] -> U32)];
        PopulationCount32 => "popcnt32" signatures [signature!([In U32] -> U32)];
        CountLeadingZeros64 => "clz64" signatures [signature!([In U64] -> U64)];
        CountTrailingZeros64 => "ctz64" signatures [signature!([In U64] -> U64)];
        PopulationCount64 => "popcnt64" signatures [signature!([In U64] -> U64)];
        RotateLeft32 => "rotl32" signatures [signature!([In U32, In U32] -> U32)];
        RotateRight32 => "rotr32" signatures [signature!([In U32, In U32] -> U32)];
        RotateLeft64 => "rotl64" signatures [signature!([In U64, In U64] -> U64)];
        RotateRight64 => "rotr64" signatures [signature!([In U64, In U64] -> U64)];
        Float64FromBits => "f64_from_bits" signatures [signature!([In U64] -> F64)];
        Float64ToBits => "f64_to_bits" signatures [signature!([In F64] -> U64)];
        Float32FromBits => "f32_from_bits" signatures [signature!([In U32] -> F32)];
        Float32ToBits => "f32_to_bits" signatures [signature!([In F32] -> U32)];
        TruncateFloat64ToSigned64 => "truncate_f64_to_s64" signatures [signature!([In F64] -> U64)];
        ConvertSigned64ToFloat32 => "s64_to_f32" signatures [signature!([In U64] -> F32)];
        ConvertSigned64ToFloat64 => "s64_to_f64" signatures [signature!([In U64] -> F64)];
        RoundFloat64ToZero => "trunc_f64" signatures [signature!([In F64] -> F64)];
        RoundFloat64ToNearest => "nearest_f64" signatures [signature!([In F64] -> F64)];
        RoundFloat64Up => "ceil_f64" signatures [signature!([In F64] -> F64)];
        RoundFloat64Down => "floor_f64" signatures [signature!([In F64] -> F64)];
        SquareRootFloat64 => "sqrt_f64" signatures [signature!([In F64] -> F64)];
        AddFloat32 => "add_f32" signatures [signature!([In F32, In F32] -> F32)];
        SubtractFloat32 => "sub_f32" signatures [signature!([In F32, In F32] -> F32)];
        MultiplyFloat32 => "mul_f32" signatures [signature!([In F32, In F32] -> F32)];
        DivideFloat32 => "div_f32" signatures [signature!([In F32, In F32] -> F32)];
        SquareRootFloat32 => "sqrt_f32" signatures [signature!([In F32] -> F32)];
        PromoteFloat32 => "promote_f32" signatures [signature!([In F32] -> F64)];
        DemoteFloat64 => "demote_f64" signatures [signature!([In F64] -> F32)];
    }
}

define_named_intrinsic_enum! {
    // Atomic accesses to 8 to 64 bits of memory, sequentially consistent: loads, and read-modify-write operations,
    // which return the old value, zero-extended. An exchange whose result is unused is a sequentially consistent
    // store. The fence orders the plain accesses around it like a sequentially consistent access would.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum AtomicOperation {
        CompareExchange8 => "atomic_cmpxchg8" signatures [signature!([In Memory, In U64, In U64] -> U64)];
        CompareExchange16 => "atomic_cmpxchg16" signatures [signature!([In Memory, In U64, In U64] -> U64)];
        CompareExchange32 => "atomic_cmpxchg32" signatures [signature!([In Memory, In U64, In U64] -> U64)];
        CompareExchange64 => "atomic_cmpxchg64" signatures [signature!([In Memory, In U64, In U64] -> U64)];
        Exchange8 => "atomic_xchg8" signatures [signature!([In Memory, In U64] -> U64)];
        Exchange16 => "atomic_xchg16" signatures [signature!([In Memory, In U64] -> U64)];
        Exchange32 => "atomic_xchg32" signatures [signature!([In Memory, In U64] -> U64)];
        Exchange64 => "atomic_xchg64" signatures [signature!([In Memory, In U64] -> U64)];
        FetchAdd8 => "atomic_add8" signatures [signature!([In Memory, In U64] -> U64)];
        FetchAdd16 => "atomic_add16" signatures [signature!([In Memory, In U64] -> U64)];
        FetchAdd32 => "atomic_add32" signatures [signature!([In Memory, In U64] -> U64)];
        FetchAdd64 => "atomic_add64" signatures [signature!([In Memory, In U64] -> U64)];
        Load8 => "atomic_load8" signatures [signature!([In Memory] -> U64)];
        Load16 => "atomic_load16" signatures [signature!([In Memory] -> U64)];
        Load32 => "atomic_load32" signatures [signature!([In Memory] -> U64)];
        Load64 => "atomic_load64" signatures [signature!([In Memory] -> U64)];
        Fence => "atomic_fence" signatures [signature!([])];
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum AtomicOperationKind {
    CompareExchange,
    Exchange,
    FetchAdd,
    Load,
    Fence,
}

impl AtomicOperation {
    pub(crate) fn kind(self) -> AtomicOperationKind {
        use AtomicOperation::*;
        match self {
            CompareExchange8 | CompareExchange16 | CompareExchange32 | CompareExchange64 => {
                AtomicOperationKind::CompareExchange
            }
            Exchange8 | Exchange16 | Exchange32 | Exchange64 => AtomicOperationKind::Exchange,
            FetchAdd8 | FetchAdd16 | FetchAdd32 | FetchAdd64 => AtomicOperationKind::FetchAdd,
            Load8 | Load16 | Load32 | Load64 => AtomicOperationKind::Load,
            Fence => AtomicOperationKind::Fence,
        }
    }

    pub(crate) fn width(self) -> IntegerWidth {
        use AtomicOperation::*;
        match self {
            CompareExchange8 | Exchange8 | FetchAdd8 | Load8 => IntegerWidth::U8,
            CompareExchange16 | Exchange16 | FetchAdd16 | Load16 => IntegerWidth::U16,
            CompareExchange32 | Exchange32 | FetchAdd32 | Load32 => IntegerWidth::U32,
            CompareExchange64 | Exchange64 | FetchAdd64 | Load64 | Fence => IntegerWidth::U64,
        }
    }
}

define_named_intrinsic_enum! {
    // 128-bit vector operations, most of them one SSE4.1 instruction on x86-64. Lanes are little-endian, the lowest
    // lane in the lowest bytes. Binary operations take the destination's lanes from their left operand. Shifts take
    // their count from the low 64 bits of their right operand. Comparisons set every bit of a true lane.
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum VectorOperation {
        FromU64Pair => "v128_from_u64x2" signatures [signature!([In U64, In U64] -> V128)];
        And => "v128_and" signatures [signature!([In V128, In V128] -> V128)];
        Or => "v128_or" signatures [signature!([In V128, In V128] -> V128)];
        Xor => "v128_xor" signatures [signature!([In V128, In V128] -> V128)];
        AndNot => "v128_andnot" signatures [signature!([In V128, In V128] -> V128)];
        AddI8 => "v128_add_i8" signatures [signature!([In V128, In V128] -> V128)];
        SubtractI8 => "v128_sub_i8" signatures [signature!([In V128, In V128] -> V128)];
        EqualI8 => "v128_eq_i8" signatures [signature!([In V128, In V128] -> V128)];
        GreaterSignedI8 => "v128_gt_s_i8" signatures [signature!([In V128, In V128] -> V128)];
        AddI16 => "v128_add_i16" signatures [signature!([In V128, In V128] -> V128)];
        SubtractI16 => "v128_sub_i16" signatures [signature!([In V128, In V128] -> V128)];
        EqualI16 => "v128_eq_i16" signatures [signature!([In V128, In V128] -> V128)];
        GreaterSignedI16 => "v128_gt_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        AddI32 => "v128_add_i32" signatures [signature!([In V128, In V128] -> V128)];
        SubtractI32 => "v128_sub_i32" signatures [signature!([In V128, In V128] -> V128)];
        EqualI32 => "v128_eq_i32" signatures [signature!([In V128, In V128] -> V128)];
        GreaterSignedI32 => "v128_gt_s_i32" signatures [signature!([In V128, In V128] -> V128)];
        AddI64 => "v128_add_i64" signatures [signature!([In V128, In V128] -> V128)];
        SubtractI64 => "v128_sub_i64" signatures [signature!([In V128, In V128] -> V128)];
        EqualI64 => "v128_eq_i64" signatures [signature!([In V128, In V128] -> V128)];
        GreaterSignedI64 => "v128_gt_s_i64" signatures [signature!([In V128, In V128] -> V128)];
        AddSaturateSignedI8 => "v128_add_sat_s_i8" signatures [signature!([In V128, In V128] -> V128)];
        AddSaturateUnsignedI8 => "v128_add_sat_u_i8" signatures [signature!([In V128, In V128] -> V128)];
        SubtractSaturateSignedI8 => "v128_sub_sat_s_i8" signatures [signature!([In V128, In V128] -> V128)];
        SubtractSaturateUnsignedI8 => "v128_sub_sat_u_i8" signatures [signature!([In V128, In V128] -> V128)];
        AverageUnsignedI8 => "v128_avgr_u_i8" signatures [signature!([In V128, In V128] -> V128)];
        AddSaturateSignedI16 => "v128_add_sat_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        AddSaturateUnsignedI16 => "v128_add_sat_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        SubtractSaturateSignedI16 => "v128_sub_sat_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        SubtractSaturateUnsignedI16 => "v128_sub_sat_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        AverageUnsignedI16 => "v128_avgr_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyI16 => "v128_mul_i16" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyI32 => "v128_mul_i32" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyUnsignedLowI32 => "v128_mul_u32_to_u64" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyHighSignedI16 => "v128_mulhi_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyHighUnsignedI16 => "v128_mulhi_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyHighRoundScaleI16 => "v128_mulhrs_i16" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyAddI16 => "v128_madd_i16" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyAddUnsignedSignedI8 => "v128_madd_u8_s8" signatures [signature!([In V128, In V128] -> V128)];
        MinimumSignedI8 => "v128_min_s_i8" signatures [signature!([In V128, In V128] -> V128)];
        MinimumUnsignedI8 => "v128_min_u_i8" signatures [signature!([In V128, In V128] -> V128)];
        MaximumSignedI8 => "v128_max_s_i8" signatures [signature!([In V128, In V128] -> V128)];
        MaximumUnsignedI8 => "v128_max_u_i8" signatures [signature!([In V128, In V128] -> V128)];
        MinimumSignedI16 => "v128_min_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        MinimumUnsignedI16 => "v128_min_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        MaximumSignedI16 => "v128_max_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        MaximumUnsignedI16 => "v128_max_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        MinimumSignedI32 => "v128_min_s_i32" signatures [signature!([In V128, In V128] -> V128)];
        MinimumUnsignedI32 => "v128_min_u_i32" signatures [signature!([In V128, In V128] -> V128)];
        MaximumSignedI32 => "v128_max_s_i32" signatures [signature!([In V128, In V128] -> V128)];
        MaximumUnsignedI32 => "v128_max_u_i32" signatures [signature!([In V128, In V128] -> V128)];
        ShiftLeftI16 => "v128_shl_i16" signatures [signature!([In V128, In V128] -> V128)];
        ShiftRightUnsignedI16 => "v128_shr_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        ShiftLeftI32 => "v128_shl_i32" signatures [signature!([In V128, In V128] -> V128)];
        ShiftRightUnsignedI32 => "v128_shr_u_i32" signatures [signature!([In V128, In V128] -> V128)];
        ShiftLeftI64 => "v128_shl_i64" signatures [signature!([In V128, In V128] -> V128)];
        ShiftRightUnsignedI64 => "v128_shr_u_i64" signatures [signature!([In V128, In V128] -> V128)];
        ShiftRightSignedI16 => "v128_shr_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        ShiftRightSignedI32 => "v128_shr_s_i32" signatures [signature!([In V128, In V128] -> V128)];
        Shuffle8 => "v128_shuffle_i8" signatures [signature!([In V128, In V128] -> V128)];
        NarrowSignedI16 => "v128_narrow_s_i16" signatures [signature!([In V128, In V128] -> V128)];
        NarrowUnsignedI16 => "v128_narrow_u_i16" signatures [signature!([In V128, In V128] -> V128)];
        NarrowSignedI32 => "v128_narrow_s_i32" signatures [signature!([In V128, In V128] -> V128)];
        NarrowUnsignedI32 => "v128_narrow_u_i32" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveLowI8 => "v128_interleave_low_i8" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveHighI8 => "v128_interleave_high_i8" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveLowI16 => "v128_interleave_low_i16" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveHighI16 => "v128_interleave_high_i16" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveLowI32 => "v128_interleave_low_i32" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveHighI32 => "v128_interleave_high_i32" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveLowI64 => "v128_interleave_low_i64" signatures [signature!([In V128, In V128] -> V128)];
        InterleaveHighI64 => "v128_interleave_high_i64" signatures [signature!([In V128, In V128] -> V128)];
        AddF32 => "v128_add_f32" signatures [signature!([In V128, In V128] -> V128)];
        SubtractF32 => "v128_sub_f32" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyF32 => "v128_mul_f32" signatures [signature!([In V128, In V128] -> V128)];
        DivideF32 => "v128_div_f32" signatures [signature!([In V128, In V128] -> V128)];
        MinimumF32 => "v128_min_f32" signatures [signature!([In V128, In V128] -> V128)];
        MaximumF32 => "v128_max_f32" signatures [signature!([In V128, In V128] -> V128)];
        EqualF32 => "v128_eq_f32" signatures [signature!([In V128, In V128] -> V128)];
        NotEqualF32 => "v128_ne_f32" signatures [signature!([In V128, In V128] -> V128)];
        LessF32 => "v128_lt_f32" signatures [signature!([In V128, In V128] -> V128)];
        LessOrEqualF32 => "v128_le_f32" signatures [signature!([In V128, In V128] -> V128)];
        UnorderedF32 => "v128_unord_f32" signatures [signature!([In V128, In V128] -> V128)];
        SquareRootF32 => "v128_sqrt_f32" signatures [signature!([In V128] -> V128)];
        CeilF32 => "v128_ceil_f32" signatures [signature!([In V128] -> V128)];
        FloorF32 => "v128_floor_f32" signatures [signature!([In V128] -> V128)];
        TruncF32 => "v128_trunc_f32" signatures [signature!([In V128] -> V128)];
        NearestF32 => "v128_nearest_f32" signatures [signature!([In V128] -> V128)];
        AddF64 => "v128_add_f64" signatures [signature!([In V128, In V128] -> V128)];
        SubtractF64 => "v128_sub_f64" signatures [signature!([In V128, In V128] -> V128)];
        MultiplyF64 => "v128_mul_f64" signatures [signature!([In V128, In V128] -> V128)];
        DivideF64 => "v128_div_f64" signatures [signature!([In V128, In V128] -> V128)];
        MinimumF64 => "v128_min_f64" signatures [signature!([In V128, In V128] -> V128)];
        MaximumF64 => "v128_max_f64" signatures [signature!([In V128, In V128] -> V128)];
        EqualF64 => "v128_eq_f64" signatures [signature!([In V128, In V128] -> V128)];
        NotEqualF64 => "v128_ne_f64" signatures [signature!([In V128, In V128] -> V128)];
        LessF64 => "v128_lt_f64" signatures [signature!([In V128, In V128] -> V128)];
        LessOrEqualF64 => "v128_le_f64" signatures [signature!([In V128, In V128] -> V128)];
        UnorderedF64 => "v128_unord_f64" signatures [signature!([In V128, In V128] -> V128)];
        SquareRootF64 => "v128_sqrt_f64" signatures [signature!([In V128] -> V128)];
        CeilF64 => "v128_ceil_f64" signatures [signature!([In V128] -> V128)];
        FloorF64 => "v128_floor_f64" signatures [signature!([In V128] -> V128)];
        TruncF64 => "v128_trunc_f64" signatures [signature!([In V128] -> V128)];
        NearestF64 => "v128_nearest_f64" signatures [signature!([In V128] -> V128)];
        AbsoluteI8 => "v128_abs_i8" signatures [signature!([In V128] -> V128)];
        AbsoluteI16 => "v128_abs_i16" signatures [signature!([In V128] -> V128)];
        AbsoluteI32 => "v128_abs_i32" signatures [signature!([In V128] -> V128)];
        ExtendLowSignedI8 => "v128_extend_low_s_i8" signatures [signature!([In V128] -> V128)];
        ExtendLowUnsignedI8 => "v128_extend_low_u_i8" signatures [signature!([In V128] -> V128)];
        ExtendLowSignedI16 => "v128_extend_low_s_i16" signatures [signature!([In V128] -> V128)];
        ExtendLowUnsignedI16 => "v128_extend_low_u_i16" signatures [signature!([In V128] -> V128)];
        ExtendLowSignedI32 => "v128_extend_low_s_i32" signatures [signature!([In V128] -> V128)];
        ExtendLowUnsignedI32 => "v128_extend_low_u_i32" signatures [signature!([In V128] -> V128)];
        ConvertSignedI32ToF32 => "v128_convert_s_i32_f32" signatures [signature!([In V128] -> V128)];
        TruncateF32ToSignedI32 => "v128_truncate_f32_s_i32" signatures [signature!([In V128] -> V128)];
        ConvertLowSignedI32ToF64 => "v128_convert_low_s_i32_f64" signatures [signature!([In V128] -> V128)];
        TruncateF64ToSignedI32 => "v128_truncate_f64_s_i32" signatures [signature!([In V128] -> V128)];
        PromoteLowF32 => "v128_promote_low_f32" signatures [signature!([In V128] -> V128)];
        DemoteF64 => "v128_demote_f64" signatures [signature!([In V128] -> V128)];
        HighHalf => "v128_high_half" signatures [signature!([In V128] -> V128)];
        BroadcastLowI32 => "v128_broadcast_low_i32" signatures [signature!([In V128] -> V128)];
        Low64 => "v128_low_u64" signatures [signature!([In V128] -> U64)];
        High64 => "v128_high_u64" signatures [signature!([In V128] -> U64)];
        ByteMask => "v128_bitmask_i8" signatures [signature!([In V128] -> U64)];
        SignMaskF32 => "v128_bitmask_f32" signatures [signature!([In V128] -> U64)];
        SignMaskF64 => "v128_bitmask_f64" signatures [signature!([In V128] -> U64)];
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub(crate) enum VectorOperationKind {
    /// Two u64 halves, the low one first, make a vector.
    FromU64Pair,
    Binary,
    Unary,
    /// A vector gives a u64.
    ToGeneralPurpose,
    /// A vector gives a u32, zero-extended.
    ToGeneralPurpose32,
}

impl VectorOperation {
    pub(crate) fn kind(self) -> VectorOperationKind {
        match self {
            Self::FromU64Pair => VectorOperationKind::FromU64Pair,
            Self::And
            | Self::Or
            | Self::Xor
            | Self::AndNot
            | Self::AddI8
            | Self::SubtractI8
            | Self::EqualI8
            | Self::GreaterSignedI8
            | Self::AddI16
            | Self::SubtractI16
            | Self::EqualI16
            | Self::GreaterSignedI16
            | Self::AddI32
            | Self::SubtractI32
            | Self::EqualI32
            | Self::GreaterSignedI32
            | Self::AddI64
            | Self::SubtractI64
            | Self::EqualI64
            | Self::GreaterSignedI64
            | Self::AddSaturateSignedI8
            | Self::AddSaturateUnsignedI8
            | Self::SubtractSaturateSignedI8
            | Self::SubtractSaturateUnsignedI8
            | Self::AverageUnsignedI8
            | Self::AddSaturateSignedI16
            | Self::AddSaturateUnsignedI16
            | Self::SubtractSaturateSignedI16
            | Self::SubtractSaturateUnsignedI16
            | Self::AverageUnsignedI16
            | Self::MultiplyI16
            | Self::MultiplyI32
            | Self::MultiplyUnsignedLowI32
            | Self::MultiplyHighSignedI16
            | Self::MultiplyHighUnsignedI16
            | Self::MultiplyHighRoundScaleI16
            | Self::MultiplyAddI16
            | Self::MultiplyAddUnsignedSignedI8
            | Self::MinimumSignedI8
            | Self::MinimumUnsignedI8
            | Self::MaximumSignedI8
            | Self::MaximumUnsignedI8
            | Self::MinimumSignedI16
            | Self::MinimumUnsignedI16
            | Self::MaximumSignedI16
            | Self::MaximumUnsignedI16
            | Self::MinimumSignedI32
            | Self::MinimumUnsignedI32
            | Self::MaximumSignedI32
            | Self::MaximumUnsignedI32
            | Self::ShiftLeftI16
            | Self::ShiftRightUnsignedI16
            | Self::ShiftLeftI32
            | Self::ShiftRightUnsignedI32
            | Self::ShiftLeftI64
            | Self::ShiftRightUnsignedI64
            | Self::ShiftRightSignedI16
            | Self::ShiftRightSignedI32
            | Self::Shuffle8
            | Self::NarrowSignedI16
            | Self::NarrowUnsignedI16
            | Self::NarrowSignedI32
            | Self::NarrowUnsignedI32
            | Self::InterleaveLowI8
            | Self::InterleaveHighI8
            | Self::InterleaveLowI16
            | Self::InterleaveHighI16
            | Self::InterleaveLowI32
            | Self::InterleaveHighI32
            | Self::InterleaveLowI64
            | Self::InterleaveHighI64
            | Self::AddF32
            | Self::SubtractF32
            | Self::MultiplyF32
            | Self::DivideF32
            | Self::MinimumF32
            | Self::MaximumF32
            | Self::EqualF32
            | Self::NotEqualF32
            | Self::LessF32
            | Self::LessOrEqualF32
            | Self::UnorderedF32
            | Self::AddF64
            | Self::SubtractF64
            | Self::MultiplyF64
            | Self::DivideF64
            | Self::MinimumF64
            | Self::MaximumF64
            | Self::EqualF64
            | Self::NotEqualF64
            | Self::LessF64
            | Self::LessOrEqualF64
            | Self::UnorderedF64 => VectorOperationKind::Binary,
            Self::SquareRootF32
            | Self::CeilF32
            | Self::FloorF32
            | Self::TruncF32
            | Self::NearestF32
            | Self::SquareRootF64
            | Self::CeilF64
            | Self::FloorF64
            | Self::TruncF64
            | Self::NearestF64
            | Self::AbsoluteI8
            | Self::AbsoluteI16
            | Self::AbsoluteI32
            | Self::ExtendLowSignedI8
            | Self::ExtendLowUnsignedI8
            | Self::ExtendLowSignedI16
            | Self::ExtendLowUnsignedI16
            | Self::ExtendLowSignedI32
            | Self::ExtendLowUnsignedI32
            | Self::ConvertSignedI32ToF32
            | Self::TruncateF32ToSignedI32
            | Self::ConvertLowSignedI32ToF64
            | Self::TruncateF64ToSignedI32
            | Self::PromoteLowF32
            | Self::DemoteF64
            | Self::HighHalf
            | Self::BroadcastLowI32 => VectorOperationKind::Unary,
            Self::Low64 | Self::High64 => VectorOperationKind::ToGeneralPurpose,
            Self::ByteMask | Self::SignMaskF32 | Self::SignMaskF64 => VectorOperationKind::ToGeneralPurpose32,
        }
    }

    pub(crate) fn x86_64_mnemonic(self) -> &'static str {
        match self {
            Self::FromU64Pair => "movq",
            Self::And => "pand",
            Self::Or => "por",
            Self::Xor => "pxor",
            Self::AndNot => "pandn",
            Self::AddI8 => "paddb",
            Self::SubtractI8 => "psubb",
            Self::EqualI8 => "pcmpeqb",
            Self::GreaterSignedI8 => "pcmpgtb",
            Self::AddI16 => "paddw",
            Self::SubtractI16 => "psubw",
            Self::EqualI16 => "pcmpeqw",
            Self::GreaterSignedI16 => "pcmpgtw",
            Self::AddI32 => "paddd",
            Self::SubtractI32 => "psubd",
            Self::EqualI32 => "pcmpeqd",
            Self::GreaterSignedI32 => "pcmpgtd",
            Self::AddI64 => "paddq",
            Self::SubtractI64 => "psubq",
            Self::EqualI64 => "pcmpeqq",
            Self::GreaterSignedI64 => "pcmpgtq",
            Self::AddSaturateSignedI8 => "paddsb",
            Self::AddSaturateUnsignedI8 => "paddusb",
            Self::SubtractSaturateSignedI8 => "psubsb",
            Self::SubtractSaturateUnsignedI8 => "psubusb",
            Self::AverageUnsignedI8 => "pavgb",
            Self::AddSaturateSignedI16 => "paddsw",
            Self::AddSaturateUnsignedI16 => "paddusw",
            Self::SubtractSaturateSignedI16 => "psubsw",
            Self::SubtractSaturateUnsignedI16 => "psubusw",
            Self::AverageUnsignedI16 => "pavgw",
            Self::MultiplyI16 => "pmullw",
            Self::MultiplyI32 => "pmulld",
            Self::MultiplyUnsignedLowI32 => "pmuludq",
            Self::MultiplyHighSignedI16 => "pmulhw",
            Self::MultiplyHighUnsignedI16 => "pmulhuw",
            Self::MultiplyHighRoundScaleI16 => "pmulhrsw",
            Self::MultiplyAddI16 => "pmaddwd",
            Self::MultiplyAddUnsignedSignedI8 => "pmaddubsw",
            Self::MinimumSignedI8 => "pminsb",
            Self::MinimumUnsignedI8 => "pminub",
            Self::MaximumSignedI8 => "pmaxsb",
            Self::MaximumUnsignedI8 => "pmaxub",
            Self::MinimumSignedI16 => "pminsw",
            Self::MinimumUnsignedI16 => "pminuw",
            Self::MaximumSignedI16 => "pmaxsw",
            Self::MaximumUnsignedI16 => "pmaxuw",
            Self::MinimumSignedI32 => "pminsd",
            Self::MinimumUnsignedI32 => "pminud",
            Self::MaximumSignedI32 => "pmaxsd",
            Self::MaximumUnsignedI32 => "pmaxud",
            Self::ShiftLeftI16 => "psllw",
            Self::ShiftRightUnsignedI16 => "psrlw",
            Self::ShiftLeftI32 => "pslld",
            Self::ShiftRightUnsignedI32 => "psrld",
            Self::ShiftLeftI64 => "psllq",
            Self::ShiftRightUnsignedI64 => "psrlq",
            Self::ShiftRightSignedI16 => "psraw",
            Self::ShiftRightSignedI32 => "psrad",
            Self::Shuffle8 => "pshufb",
            Self::NarrowSignedI16 => "packsswb",
            Self::NarrowUnsignedI16 => "packuswb",
            Self::NarrowSignedI32 => "packssdw",
            Self::NarrowUnsignedI32 => "packusdw",
            Self::InterleaveLowI8 => "punpcklbw",
            Self::InterleaveHighI8 => "punpckhbw",
            Self::InterleaveLowI16 => "punpcklwd",
            Self::InterleaveHighI16 => "punpckhwd",
            Self::InterleaveLowI32 => "punpckldq",
            Self::InterleaveHighI32 => "punpckhdq",
            Self::InterleaveLowI64 => "punpcklqdq",
            Self::InterleaveHighI64 => "punpckhqdq",
            Self::AddF32 => "addps",
            Self::SubtractF32 => "subps",
            Self::MultiplyF32 => "mulps",
            Self::DivideF32 => "divps",
            Self::MinimumF32 => "minps",
            Self::MaximumF32 => "maxps",
            Self::EqualF32 => "cmpeqps",
            Self::NotEqualF32 => "cmpneqps",
            Self::LessF32 => "cmpltps",
            Self::LessOrEqualF32 => "cmpleps",
            Self::UnorderedF32 => "cmpunordps",
            Self::SquareRootF32 => "sqrtps",
            Self::CeilF32 => "roundps",
            Self::FloorF32 => "roundps",
            Self::TruncF32 => "roundps",
            Self::NearestF32 => "roundps",
            Self::AddF64 => "addpd",
            Self::SubtractF64 => "subpd",
            Self::MultiplyF64 => "mulpd",
            Self::DivideF64 => "divpd",
            Self::MinimumF64 => "minpd",
            Self::MaximumF64 => "maxpd",
            Self::EqualF64 => "cmpeqpd",
            Self::NotEqualF64 => "cmpneqpd",
            Self::LessF64 => "cmpltpd",
            Self::LessOrEqualF64 => "cmplepd",
            Self::UnorderedF64 => "cmpunordpd",
            Self::SquareRootF64 => "sqrtpd",
            Self::CeilF64 => "roundpd",
            Self::FloorF64 => "roundpd",
            Self::TruncF64 => "roundpd",
            Self::NearestF64 => "roundpd",
            Self::AbsoluteI8 => "pabsb",
            Self::AbsoluteI16 => "pabsw",
            Self::AbsoluteI32 => "pabsd",
            Self::ExtendLowSignedI8 => "pmovsxbw",
            Self::ExtendLowUnsignedI8 => "pmovzxbw",
            Self::ExtendLowSignedI16 => "pmovsxwd",
            Self::ExtendLowUnsignedI16 => "pmovzxwd",
            Self::ExtendLowSignedI32 => "pmovsxdq",
            Self::ExtendLowUnsignedI32 => "pmovzxdq",
            Self::ConvertSignedI32ToF32 => "cvtdq2ps",
            Self::TruncateF32ToSignedI32 => "cvttps2dq",
            Self::ConvertLowSignedI32ToF64 => "cvtdq2pd",
            Self::TruncateF64ToSignedI32 => "cvttpd2dq",
            Self::PromoteLowF32 => "cvtps2pd",
            Self::DemoteF64 => "cvtpd2ps",
            Self::HighHalf => "pshufd",
            Self::BroadcastLowI32 => "pshufd",
            Self::Low64 => "movq",
            Self::High64 => "pextrq",
            Self::ByteMask => "pmovmskb",
            Self::SignMaskF32 => "movmskps",
            Self::SignMaskF64 => "movmskpd",
        }
    }

    /// The immediate operand the x86-64 instruction takes, if any.
    pub(crate) fn x86_64_immediate(self) -> Option<i64> {
        match self {
            Self::CeilF32 => Some(10),
            Self::FloorF32 => Some(9),
            Self::TruncF32 => Some(11),
            Self::NearestF32 => Some(8),
            Self::CeilF64 => Some(10),
            Self::FloorF64 => Some(9),
            Self::TruncF64 => Some(11),
            Self::NearestF64 => Some(8),
            Self::HighHalf => Some(238),
            Self::BroadcastLowI32 => Some(0),
            Self::High64 => Some(1),
            _ => None,
        }
    }
}

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum AddressOperation {
        SequenceElement => "sequence_element_address";
        BytecodeField => "bytecode_field_address";
        EmbeddedField => "embedded_field_address";
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum BytecodeOperation {
    Load(FieldWidth),
    LoadInlineInt32,
}

impl BytecodeOperation {
    pub(crate) fn access_width(self) -> u8 {
        match self {
            Self::Load(FieldWidth::U8) => 1,
            Self::Load(FieldWidth::U16) => 2,
            Self::Load(FieldWidth::U32) => 4,
            Self::Load(FieldWidth::U64) => 8,
            Self::LoadInlineInt32 => 4,
        }
    }
}

intrinsic_names!(BytecodeOperation {
    Self::Load(FieldWidth::U8) => "load_bytecode_u8";
    Self::Load(FieldWidth::U16) => "load_bytecode_u16";
    Self::Load(FieldWidth::U32) => "load_bytecode_u32";
    Self::Load(FieldWidth::U64) => "load_bytecode_u64";
    Self::LoadInlineInt32 => "load_inline_int32";
});

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum AggregateOperation {
        Copy16 => "copy16_field";
        Zero16 => "zero16_field";
    }
}

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum ClassificationOperation {
        Float64Tag => "is_f64_tag";
        NumberTag => "is_number_tag";
        Int32Pair => "is_int32_pair";
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ComparisonDomain {
    Value,
    SignedInteger,
    UnsignedInteger,
}

/// A comparison of two floats with IEEE semantics: false if either is NaN, except for inequality, which is true.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum OrderedFloatComparison {
    Equal,
    NotEqual,
    Less,
    LessOrEqual,
    Greater,
    GreaterOrEqual,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum IntegerComparisonOperation {
    Equal,
    NotEqual,
    Relational {
        relation: ComparisonRelation,
        domain: ComparisonDomain,
    },
    BitsSet,
    BitsClear,
    Float(OrderedFloatComparison),
}

intrinsic_names!(IntegerComparisonOperation {
    Self::Equal => "equal";
    Self::NotEqual => "not_equal";
    Self::Relational { relation: ComparisonRelation::Less, domain: ComparisonDomain::Value } => "less_than";
    Self::Relational { relation: ComparisonRelation::Less, domain: ComparisonDomain::SignedInteger } => "less_than_signed";
    Self::Relational { relation: ComparisonRelation::Less, domain: ComparisonDomain::UnsignedInteger } => "less_than_unsigned";
    Self::Relational { relation: ComparisonRelation::LessOrEqual, domain: ComparisonDomain::Value } => "less_than_or_equal";
    Self::Relational { relation: ComparisonRelation::LessOrEqual, domain: ComparisonDomain::SignedInteger } => "less_than_or_equal_signed";
    Self::Relational { relation: ComparisonRelation::LessOrEqual, domain: ComparisonDomain::UnsignedInteger } => "less_than_or_equal_unsigned";
    Self::Relational { relation: ComparisonRelation::Greater, domain: ComparisonDomain::Value } => "greater_than";
    Self::Relational { relation: ComparisonRelation::Greater, domain: ComparisonDomain::SignedInteger } => "greater_than_signed";
    Self::Relational { relation: ComparisonRelation::Greater, domain: ComparisonDomain::UnsignedInteger } => "greater_than_unsigned";
    Self::Relational { relation: ComparisonRelation::GreaterOrEqual, domain: ComparisonDomain::Value } => "greater_than_or_equal";
    Self::Relational { relation: ComparisonRelation::GreaterOrEqual, domain: ComparisonDomain::SignedInteger } => "greater_than_or_equal_signed";
    Self::Relational { relation: ComparisonRelation::GreaterOrEqual, domain: ComparisonDomain::UnsignedInteger } => "greater_than_or_equal_unsigned";
    Self::BitsSet => "bits_set";
    Self::BitsClear => "bits_clear";
    Self::Float(OrderedFloatComparison::Equal) => "float_equal";
    Self::Float(OrderedFloatComparison::NotEqual) => "float_not_equal";
    Self::Float(OrderedFloatComparison::Less) => "float_less";
    Self::Float(OrderedFloatComparison::LessOrEqual) => "float_less_or_equal";
    Self::Float(OrderedFloatComparison::Greater) => "float_greater";
    Self::Float(OrderedFloatComparison::GreaterOrEqual) => "float_greater_or_equal";
});

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatBinaryOperation {
    Add,
    Subtract,
    Multiply,
    Divide,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatUnaryOperation {
    Floor,
    Ceil,
    SquareRoot,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatConversion {
    Int32ToFloat64,
    Int64ToFloat64,
    Uint32ToFloat64,
    Float32ToFloat64,
    Float64ToFloat32,
    Float64ToInt32,
    Float64ToInt64,
    JavaScriptToInt32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum FloatingPointOperation {
    Binary(FloatBinaryOperation),
    Unary(FloatUnaryOperation),
    Convert(FloatConversion),
    CanonicalizeNan,
}

impl FloatingPointOperation {
    pub(crate) fn is_fallible(self) -> bool {
        matches!(
            self,
            Self::Convert(
                FloatConversion::Float64ToInt32 | FloatConversion::Float64ToInt64 | FloatConversion::JavaScriptToInt32
            )
        )
    }
}

intrinsic_names!(FloatingPointOperation {
    Self::Binary(FloatBinaryOperation::Add) => "add_f64" signatures [signature!([InOut F64, In F64])];
    Self::Binary(FloatBinaryOperation::Subtract) => "sub_f64" signatures [signature!([InOut F64, In F64])];
    Self::Binary(FloatBinaryOperation::Multiply) => "mul_f64" signatures [signature!([InOut F64, In F64])];
    Self::Binary(FloatBinaryOperation::Divide) => "div_f64" signatures [signature!([InOut F64, In F64])];
    Self::Unary(FloatUnaryOperation::Floor) => "fp_floor" signatures [signature!([In F64] -> F64), signature!([Out F64, In F64])];
    Self::Unary(FloatUnaryOperation::Ceil) => "fp_ceil" signatures [signature!([In F64] -> F64), signature!([Out F64, In F64])];
    Self::Unary(FloatUnaryOperation::SquareRoot) => "fp_sqrt" signatures [signature!([In F64] -> F64), signature!([Out F64, In F64])];
    Self::Convert(FloatConversion::Int64ToFloat64) => "i64_to_f64" signatures [signature!([In I64] -> F64)];
    Self::Convert(FloatConversion::Float64ToInt64) => "double_to_int64" signatures [signature!(fallible [In F64] -> I64)];
    Self::Convert(FloatConversion::Int32ToFloat64) => "to_f64" signatures [signature!([In I32] -> F64)];
    Self::Convert(FloatConversion::Uint32ToFloat64) => "u32_to_f64" signatures [signature!([In U32] -> F64), signature!([Out F64, In U32])];
    Self::Convert(FloatConversion::Float32ToFloat64) => "float_to_double" signatures [signature!([In F32] -> F64), signature!([Out F64, In F32])];
    Self::Convert(FloatConversion::Float64ToFloat32) => "to_f32" signatures [signature!([In F64] -> F32)];
    Self::Convert(FloatConversion::Float64ToInt32) => "double_to_int32" signatures [signature!(fallible [In F64] -> I32), signature!(fallible [Out AnyGpr, In F64])];
    Self::Convert(FloatConversion::JavaScriptToInt32) => "js_to_int32" signatures [signature!(fallible [In F64] -> I32), signature!(fallible [Out AnyGpr, In F64])];
    Self::CanonicalizeNan => "canonicalize_nan" signatures [signature!([In F64] -> Value), signature!([Out Value, In F64])];
});

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum OperandLoad {
    Field,
    Index,
    RematerializedIndex,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum OperandStore {
    Field,
    Index,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum OperandOperation {
    Decode,
    Load(OperandLoad),
    LoadPair,
    Store(OperandStore),
    Copy,
}

impl OperandOperation {
    pub(crate) fn reads_machine_state(self) -> bool {
        matches!(
            self,
            Self::Load(OperandLoad::Field) | Self::LoadPair | Self::Store(OperandStore::Field) | Self::Copy
        )
    }
}

intrinsic_names!(OperandOperation {
    Self::Decode => "decode_operand";
    Self::Load(OperandLoad::Field) => "load" signatures [signature!([In Operand] -> Value)];
    Self::Load(OperandLoad::Index) => "load_indexed_operand";
    Self::Load(OperandLoad::RematerializedIndex) => "rematerialize_load_indexed_operand" from [];
    Self::LoadPair => "load_operand_pair";
    Self::Store(OperandStore::Field) => "store" signatures [signature!([Out Operand, In Value])];
    Self::Store(OperandStore::Index) => "store_indexed_operand";
    Self::Copy => "copy_operand";
});

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum CallOperation {
        SlowPath => "call_slow_path" signatures [signature!([In SlowPath])];
        BinarySlowPath => "call_binary_slow_path" signatures [signature!([In SlowPath, Out Operand, In Value, In Value])];
        JumpSlowPath => "call_jump_slow_path" signatures [signature!([In SlowPath, In Value, In Value, In BytecodeOffset, In BytecodeOffset])];
        Interpreter => "call_interp" signatures [signature!([In FunctionSymbol] -> I32), signature!([In FunctionSymbol, Out AnyGpr])];
        Helper => "call_helper" signatures [signature!([In FunctionSymbol, In AnyGpr, Out AnyGpr])];
        HelperWithTwoArguments => "call_helper_with_two_arguments" signatures [signature!([In FunctionSymbol, In AnyGpr, In AnyGpr, Out AnyGpr])];
        RawNative => "call_raw_native" signatures [signature!([In AnyGpr] -> (Value, U64)), signature!([In AnyGpr, Out AnyGpr, Out AnyGpr])];
    }
}

impl CallOperation {
    pub(crate) fn result_count(self) -> usize {
        match self {
            Self::SlowPath | Self::BinarySlowPath | Self::JumpSlowPath => 0,
            Self::Interpreter | Self::Helper | Self::HelperWithTwoArguments => 1,
            Self::RawNative => 2,
        }
    }
}

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum ControlOperation {
        DispatchNext => "dispatch_next" signatures [signature!([])];
        Dispatch => "dispatch" signatures [signature!([])];
        DispatchVariable => "dispatch_variable" signatures [signature!([In AnyGpr])];
        GotoHandler => "goto_handler" signatures [signature!([In AnyGpr])];
        DispatchSecondary => "dispatch_secondary" signatures [signature!([In AnyGpr, In AnyGpr])];
        Exit => "exit" signatures [signature!([])];
        JumpBytecode => "jump" signatures [signature!([In BytecodeOffset])];
        JumpLabel => "jmp" signatures [signature!([In Label])];
    }
}

impl ControlOperation {
    pub(crate) fn writes_machine_state(self) -> bool {
        matches!(
            self,
            Self::DispatchNext
                | Self::DispatchVariable
                | Self::GotoHandler
                | Self::DispatchSecondary
                | Self::Exit
                | Self::JumpBytecode
        )
    }

    pub(crate) fn is_terminal(self) -> bool {
        true
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum MemoryOperation {
    Load { width: MemoryWidth, signed: bool },
    Load64NonZero,
    LoadCellPointer,
    LoadNonnullCellPointer,
    Store(MemoryWidth),
    LoadPair(PairWidth),
    StorePair(PairWidth),
}

intrinsic_names!(MemoryOperation {
    Self::Load { width: MemoryWidth::Byte, signed: false } => "load8" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load { width: MemoryWidth::Byte, signed: true } => "load8s" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load { width: MemoryWidth::HalfWord, signed: false } => "load16" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load { width: MemoryWidth::HalfWord, signed: true } => "load16s" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load { width: MemoryWidth::Word, signed: false } => "load32" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load { width: MemoryWidth::DoubleWord, signed: false } => "load64" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load64NonZero => "load64_nonzero" signatures [signature!([Out AnyGpr, In Memory])];
    Self::LoadCellPointer => "load_cell_ptr" signatures [signature!([Out AnyGpr, In Memory])];
    Self::LoadNonnullCellPointer => "load_nonnull_cell_ptr" signatures [signature!([Out AnyGpr, In Memory])];
    Self::Load { width: MemoryWidth::Float, signed: false } => "loadf32" signatures [signature!([Out F32, In Memory])];
    Self::Load { width: MemoryWidth::Double, signed: false } => "loadf64" signatures [signature!([Out F64, In Memory])];
    Self::Load { width: MemoryWidth::Vector, signed: false } => "load128" signatures [signature!([Out V128, In Memory])];
    Self::LoadPair(PairWidth::Word) => "load_pair32" signatures [signature!([In Memory, In Memory] -> (U32, U32)), signature!([Out AnyGpr, Out AnyGpr, In Memory, In Memory])];
    Self::LoadPair(PairWidth::DoubleWord) => "load_pair64" signatures [signature!([In Memory, In Memory] -> (U64, U64)), signature!([Out AnyGpr, Out AnyGpr, In Memory, In Memory])];
    Self::Store(MemoryWidth::Byte) => "store8" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::HalfWord) => "store16" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::Word) => "store32" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::DoubleWord) => "store64" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::Float) => "storef32" signatures [signature!([In Memory, In F32])];
    Self::Store(MemoryWidth::Double) => "storef64" signatures [signature!([In Memory, In F64])];
    Self::Store(MemoryWidth::Vector) => "store128" signatures [signature!([In Memory, In V128])];
    Self::StorePair(PairWidth::Word) => "store_pair32" signatures [signature!([In Memory, In Memory, In AnyGpr, In AnyGpr])];
    Self::StorePair(PairWidth::DoubleWord) => "store_pair64" signatures [signature!([In Memory, In Memory, In AnyGpr, In AnyGpr])];
});

impl MemoryOperation {
    pub(crate) fn effects(self) -> IntrinsicEffects {
        IntrinsicEffects {
            memory: if self.writes() { ModRef::Write } else { ModRef::Read },
            may_trap: matches!(self, Self::Load64NonZero | Self::LoadNonnullCellPointer),
            ..IntrinsicEffects::PURE
        }
    }

    pub(crate) fn access_width(self) -> u8 {
        match self.width() {
            MemoryWidth::Byte => 1,
            MemoryWidth::HalfWord => 2,
            MemoryWidth::Word | MemoryWidth::Float => 4,
            MemoryWidth::DoubleWord | MemoryWidth::Double => 8,
            MemoryWidth::Vector => 16,
        }
    }

    pub(crate) fn width(self) -> MemoryWidth {
        match self {
            Self::Load { width, .. } | Self::Store(width) => width,
            Self::Load64NonZero | Self::LoadCellPointer | Self::LoadNonnullCellPointer => MemoryWidth::DoubleWord,
            Self::LoadPair(PairWidth::Word) | Self::StorePair(PairWidth::Word) => MemoryWidth::Word,
            Self::LoadPair(PairWidth::DoubleWord) | Self::StorePair(PairWidth::DoubleWord) => MemoryWidth::DoubleWord,
        }
    }

    pub(crate) fn address_count(self) -> usize {
        if matches!(self, Self::LoadPair(_) | Self::StorePair(_)) {
            2
        } else {
            1
        }
    }

    pub(crate) fn writes(self) -> bool {
        matches!(self, Self::Store(_) | Self::StorePair(_))
    }
}

macro_rules! define_intrinsic_families {
    ($($variant:ident($operation:ty)),+ $(,)?) => {
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub(crate) enum Intrinsic {
            $($variant($operation)),+
        }

        impl Intrinsic {
            fn lookup(name: &str) -> Option<Self> {
                $(
                    if let Some(operation) = <$operation>::from_name(name) {
                        return Some(Self::$variant(operation));
                    }
                )+
                None
            }

            pub(crate) fn name(self) -> &'static str {
                match self {
                    $(Self::$variant(operation) => operation.name()),+
                }
            }

            pub(crate) fn call_signature(
                self,
                argument_count: usize,
            ) -> Option<IntrinsicCallSignature> {
                match self {
                    $(Self::$variant(operation) => operation.call_signature(argument_count)),+
                }
            }
        }
    };
}

define_intrinsic_families! {
    Address(AddressOperation),
    Aggregate(AggregateOperation),
    Assertion(AssertionOperation),
    Branch(BranchOperation),
    Bytecode(BytecodeOperation),
    Call(CallOperation),
    CheckedInteger(CheckedIntegerOperation),
    Classification(ClassificationOperation),
    Control(ControlOperation),
    FloatingPoint(FloatingPointOperation),
    IntegerBinary(IntegerBinaryOperation),
    IntegerComparison(IntegerComparisonOperation),
    LowLevel(LowLevelOperation),
    Memory(MemoryOperation),
    Numeric(NumericOperation),
    Atomic(AtomicOperation),
    Operand(OperandOperation),
    Vector(VectorOperation),
    Value(ValueOperation),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum IntrinsicValueType {
    AnyGpr,
    BytecodeOffset,
    F32,
    F64,
    FunctionSymbol,
    I32,
    I64,
    Label,
    Memory,
    Operand,
    SlowPath,
    U8,
    U16,
    U32,
    U64,
    V128,
    Value,
    ValueTag,
    Vm,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum IntrinsicResultType {
    Single(IntrinsicValueType),
    Pair(IntrinsicValueType, IntrinsicValueType),
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct IntrinsicParameter {
    pub(crate) mode: ParameterMode,
    pub(crate) ty: IntrinsicValueType,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) struct IntrinsicCallSignature {
    parameters: &'static [IntrinsicParameter],
    pub(crate) has_else_parameter: bool,
    pub(crate) result: Option<IntrinsicResultType>,
}

impl IntrinsicCallSignature {
    pub(crate) fn parameters(&self) -> &[IntrinsicParameter] {
        self.parameters
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) enum OperationValue {
    Intrinsic(Intrinsic),
    InlineFunction(InlineFunctionId),
}

const fn intrinsic_parameter(mode: ParameterMode, ty: IntrinsicValueType) -> IntrinsicParameter {
    IntrinsicParameter { mode, ty }
}

impl Intrinsic {
    pub(crate) fn from_name(name: &str) -> Option<Self> {
        Self::lookup(name)
    }

    pub(crate) fn effects(self) -> IntrinsicEffects {
        match self {
            Self::Aggregate(operation) => IntrinsicEffects {
                memory: match operation {
                    AggregateOperation::Copy16 => ModRef::ReadWrite,
                    AggregateOperation::Zero16 => ModRef::Write,
                },
                ..IntrinsicEffects::PURE
            },
            Self::Assertion(_) => IntrinsicEffects {
                may_trap: true,
                ..IntrinsicEffects::PURE
            },
            Self::Bytecode(operation) => IntrinsicEffects {
                memory: ModRef::Read,
                machine_state: if matches!(
                    operation,
                    BytecodeOperation::Load(FieldWidth::U8 | FieldWidth::U32) | BytecodeOperation::LoadInlineInt32
                ) {
                    ModRef::Read
                } else {
                    ModRef::None
                },
                ..IntrinsicEffects::PURE
            },
            Self::Call(_) => IntrinsicEffects::UNKNOWN,
            Self::Control(operation) if operation.writes_machine_state() => IntrinsicEffects {
                machine_state: ModRef::ReadWrite,
                ..IntrinsicEffects::PURE
            },
            Self::Control(_) => IntrinsicEffects::UNKNOWN,
            Self::LowLevel(
                LowLevelOperation::Increment32Memory
                | LowLevelOperation::Or8Memory
                | LowLevelOperation::Or32Memory
                | LowLevelOperation::Subtract32Memory,
            ) => IntrinsicEffects {
                memory: ModRef::ReadWrite,
                ..IntrinsicEffects::PURE
            },
            Self::LowLevel(LowLevelOperation::LoadVm) => IntrinsicEffects {
                machine_state: ModRef::Read,
                ..IntrinsicEffects::PURE
            },
            Self::Memory(operation) => operation.effects(),
            Self::Atomic(_) => IntrinsicEffects {
                memory: ModRef::ReadWrite,
                ..IntrinsicEffects::PURE
            },
            Self::Operand(operation) => IntrinsicEffects {
                memory: match operation {
                    OperandOperation::Decode => ModRef::None,
                    OperandOperation::Load(_) | OperandOperation::LoadPair => ModRef::Read,
                    OperandOperation::Store(_) => ModRef::Write,
                    OperandOperation::Copy => ModRef::ReadWrite,
                },
                machine_state: if operation.reads_machine_state() {
                    ModRef::Read
                } else {
                    ModRef::None
                },
                ..IntrinsicEffects::PURE
            },
            Self::LowLevel(
                LowLevelOperation::LoadLabel
                | LowLevelOperation::LoadEffectiveAddress
                | LowLevelOperation::Add
                | LowLevelOperation::Subtract,
            )
            | Self::Branch(_) => IntrinsicEffects::UNKNOWN,
            Self::Address(_)
            | Self::CheckedInteger(_)
            | Self::Classification(_)
            | Self::FloatingPoint(_)
            | Self::IntegerBinary(_)
            | Self::IntegerComparison(_)
            | Self::LowLevel(_)
            | Self::Numeric(_)
            | Self::Value(_)
            | Self::Vector(_) => IntrinsicEffects::PURE,
        }
    }

    pub(crate) fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Control(operation) if operation.is_terminal()
        ) || matches!(
            self,
            Self::Call(CallOperation::SlowPath | CallOperation::BinarySlowPath | CallOperation::JumpSlowPath)
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn describes_intrinsic_effects_without_source_spellings() {
        assert_eq!(
            Intrinsic::Value(ValueOperation::BoxFloat64).effects(),
            IntrinsicEffects::PURE
        );
        assert_eq!(
            Intrinsic::Operand(OperandOperation::Load(OperandLoad::Field))
                .effects()
                .memory,
            ModRef::Read
        );
        assert_eq!(
            Intrinsic::Operand(OperandOperation::Store(OperandStore::Field))
                .effects()
                .memory,
            ModRef::Write
        );
        assert_eq!(
            Intrinsic::LowLevel(LowLevelOperation::LoadVm).effects().machine_state,
            ModRef::Read
        );
        assert!(Intrinsic::Assertion(AssertionOperation::Assert).effects().may_trap);
        assert_eq!(
            Intrinsic::Call(CallOperation::Interpreter).effects(),
            IntrinsicEffects::UNKNOWN
        );
        assert_eq!(
            Intrinsic::Control(ControlOperation::DispatchNext).effects(),
            IntrinsicEffects {
                machine_state: ModRef::ReadWrite,
                ..IntrinsicEffects::PURE
            }
        );
    }

    #[test]
    fn describes_structured_field_access_effects() {
        assert_eq!(
            FieldAccess {
                kind: FieldAccessKind::Load {
                    width: FieldWidth::U64,
                    nonzero: true,
                },
                pair: None,
            }
            .effects(),
            IntrinsicEffects {
                memory: ModRef::Read,
                may_trap: true,
                ..IntrinsicEffects::PURE
            }
        );
    }

    #[test]
    fn resolves_typed_intrinsic_call_signatures() {
        let intrinsic = Intrinsic::from_name("checked_add").unwrap();
        assert_eq!(intrinsic, Intrinsic::CheckedInteger(CheckedIntegerOperation::Add));
        let checked = intrinsic.call_signature(2).unwrap();
        assert_eq!(
            checked.result,
            Some(IntrinsicResultType::Single(IntrinsicValueType::I32))
        );

        assert!(Intrinsic::from_name("to_f64").unwrap().call_signature(1).is_some());
        assert!(Intrinsic::from_name("to_f64").unwrap().call_signature(2).is_none());
        assert!(Intrinsic::from_name("call_interp").unwrap().call_signature(1).is_some());
        assert!(Intrinsic::from_name("call_interp_result").is_none());
        assert!(Intrinsic::from_name("xor_integer").is_none());
    }

    #[test]
    fn rejects_removed_intrinsic_aliases() {
        for name in [
            "and_i32",
            "add_integer",
            "sub_integer",
            "mul_integer",
            "and_integer",
            "or_i32",
            "or_integer",
            "xor_i32",
            "xor_integer",
            "shl_i32",
            "shl_integer",
            "shr_integer",
            "sar_i32",
            "sar_integer",
            "box_u32_clean",
            "box_int32_clean",
            "reinterpret_f64_as_value",
            "reinterpret_value_as_f64",
            "unbox_int32",
            "fp_add",
            "fp_sub",
            "fp_mul",
            "fp_div",
            "int_to_double",
            "double_to_float",
            "call_interp_result",
            "call_helper_result",
            "call_raw_native_result",
            "unbox_pointer",
        ] {
            assert!(Intrinsic::from_name(name).is_none(), "{name}");
        }
    }

    #[test]
    fn rejects_internal_operation_names_as_source_intrinsics() {
        for name in [
            "rematerialize_unbox_i32",
            "rematerialize_extract_tag",
            "rematerialize_load",
            "rematerialize_load_indexed_operand",
            "bitwise_not_i32",
        ] {
            assert!(Intrinsic::from_name(name).is_none(), "{name}");
        }
    }
}
