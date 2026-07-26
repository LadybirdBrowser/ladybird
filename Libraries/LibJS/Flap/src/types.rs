/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Compiler-wide Flap type definitions and target-independent properties.

use crate::intrinsic::{MemoryOperation, MemoryWidth};

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub(crate) enum ParameterMode {
    In,
    Out,
    InOut,
}

macro_rules! define_named_types {
    (
        shared { $($shared_variant:ident => $shared_name:literal, $shared_width:expr, $shared_pointer:expr;)* }
        source { $($source_variant:ident => $source_name:literal, $source_width:expr, $source_pointer:expr;)* }
        layout { $($layout_variant:ident => $layout_name:literal, $layout_width:expr, $layout_pointer:expr;)* }
        internal { $($internal_variant:ident => $internal_name:literal, $internal_width:expr, $internal_pointer:expr;)* }
        compound { $($compound_variant:ident $(($compound_payload:ty))?;)* }
    ) => {
        #[derive(Debug, Clone, PartialEq, Eq, Hash)]
        pub(crate) enum Type {
            $($shared_variant,)*
            $($source_variant,)*
            $($layout_variant,)*
            $($internal_variant,)*
            $($compound_variant $(($compound_payload))?,)*
        }

        impl Type {
            pub(crate) fn from_source_name(name: &str) -> Option<Self> {
                Some(match name {
                    $($shared_name => Self::$shared_variant,)*
                    $($source_name => Self::$source_variant,)*
                    _ => return None,
                })
            }

            fn from_layout_name(name: &str) -> Option<Self> {
                Some(match name {
                    $($shared_name => Self::$shared_variant,)*
                    $($layout_name => Self::$layout_variant,)*
                    _ => return None,
                })
            }

            fn simple_name(&self) -> Option<&'static str> {
                Some(match self {
                    $(Self::$shared_variant => $shared_name,)*
                    $(Self::$source_variant => $source_name,)*
                    $(Self::$layout_variant => $layout_name,)*
                    $(Self::$internal_variant => $internal_name,)*
                    _ => return None,
                })
            }

            fn simple_width(&self) -> Option<u8> {
                match self {
                    $(Self::$shared_variant => $shared_width,)*
                    $(Self::$source_variant => $source_width,)*
                    $(Self::$layout_variant => $layout_width,)*
                    $(Self::$internal_variant => $internal_width,)*
                    _ => None,
                }
            }

            fn simple_is_pointer(&self) -> bool {
                match self {
                    $(Self::$shared_variant => $shared_pointer,)*
                    $(Self::$source_variant => $source_pointer,)*
                    $(Self::$layout_variant => $layout_pointer,)*
                    $(Self::$internal_variant => $internal_pointer,)*
                    _ => false,
                }
            }
        }
    };
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, PartialOrd, Ord)]
pub(crate) enum RegisterClass {
    GeneralPurpose,
    FloatingPoint,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub(crate) enum BlockTemperature {
    #[default]
    Default,
    Hot,
    Cold,
}

macro_rules! define_interpreter_registers {
    ($($variant:ident => $name:literal, $ty:expr, $source:literal;)+) => {
        /// A target-independent register holding interpreter state.
        #[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
        pub(crate) enum InterpreterRegister {
            $($variant),+
        }

        impl InterpreterRegister {
            pub(crate) const COUNT: usize = [$(Self::$variant),+].len();

            pub(crate) fn from_name(name: &str) -> Option<Self> {
                Some(match name {
                    $($name => Self::$variant,)+
                    _ => return None,
                })
            }

            pub(crate) fn as_str(self) -> &'static str {
                match self {
                    $(Self::$variant => $name,)+
                }
            }

            pub(crate) fn value_type(self) -> Type {
                match self {
                    $(Self::$variant => $ty,)+
                }
            }

            pub(crate) fn source_value_type(self) -> Option<Type> {
                match self {
                    $(Self::$variant => $source.then(|| self.value_type()),)+
                }
            }
        }
    };
}

define_interpreter_registers! {
    ProgramCounter => "pc", Type::U32, true;
    ProgramBase => "pb", Type::U64, true;
    Values => "values", Type::Sequence(Box::new(Type::Value)), true;
    ExecutionContext => "exec_ctx", Type::ExecutionContext, true;
    DispatchTable => "dispatch", Type::U64, true;
    Int32TagShifted => "int32_tag_shifted", Type::U64, false;
    StackPointer => "sp", Type::U64, true;
    FramePointer => "fp", Type::U64, true;
}

#[derive(Debug, Clone, PartialEq, Eq, Hash)]
pub(crate) enum RegisterReference {
    Interpreter(InterpreterRegister),
    Named(String),
}

impl RegisterReference {
    pub(crate) fn new(name: impl Into<String>) -> Self {
        let name = name.into();
        InterpreterRegister::from_name(&name)
            .map(Self::Interpreter)
            .unwrap_or(Self::Named(name))
    }

    pub(crate) fn as_str(&self) -> &str {
        match self {
            Self::Interpreter(register) => register.as_str(),
            Self::Named(name) => name,
        }
    }
}

