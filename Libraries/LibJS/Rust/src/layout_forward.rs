/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The types outside the layout module that it names. build.rs has stand-ins for each of them.

use core::cell::{Cell, UnsafeCell};
use core::ffi::c_void;

pub use crate::gc::class::Class;
pub use crate::gc::foreign::ForeignCellSlot;
pub use crate::runtime::object::PrivateElements;
pub use crate::runtime::private_environment::PrivateEnvironmentStorage;
pub use crate::runtime::realm::RealmStorage;
pub use crate::runtime::shape::ShapeStorage;
pub use crate::runtime::special_property_cache::SpecialPropertyCache;
pub use crate::runtime::symbol::Symbol;
pub use crate::runtime::watchpoint_set::WatchpointSetStorage;
pub use ak::Utf16StringDataHeader;

pub use crate::runtime::intrinsics::Intrinsics;
pub use crate::runtime::module::Module;

pub use crate::runtime::object_environment::ObjectEnvironment;
pub use crate::script::Script;

/// An optional fly string, which is only ever copied in and out.
#[repr(transparent)]
pub struct FlyStringSlot(Cell<Option<ak::Utf16FlyString>>);

impl FlyStringSlot {
    pub const fn new(string: Option<ak::Utf16FlyString>) -> Self {
        Self(Cell::new(string))
    }

    pub fn get(&self) -> Option<ak::Utf16FlyString> {
        let string = self.0.take();
        let copy = string.clone();
        self.0.set(string);
        copy
    }

    pub fn set(&self, string: Option<ak::Utf16FlyString>) {
        self.0.set(string);
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

/// A copy of LibGC's js_heap_region_base, which LibGC fills in through its registration (see
/// js_heap_region_base_copy()) before any cell exists, and never changes after that.
struct HeapRegionBaseCopy(core::cell::UnsafeCell<usize>);

// SAFETY: LibGC writes the copy once, before any cell exists, and every read of it is of a cell that some thread has
//         made since, which it had to synchronize with to get at.
unsafe impl Sync for HeapRegionBaseCopy {}

// OPTIMIZATION: A copy of the base in this library, which a decode reads with one PC-relative load instead of a load of
//               js_heap_region_base's address from the GOT first. It is a plain load, which the compiler can hoist out
//               of loops, where an atomic one would have to stay in them.
static HEAP_REGION_BASE: HeapRegionBaseCopy = HeapRegionBaseCopy(core::cell::UnsafeCell::new(0));

/// The base of the heap region, which compressed cell pointers and cell values are offsets from. Zero until LibGC
/// reserves the region, which happens before any cell exists.
#[inline(always)]
pub fn heap_region_base() -> usize {
    // SAFETY: Nothing writes the copy after the first cell exists, see HeapRegionBaseCopy.
    unsafe { *HEAP_REGION_BASE.0.get() }
}

/// The copy of the heap region base, which LibJS registers with LibGC at static initialization.
#[unsafe(no_mangle)]
pub extern "C" fn js_heap_region_base_copy() -> *mut usize {
    HEAP_REGION_BASE.0.get()
}

const _: () =
    assert!(crate::layout::member::COMPRESSED_POINTER_RANGE == crate::build_configuration::HEAP_REGION_OFFSET_MASK + 1);
