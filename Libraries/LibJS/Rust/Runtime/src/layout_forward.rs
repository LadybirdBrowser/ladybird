/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types outside the layout module that it names. build.rs has stand-ins for each of them.

use core::cell::UnsafeCell;
use core::ffi::c_void;

pub use crate::gc::class::Class;
pub use crate::gc::foreign::ForeignCellSlot;
pub use crate::runtime::object::PrivateElements;
pub use crate::runtime::private_environment::PrivateEnvironmentStorage;
pub use crate::runtime::realm::RealmStorage;
pub use crate::runtime::shape::ShapeStorage;
pub use crate::runtime::symbol::Symbol;
pub use ak::Utf16StringDataHeader;

pub use crate::runtime::intrinsics::Intrinsics;
pub use crate::runtime::module::Module;

pub use crate::runtime::object_environment::ObjectEnvironment;
pub use crate::script::Script;

/// An optional fly string, which is only ever copied in and out.
#[repr(transparent)]
pub struct FlyStringSlot(pub UnsafeCell<Option<ak::Utf16FlyString>>);

impl FlyStringSlot {
    pub const fn new(string: Option<ak::Utf16FlyString>) -> Self {
        Self(UnsafeCell::new(string))
    }

    pub fn get(&self) -> Option<ak::Utf16FlyString> {
        // SAFETY: The slot is only ever copied in and out, so no reference into it outlives these calls.
        unsafe { (*self.0.get()).clone() }
    }

    pub fn set(&self, string: Option<ak::Utf16FlyString>) {
        // SAFETY: As above. The previous string is dropped after the slot no longer refers to it.
        drop(unsafe { core::ptr::replace(self.0.get(), string) });
    }
}

/// The resolved contents of a string, written at most once.
#[repr(transparent)]
pub struct Utf16StringSlot(pub UnsafeCell<Option<ak::Utf16String>>);

/// What a raw native function returns: the ABI of LibJS's ThrowCompletionOr<Value>, whose variant is 0 for a value and
/// 1 for a thrown exception.
#[repr(C)]
pub struct RawNativeFunctionResult {
    pub payload: u64,
    pub variant: u8,
}

/// The interpreter calls raw native functions the way C++ returns a 16-byte ThrowCompletionOr<Value>: in two registers
/// on most targets, but through a caller-provided buffer on Mach-O x86_64 and Windows.
#[cfg(not(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows")))]
pub type RawNativeFunctionPointer = Option<unsafe extern "C" fn(vm: *mut c_void) -> RawNativeFunctionResult>;
#[cfg(any(all(target_arch = "x86_64", target_vendor = "apple"), target_os = "windows"))]
pub type RawNativeFunctionPointer = Option<unsafe extern "C" fn(result: *mut RawNativeFunctionResult, vm: *mut c_void)>;

pub use crate::bytecode::executable::ObjectPropertyIteratorCacheDataStorage;

pub use crate::runtime::declarative_environment::DeclarativeEnvironmentRareDataStorage;
pub use crate::runtime::environment_shape::EnvironmentShapeStorage;

pub use crate::runtime::ecmascript_function_object::EcmascriptFunctionObjectStorage;
pub use crate::runtime::shared_function_instance_data::SharedFunctionInstanceDataStorage;

pub use crate::runtime::typed_array::TypedArrayBaseStorage;