impl From<String> for RegisterReference {
    fn from(name: String) -> Self {
        Self::new(name)
    }
}

impl From<&str> for RegisterReference {
    fn from(name: &str) -> Self {
        Self::new(name)
    }
}

define_named_types! {
    shared {
        I8 => "i8", Some(1), false;
        I16 => "i16", Some(2), false;
        I32 => "i32", Some(4), false;
        U8 => "u8", Some(1), false;
        U16 => "u16", Some(2), false;
        U32 => "u32", Some(4), false;
        I64 => "i64", Some(8), false;
        U64 => "u64", Some(8), false;
        Bool => "bool", Some(1), false;
        ValueTag => "ValueTag", Some(2), false;
        EnvironmentCoordinate => "EnvironmentCoordinate", Some(4), false;
        Object => "Object", Some(8), true;
        PrimitiveString => "PrimitiveString", Some(8), true;
        Utf16StringData => "Utf16StringData", Some(8), true;
        Shape => "Shape", Some(8), true;
        ExecutionContext => "ExecutionContext", Some(8), true;
        VM => "VM", Some(8), true;
        Executable => "Executable", Some(8), true;
        Realm => "Realm", Some(8), true;
        GlobalEnvironment => "GlobalEnvironment", Some(8), true;
        Environment => "Environment", Some(8), true;
        PrivateEnvironment => "PrivateEnvironment", Some(8), true;
        DeclarativeEnvironment => "DeclarativeEnvironment", Some(8), true;
        EnvironmentShape => "EnvironmentShape", Some(8), true;
        DeclarativeEnvironmentRareData => "DeclarativeEnvironmentRareData", Some(8), true;
        GlobalVariableCache => "GlobalVariableCache", Some(8), true;
        PropertyLookupCache => "PropertyLookupCache", Some(8), true;
        PropertyNameIterator => "PropertyNameIterator", Some(8), true;
        ObjectPropertyIteratorCacheData => "ObjectPropertyIteratorCacheData", Some(8), true;
        ObjectPropertyIteratorCache => "ObjectPropertyIteratorCache", Some(8), true;
        PrototypeChainValidity => "PrototypeChainValidity", Some(8), true;
        FunctionObject => "FunctionObject", Some(8), true;
        ECMAScriptFunctionObject => "ECMAScriptFunctionObject", Some(8), true;
        RawNativeFunction => "RawNativeFunction", Some(8), true;
        SharedFunctionInstanceData => "SharedFunctionInstanceData", Some(8), true;
        ScriptOrModule => "ScriptOrModule", Some(8), true;
        PropertyStorage => "PropertyStorage", Some(8), true;
    }
    source {
        F64 => "f64", Some(8), false;
        F32 => "f32", Some(4), false;
        SlotIndex => "SlotIndex", Some(4), false;
        BytecodeOffset => "BytecodeOffset", Some(4), false;
        EnvironmentCoordinateCacheIndex => "EnvironmentCoordinateCacheIndex", Some(4), false;
        GlobalVariableCacheIndex => "GlobalVariableCacheIndex", Some(4), false;
        PropertyLookupCacheIndex => "PropertyLookupCacheIndex", Some(4), false;
        Operand => "Operand", Some(8), false;
        Memory => "Memory", None, false;
        IntegerCondition => "IntegerCondition", None, false;
        FloatCondition => "FloatCondition", None, false;
        EqualityOperation => "EqualityOperation", None, false;
        SlowPath => "SlowPath", Some(8), false;
    }
    layout {
        Value => "Value", Some(8), false;
        EnvironmentCoordinateEntry => "EnvironmentCoordinateEntry", Some(4), false;
    }
    internal {
        AnyGpr => "general-purpose value", None, false;
        FunctionSymbol => "FunctionSymbol", Some(8), false;
    }
    compound {
        Boxed(Box<Type>);
        Sequence(Box<Type>);
        Pointer(Box<Type>);
        Field(Box<Type>);
        Continuation(Vec<Type>);
        BinaryOperation(Box<Type>);
        CheckedBinaryOperation(Box<Type>);
        Tuple(Vec<Type>);
    }
}

impl Type {
    pub(crate) fn label() -> Self {
        Self::Continuation(Vec::new())
    }

    pub(crate) fn width(&self) -> Option<u8> {
        match self {
            Type::Boxed(_) | Type::Sequence(_) | Type::Pointer(_) | Type::Continuation(_) => Some(8),
            Type::Field(_) | Type::BinaryOperation(_) | Type::CheckedBinaryOperation(_) | Type::Tuple(_) => None,
            _ => self.simple_width(),
        }
    }

    pub(crate) fn register_class(&self) -> Option<RegisterClass> {
        match self {
            Type::F32 | Type::F64 => Some(RegisterClass::FloatingPoint),
            _ if self.width().is_some() => Some(RegisterClass::GeneralPurpose),
            _ => None,
        }
    }

    pub(crate) fn is_operation(&self) -> bool {
        matches!(
            self,
            Type::BinaryOperation(_)
                | Type::CheckedBinaryOperation(_)
                | Type::IntegerCondition
                | Type::FloatCondition
                | Type::EqualityOperation
        )
    }

