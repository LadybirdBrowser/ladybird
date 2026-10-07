/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Mirrors Libraries/LibJS/HostObjectABI.h: the host class tables through which an embedder defines objects whose
//! internal methods it implements, and the C types their hooks pass. The header is the definition. When C++ includes
//! the generated LibJS/Embedding/Layout.h, it checks the size, alignment, field count and the offset and type of every
//! field of each struct here, hook signatures included, and the value of every constant here against the header. A
//! constant that only the header defines goes unnoticed.

use core::ffi::{c_char, c_void};

pub const JS_HOST_ABI_VERSION: u16 = 1;

pub type JSValue = u64;

/// A borrowed property key, with the bits of the runtime's own property key.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct JSPropertyKey {
    pub bits: usize,
}

pub const JS_COMPLETION_NORMAL: u8 = 0;
pub const JS_COMPLETION_THROW: u8 = 1;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct JSCompletion {
    pub payload: u64,
    pub variant: u8,
}

/// Declares a type that C only ever sees behind a pointer.
macro_rules! opaque_c_types {
    ($($name:ident),* $(,)?) => {
        $(
            #[allow(clippy::upper_case_acronyms, reason = "the type keeps its C name, which ABI.h refers to")]
            #[repr(C)]
            pub struct $name {
                _opaque: [u8; 0],
            }
        )*
    };
}

opaque_c_types!(
    JSObject,
    JSVM,
    JSModule,
    JSPromiseCapability,
    JSGetCacheMetadata,
    JSSetCacheMetadata,
);

pub const JS_PROPERTY_LOOKUP_PHASE_OWN_PROPERTY: u8 = 0;
pub const JS_PROPERTY_LOOKUP_PHASE_PROTOTYPE_CHAIN: u8 = 1;

pub const JS_PD_HAS_VALUE: u16 = 1 << 0;
pub const JS_PD_HAS_GET: u16 = 1 << 1;
pub const JS_PD_HAS_SET: u16 = 1 << 2;
pub const JS_PD_HAS_WRITABLE: u16 = 1 << 3;
pub const JS_PD_WRITABLE: u16 = 1 << 4;
pub const JS_PD_HAS_ENUMERABLE: u16 = 1 << 5;
pub const JS_PD_ENUMERABLE: u16 = 1 << 6;
pub const JS_PD_HAS_CONFIGURABLE: u16 = 1 << 7;
pub const JS_PD_CONFIGURABLE: u16 = 1 << 8;
pub const JS_PD_HAS_PROPERTY_OFFSET: u16 = 1 << 9;
pub const JS_PD_PRESENT: u16 = 1 << 10;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct JSPropertyDescriptor {
    pub value: JSValue,
    pub get: *mut JSObject,
    pub set: *mut JSObject,
    pub property_offset: u32,
    pub flags: u16,
}

#[repr(C)]
pub struct JSValueSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, value: JSValue)>,
}

#[repr(C)]
pub struct JSStringSink {
    pub context: *mut c_void,
    pub append: Option<unsafe extern "C" fn(context: *mut c_void, code_units: *const u16, length_in_code_units: usize)>,
}

pub const JS_HOST_CLASS_OBJECT: u8 = 1;
pub const JS_HOST_CLASS_FUNCTION: u8 = 2;
pub const JS_HOST_CLASS_ARRAY: u8 = 3;
pub const JS_HOST_CLASS_MODULE: u8 = 4;

pub const JS_HOST_CLASS_IS_PLATFORM_OBJECT: u32 = 1 << 0;
pub const JS_HOST_CLASS_REQUIRES_SLOW_ADD_OWN_PROPERTY: u32 = 1 << 1;
pub const JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS: u32 = 1 << 2;
pub const JS_HOST_CLASS_IS_HTMLDDA: u32 = 1 << 3;
pub const JS_HOST_CLASS_IS_GLOBAL_OBJECT: u32 = 1 << 4;
pub const JS_HOST_CLASS_IMMUTABLE_PROTOTYPE: u32 = 1 << 5;
pub const JS_HOST_CLASS_NOT_CACHEABLE_FOR_PROPERTY_ABSENCE: u32 = 1 << 6;
pub const JS_HOST_CLASS_NOT_ELIGIBLE_FOR_OWN_PROPERTY_ENUMERATION_FAST_PATH: u32 = 1 << 7;
pub const JS_HOST_CLASS_HAS_CONSTRUCTOR: u32 = 1 << 8;
pub const JS_HOST_CLASS_SHARES_ALLOCATOR_WITH_PARENT: u32 = 1 << 9;

