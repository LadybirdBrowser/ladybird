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
        !self.memory.writes()
            && !self.machine_state.writes()
            && !self.may_call
            && !self.may_trap
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
    Store(FieldWidth),
}

impl FieldAccessKind {
    pub(crate) fn width(self) -> FieldWidth {
        match self {
            Self::Load { width, .. } | Self::Store(width) => width,
        }
    }

    pub(crate) fn writes(self) -> bool {
        matches!(self, Self::Store(_))
    }

    pub(crate) fn nonzero(self) -> bool {
        matches!(self, Self::Load { nonzero: true, .. })
    }

    pub(crate) fn memory_operation(self) -> MemoryOperation {
        match self {
            Self::Load { width: FieldWidth::U64, nonzero: true } => MemoryOperation::Load64NonZero,
            Self::Load { width, .. } => MemoryOperation::Load {
                width: width.into(),
                signed: false,
            },
            Self::Store(width) => MemoryOperation::Store(width.into()),
        }
    }

    pub(crate) fn paired_operation(self) -> Option<MemoryOperation> {
        match self {
            Self::Load { width: FieldWidth::U64, .. } => Some(MemoryOperation::LoadPair(PairWidth::DoubleWord)),
            Self::Load { width: FieldWidth::U32, nonzero: false } => Some(MemoryOperation::LoadPair(PairWidth::Word)),
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
    Float,
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
            Self::Equality { width, condition } => Self::Equality { width, condition: condition.inverted() },
            Self::Ordered { width, relation, signedness } => Self::Ordered { width, relation: ordered(relation), signedness },
            Self::Zero { width, condition } => Self::Zero { width, condition: condition.inverted() },
            Self::Sign { width, condition } => Self::Sign { width, condition: condition.inverted() },
            Self::Bits { width, condition } => Self::Bits { width, condition: condition.inverted() },
            Self::Bit(condition) => Self::Bit(condition.inverted()),
            Self::Tag(condition) => Self::Tag(condition.inverted()),
            Self::Singleton(condition) => Self::Singleton(condition.inverted()),
            Self::Memory { width, condition } => Self::Memory { width, condition: condition.inverted() },
            Self::ValueBitsNegative => Self::Sign { width: IntegerWidth::U64, condition: SignCondition::NotNegative },
            Self::Float(_) | Self::AnyEqual => return None,
        })
    }
}

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum AssertionOperation {
        NonZero => "assert_nonzero" signatures [signature!([In AnyGpr])];
        UnsignedLess => "assert_lt_unsigned" signatures [signature!([In AnyGpr, In AnyGpr])];
        UnsignedGreaterOrEqual => "assert_ge_unsigned" signatures [signature!([In AnyGpr, In AnyGpr])];
        TagEqual => "assert_tag" signatures [signature!([In AnyGpr, In AnyGpr])];
        TagNotEqual => "assert_not_tag" signatures [signature!([In AnyGpr, In AnyGpr])];
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
    ToUint32,
    LogicalNot,
    UnboxObject,
}

