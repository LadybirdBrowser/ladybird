/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types outside the layout module that it names. build.rs has stand-ins for each of them.

use core::cell::UnsafeCell;
use core::ffi::c_void;

pub use crate::gc::class::Class;
pub use crate::runtime::object::PrivateElements;
pub use crate::runtime::private_environment::PrivateEnvironmentStorage;
pub use crate::runtime::realm::RealmStorage;
pub use crate::runtime::shape::ShapeStorage;
pub use crate::runtime::symbol::Symbol;
pub use ak::Utf16StringDataHeader;

/// Cells the layout points to that the runtime does not define yet.
pub enum Script {}
pub enum Module {}
pub enum ObjectEnvironment {}
pub enum Intrinsics {}

/// An optional fly string that is written at most once.
#[repr(transparent)]
pub struct FlyStringSlot(pub UnsafeCell<Option<ak::Utf16FlyString>>);

/// The resolved contents of a string, written at most once.
#[repr(transparent)]
pub struct Utf16StringSlot(pub UnsafeCell<Option<ak::Utf16String>>);

/// What a raw native function returns: the ABI of the C++ runtime's ThrowCompletionOr<Value>, whose variant is 0 for a
/// value and 1 for a thrown exception.
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

#[derive(Default)]
pub struct ObjectPropertyIteratorCacheDataStorage {}

#[derive(Default)]
pub struct DeclarativeEnvironmentRareDataStorage {}

#[derive(Default)]
pub struct EnvironmentShapeStorage {}

#[derive(Default)]
pub struct EcmascriptFunctionObjectStorage {}

#[derive(Default)]
pub struct SharedFunctionInstanceDataStorage {}
