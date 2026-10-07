/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Spells the field types of the layout module's mirror of LibJS/HostObjectABI.h in C, so that Layout.h can check
//! that the header declares each field, hook parameter and hook result with the type the mirror gives it. Only the
//! types the mirror uses have a spelling, so a field of any other type fails to compile here until it gets one.

use core::ffi::{c_char, c_void};

use crate::layout::host_class::*;

pub trait CTypeSpelling {
    fn c_type_spelling() -> String;
}

/// The spelling of a type that the mirror points to. It is separate from CTypeSpelling because c_char is i8 or u8
/// depending on the target, and only behind a pointer does the mirror mean C's char rather than uint8_t.
pub trait CPointeeSpelling {
    fn c_pointee_spelling() -> String;
}

pub fn c_type_spelling_of_field<Struct, Field: CTypeSpelling>(_: fn(&Struct) -> &Field) -> String {
    Field::c_type_spelling()
}

macro_rules! spelled_as {
    ($spelling_trait:ident :: $spelling_function:ident, $($type:ty => $spelling:literal),* $(,)?) => {
        $(
            impl $spelling_trait for $type {
                fn $spelling_function() -> String {
                    $spelling.to_string()
                }
            }
        )*
    };
}

spelled_as!(CTypeSpelling::c_type_spelling,
    () => "void",
    bool => "bool",
    u8 => "uint8_t",
    u16 => "uint16_t",
    u32 => "uint32_t",
    u64 => "uint64_t",
    usize => "size_t",
);

spelled_as!(CPointeeSpelling::c_pointee_spelling,
    c_void => "void",
    c_char => "char",
    u16 => "uint16_t",
);

/// Spells the mirror's own C types by their names, the same in C as in Rust.
macro_rules! spelled_by_name {
    ($($type:ident),* $(,)?) => {
        $(
            impl CTypeSpelling for $type {
                fn c_type_spelling() -> String {
                    stringify!($type).to_string()
                }
            }

            impl CPointeeSpelling for $type {
                fn c_pointee_spelling() -> String {
                    stringify!($type).to_string()
                }
            }
        )*
    };
}

spelled_by_name!(
    JSPropertyKey,
    JSCompletion,
    JSObject,
    JSVM,
    JSModule,
    JSPromiseCapability,
    JSGetCacheMetadata,
    JSSetCacheMetadata,
    JSPropertyDescriptor,
    JSValueSink,
    JSStringSink,
    JSHostObjectHooks,
    JSHostFunctionHooks,
    JSHostArrayHooks,
    JSResolvedBinding,
    JSHostModuleHooks,
    JSHostClass,
);

impl<Pointee: CPointeeSpelling> CTypeSpelling for *mut Pointee {
    fn c_type_spelling() -> String {
        format!("{}*", Pointee::c_pointee_spelling())
    }
}

impl<Pointee: CPointeeSpelling> CTypeSpelling for *const Pointee {
    fn c_type_spelling() -> String {
        format!("{} const*", Pointee::c_pointee_spelling())
    }
}

/// A hook or sink callback, which C declares as a plain function pointer that may be null.
macro_rules! nullable_function_pointer_spelling {
    ($($parameter:ident),*) => {
        impl<Result: CTypeSpelling, $($parameter: CTypeSpelling),*> CTypeSpelling
            for Option<unsafe extern "C" fn($($parameter),*) -> Result>
        {
            fn c_type_spelling() -> String {
                let parameters: &[String] = &[$($parameter::c_type_spelling()),*];
                format!("{} (*)({})", Result::c_type_spelling(), parameters.join(", "))
            }
        }
    };
}

nullable_function_pointer_spelling!();
nullable_function_pointer_spelling!(A);
nullable_function_pointer_spelling!(A, B);
nullable_function_pointer_spelling!(A, B, C);
nullable_function_pointer_spelling!(A, B, C, D);
nullable_function_pointer_spelling!(A, B, C, D, E);
nullable_function_pointer_spelling!(A, B, C, D, E, F);