    pub(crate) fn is_pointer(&self) -> bool {
        matches!(self, Type::Pointer(_) | Type::Sequence(_)) || self.simple_is_pointer()
    }

    pub(crate) fn memory_load(&self) -> Option<MemoryOperation> {
        Some(match self {
            Type::U8 | Type::Bool => MemoryOperation::Load {
                width: MemoryWidth::Byte,
                signed: false,
            },
            Type::I8 => MemoryOperation::Load {
                width: MemoryWidth::Byte,
                signed: true,
            },
            Type::U16 => MemoryOperation::Load {
                width: MemoryWidth::HalfWord,
                signed: false,
            },
            Type::I16 => MemoryOperation::Load {
                width: MemoryWidth::HalfWord,
                signed: true,
            },
            Type::U32 | Type::I32 | Type::ValueTag => MemoryOperation::Load {
                width: MemoryWidth::Word,
                signed: false,
            },
            Type::F32 => MemoryOperation::Load {
                width: MemoryWidth::Float,
                signed: false,
            },
            Type::F64 => return None,
            _ if self.width() == Some(8) => MemoryOperation::Load {
                width: MemoryWidth::DoubleWord,
                signed: false,
            },
            _ => return None,
        })
    }

    pub(crate) fn from_layout_type_name(name: &str) -> Option<Self> {
        if let Some(inner) = name.strip_prefix("Value<").and_then(|name| name.strip_suffix('>')) {
            return Self::from_layout_type_name(inner).map(|inner| Self::Boxed(Box::new(inner)));
        }
        if let Some(element) = name.strip_prefix("Sequence<").and_then(|name| name.strip_suffix('>')) {
            return Self::from_layout_type_name(element).map(|element| Self::Sequence(Box::new(element)));
        }
        Some(match name {
            "BindingFlags" => Self::Sequence(Box::new(Self::U8)),
            "IndexedElements" | "BindingValues" => Self::Sequence(Box::new(Self::Value)),
            "GlobalVariableCaches" => Self::Sequence(Box::new(Self::GlobalVariableCache)),
            "PropertyLookupCaches" => Self::Sequence(Box::new(Self::PropertyLookupCache)),
            _ => Self::from_layout_name(name)?,
        })
    }
}

impl std::fmt::Display for Type {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if let Some(name) = self.simple_name() {
            return formatter.write_str(name);
        }
        match self {
            Type::Boxed(inner) => write!(formatter, "Value<{inner}>"),
            Type::Sequence(inner) => write!(formatter, "Sequence<{inner}>"),
            Type::Pointer(inner) => write!(formatter, "ptr<{inner}>"),
            Type::Field(inner) => write!(formatter, "Field<{inner}>"),
            Type::Operand => write!(formatter, "Operand"),
            Type::Continuation(parameters) if parameters.is_empty() => write!(formatter, "Label"),
            Type::Continuation(parameters) => {
                write!(formatter, "Continuation<(")?;
                for (index, parameter) in parameters.iter().enumerate() {
                    if index != 0 {
                        write!(formatter, ", ")?;
                    }
                    write!(formatter, "{parameter}")?;
                }
                write!(formatter, ")>")
            }
            Type::BinaryOperation(inner) => write!(formatter, "BinaryOperation<{inner}>"),
            Type::CheckedBinaryOperation(inner) => write!(formatter, "CheckedBinaryOperation<{inner}>"),
            Type::Tuple(elements) => {
                write!(formatter, "(")?;
                for (index, element) in elements.iter().enumerate() {
                    if index != 0 {
                        write!(formatter, ", ")?;
                    }
                    write!(formatter, "{element}")?;
                }
                write!(formatter, ")")
            }
            _ => unreachable!("simple types have registry names"),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn centralizes_storage_and_register_properties() {
        assert_eq!(Type::U16.width(), Some(2));
        assert_eq!(Type::F64.register_class(), Some(RegisterClass::FloatingPoint));
        assert_eq!(
            Type::I32.memory_load().unwrap(),
            MemoryOperation::Load {
                width: MemoryWidth::Word,
                signed: false
            }
        );
        assert_eq!(Type::F64.memory_load(), None);
        assert!(Type::Object.is_pointer());
    }

    #[test]
    fn centralizes_interpreter_register_types() {
        assert_eq!(
            InterpreterRegister::from_name("values").unwrap().value_type(),
            Type::Sequence(Box::new(Type::Value))
        );
        assert_eq!(InterpreterRegister::from_name("pc").unwrap().value_type(), Type::U32);
        assert_eq!(InterpreterRegister::ProgramBase.as_str(), "pb");
        assert_eq!(InterpreterRegister::Int32TagShifted.source_value_type(), None);
        assert_eq!(InterpreterRegister::from_name("temporary"), None);
        assert_eq!(
            RegisterReference::new("pb"),
            RegisterReference::Interpreter(InterpreterRegister::ProgramBase)
        );
        assert_eq!(
            RegisterReference::new("temporary"),
            RegisterReference::Named("temporary".into())
        );
    }
}