impl ValueOperation {
    pub(crate) fn is_rematerialized(self) -> bool {
        matches!(
            self,
            Self::UnboxInt32 { rematerialized: true }
                | Self::ExtractTag { rematerialized: true }
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
    Self::ToInt32 => "i32" signatures [signature!([In AnyGpr] -> I32)];
    Self::ToUint32 => "u32" signatures [signature!([In AnyGpr] -> U32)];
    Self::LogicalNot => "not_bool";
    Self::UnboxObject => "unbox_object" signatures [signature!([Out AnyGpr, In AnyGpr])];
});

define_named_intrinsic_enum! {
    #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
    pub(crate) enum LowLevelOperation {
        LoadLabel => "load_label" signatures [signature!([Out BytecodeOffset, In BytecodeOffset])];
        DivideModulo => "divmod" signatures [signature!([In I32, In I32] -> (I32, I32)), signature!([Out I32, Out I32, In I32, In I32])];
        Move => "mov" signatures [signature!([Out AnyGpr, In AnyGpr])];
        LoadEffectiveAddress => "lea" signatures [signature!([Out AnyGpr, In Memory])];
        LoadVm => "load_vm" signatures [signature!([] -> Vm), signature!([Out AnyGpr])];
        Increment32Memory => "inc32_mem" signatures [signature!([In Memory])];
        ClearBit => "clear_bit" signatures [signature!([InOut AnyGpr, In AnyGpr])];
        ToggleBit => "toggle_bit" signatures [signature!([InOut AnyGpr, In AnyGpr])];
        Negate => "neg" signatures [signature!([InOut AnyGpr])];
        BitwiseNotInt32 => "bitwise_not_i32" from [];
        Add => "add" signatures [signature!([InOut AnyGpr, In AnyGpr])];
        Subtract => "sub" signatures [signature!([InOut AnyGpr, In AnyGpr])];
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
}

impl BytecodeOperation {
    pub(crate) fn access_width(self) -> u8 {
        match self {
            Self::Load(FieldWidth::U8) => 1,
            Self::Load(FieldWidth::U16) => 2,
            Self::Load(FieldWidth::U32) => 4,
            Self::Load(FieldWidth::U64) => 8,
        }
    }
}

intrinsic_names!(BytecodeOperation {
    Self::Load(FieldWidth::U8) => "load_bytecode_u8";
    Self::Load(FieldWidth::U16) => "load_bytecode_u16";
    Self::Load(FieldWidth::U32) => "load_bytecode_u32";
    Self::Load(FieldWidth::U64) => "load_bytecode_u64";
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
    Uint32ToFloat64,
    Float32ToFloat64,
    Float64ToFloat32,
    Float64ToInt32,
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
            Self::Convert(FloatConversion::Float64ToInt32 | FloatConversion::JavaScriptToInt32)
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
            Self::Load(OperandLoad::Field) | Self::LoadPair
                | Self::Store(OperandStore::Field)
                | Self::Copy
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
        Interpreter => "call_interp" signatures [signature!([In FunctionSymbol] -> I32), signature!([In FunctionSymbol, Out AnyGpr])];
        Helper => "call_helper" signatures [signature!([In FunctionSymbol, In AnyGpr, Out AnyGpr])];
        RawNative => "call_raw_native" signatures [signature!([In AnyGpr] -> (Value, U64)), signature!([In AnyGpr, Out AnyGpr, Out AnyGpr])];
    }
}

impl CallOperation {
    pub(crate) fn result_count(self) -> usize {
        match self {
            Self::SlowPath => 0,
            Self::Interpreter | Self::Helper => 1,
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
        Exit => "exit" signatures [signature!([])];
        JumpBytecode => "jump" signatures [signature!([In BytecodeOffset])];
        JumpLabel => "jmp" signatures [signature!([In Label])];
    }
}

impl ControlOperation {
    pub(crate) fn writes_machine_state(self) -> bool {
        matches!(
            self,
            Self::DispatchNext | Self::DispatchVariable | Self::GotoHandler
                | Self::Exit | Self::JumpBytecode
        )
    }

    pub(crate) fn is_terminal(self) -> bool {
        true
    }
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum MemoryOperation {
    Load {
        width: MemoryWidth,
        signed: bool,
    },
    Load64NonZero,
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
    Self::Load { width: MemoryWidth::Float, signed: false } => "loadf32" signatures [signature!([Out F32, In Memory])];
    Self::LoadPair(PairWidth::Word) => "load_pair32" signatures [signature!([In Memory, In Memory] -> (U32, U32)), signature!([Out AnyGpr, Out AnyGpr, In Memory, In Memory])];
    Self::LoadPair(PairWidth::DoubleWord) => "load_pair64" signatures [signature!([In Memory, In Memory] -> (U64, U64)), signature!([Out AnyGpr, Out AnyGpr, In Memory, In Memory])];
    Self::Store(MemoryWidth::Byte) => "store8" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::HalfWord) => "store16" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::Word) => "store32" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::DoubleWord) => "store64" signatures [signature!([In Memory, In AnyGpr])];
    Self::Store(MemoryWidth::Float) => "storef32" signatures [signature!([In Memory, In F32])];
    Self::StorePair(PairWidth::Word) => "store_pair32" signatures [signature!([In Memory, In Memory, In AnyGpr, In AnyGpr])];
    Self::StorePair(PairWidth::DoubleWord) => "store_pair64" signatures [signature!([In Memory, In Memory, In AnyGpr, In AnyGpr])];
});

impl MemoryOperation {
    pub(crate) fn effects(self) -> IntrinsicEffects {
        IntrinsicEffects {
            memory: if self.writes() { ModRef::Write } else { ModRef::Read },
            may_trap: self == Self::Load64NonZero,
            ..IntrinsicEffects::PURE
        }
    }

    pub(crate) fn access_width(self) -> u8 {
        match self.width() {
            MemoryWidth::Byte => 1,
            MemoryWidth::HalfWord => 2,
            MemoryWidth::Word | MemoryWidth::Float => 4,
            MemoryWidth::DoubleWord => 8,
        }
    }

    pub(crate) fn width(self) -> MemoryWidth {
        match self {
            Self::Load { width, .. } | Self::Store(width) => width,
            Self::Load64NonZero => MemoryWidth::DoubleWord,
            Self::LoadPair(PairWidth::Word)
            | Self::StorePair(PairWidth::Word) => MemoryWidth::Word,
            Self::LoadPair(PairWidth::DoubleWord)
            | Self::StorePair(PairWidth::DoubleWord) => MemoryWidth::DoubleWord,
        }
    }

    pub(crate) fn address_count(self) -> usize {
        if matches!(self, Self::LoadPair(_) | Self::StorePair(_)) { 2 } else { 1 }
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
    Operand(OperandOperation),
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
    Label,
    Memory,
    Operand,
    SlowPath,
    U8,
    U16,
    U32,
    U64,
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
                    BytecodeOperation::Load(FieldWidth::U8 | FieldWidth::U32)
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
            Self::LowLevel(LowLevelOperation::Increment32Memory) => IntrinsicEffects {
                memory: ModRef::ReadWrite,
                ..IntrinsicEffects::PURE
            },
            Self::LowLevel(LowLevelOperation::LoadVm) => IntrinsicEffects {
                machine_state: ModRef::Read,
                ..IntrinsicEffects::PURE
            },
            Self::Memory(operation) => operation.effects(),
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
            | Self::Value(_) => IntrinsicEffects::PURE,
        }
    }

    pub(crate) fn is_terminal(self) -> bool {
        matches!(
            self,
            Self::Control(operation) if operation.is_terminal()
        ) || self == Self::Call(CallOperation::SlowPath)
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
            Intrinsic::LowLevel(LowLevelOperation::LoadVm)
                .effects()
                .machine_state,
            ModRef::Read
        );
        assert!(Intrinsic::Assertion(AssertionOperation::NonZero).effects().may_trap);
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
        assert!(
            Intrinsic::from_name("call_interp")
                .unwrap()
                .call_signature(1)
                .is_some()
        );
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