#[repr(C)]
pub struct JSHostObjectHooks {
    pub get_prototype_of: Option<unsafe extern "C" fn(object: *mut JSObject) -> JSCompletion>,
    pub set_prototype_of: Option<unsafe extern "C" fn(object: *mut JSObject, prototype: *mut JSObject) -> JSCompletion>,
    pub is_extensible: Option<unsafe extern "C" fn(object: *mut JSObject) -> JSCompletion>,
    pub prevent_extensions: Option<unsafe extern "C" fn(object: *mut JSObject) -> JSCompletion>,
    pub get_own_property: Option<
        unsafe extern "C" fn(object: *mut JSObject, key: JSPropertyKey, out: *mut JSPropertyDescriptor) -> JSCompletion,
    >,
    pub define_own_property: Option<
        unsafe extern "C" fn(
            object: *mut JSObject,
            key: JSPropertyKey,
            descriptor: *mut JSPropertyDescriptor,
            precomputed_get_own_property: *const JSPropertyDescriptor,
        ) -> JSCompletion,
    >,
    pub has_property: Option<unsafe extern "C" fn(object: *mut JSObject, key: JSPropertyKey) -> JSCompletion>,
    pub get: Option<
        unsafe extern "C" fn(
            object: *mut JSObject,
            key: JSPropertyKey,
            receiver: JSValue,
            cache_metadata: *mut JSGetCacheMetadata,
            phase: u8,
        ) -> JSCompletion,
    >,
    pub set: Option<
        unsafe extern "C" fn(
            object: *mut JSObject,
            key: JSPropertyKey,
            value: JSValue,
            receiver: JSValue,
            cache_metadata: *mut JSSetCacheMetadata,
            phase: u8,
        ) -> JSCompletion,
    >,
    pub delete_property: Option<unsafe extern "C" fn(object: *mut JSObject, key: JSPropertyKey) -> JSCompletion>,
    pub own_property_keys: Option<unsafe extern "C" fn(object: *mut JSObject, keys: *mut JSValueSink) -> JSCompletion>,
    pub is_cacheable_for_inherited_property: Option<unsafe extern "C" fn(object: *mut JSObject) -> bool>,
    pub error_data: Option<unsafe extern "C" fn(object: *mut JSObject) -> *mut c_void>,
    pub finalize: Option<unsafe extern "C" fn(object: *mut JSObject)>,
}

#[repr(C)]
pub struct JSHostFunctionHooks {
    pub call: Option<unsafe extern "C" fn(function: *mut JSObject, vm: *mut JSVM) -> JSCompletion>,
    pub construct:
        Option<unsafe extern "C" fn(function: *mut JSObject, vm: *mut JSVM, new_target: *mut JSObject) -> JSCompletion>,
    pub finalize: Option<unsafe extern "C" fn(function: *mut JSObject)>,
}

#[repr(C)]
pub struct JSHostArrayHooks {
    pub set: Option<
        unsafe extern "C" fn(
            object: *mut JSObject,
            key: JSPropertyKey,
            value: JSValue,
            receiver: JSValue,
            cache_metadata: *mut JSSetCacheMetadata,
            phase: u8,
        ) -> JSCompletion,
    >,
    pub delete_property: Option<unsafe extern "C" fn(object: *mut JSObject, key: JSPropertyKey) -> JSCompletion>,
}

pub const JS_RESOLVED_BINDING_BINDING_NAME: u8 = 0;
pub const JS_RESOLVED_BINDING_NAMESPACE: u8 = 1;
pub const JS_RESOLVED_BINDING_AMBIGUOUS: u8 = 2;
pub const JS_RESOLVED_BINDING_NULL: u8 = 3;

#[repr(C)]
pub struct JSResolvedBinding {
    pub module: *mut JSModule,
    pub binding_name: JSStringSink,
    pub r#type: u8,
}

#[repr(C)]
pub struct JSHostModuleHooks {
    pub get_exported_names: Option<unsafe extern "C" fn(module: *mut JSModule, names: *mut JSStringSink)>,
    pub resolve_export: Option<
        unsafe extern "C" fn(
            module: *mut JSModule,
            export_name: *const u16,
            export_name_length: usize,
            out: *mut JSResolvedBinding,
        ),
    >,
    pub initialize_environment: Option<unsafe extern "C" fn(module: *mut JSModule) -> JSCompletion>,
    pub execute_module: Option<
        unsafe extern "C" fn(module: *mut JSModule, capability_or_null: *mut JSPromiseCapability) -> JSCompletion,
    >,
}

#[repr(C)]
pub struct JSHostClass {
    pub abi_version: u16,
    pub kind: u8,
    pub reserved: u8,
    pub flags: u32,
    pub name: *const c_char,
    pub name_length: usize,
    pub parent: *const JSHostClass,
    /// The hook struct of the class's kind.
    pub hooks: *const c_void,
    /// Embedder data that the runtime never reads.
    pub user_data: *const c_void,
}
