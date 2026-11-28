/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stand-ins for the crate's types that the layout module names. Each has the size and alignment of the real type
//! wherever that affects a field the interpreter reads; the crate statically asserts every offset computed with them.

#![allow(dead_code)]

pub struct Class;
pub struct Symbol;
pub struct SpecialPropertyCache;
pub struct Script;
pub struct Module;
pub struct ObjectEnvironment;
pub struct Intrinsics;

#[repr(transparent)]
pub struct FlyStringSlot(usize);

#[repr(transparent)]
pub struct ForeignCellSlot(usize);

#[repr(transparent)]
pub struct Utf16StringSlot(usize);

#[repr(transparent)]
pub struct RawNativeFunctionPointer(usize);

pub struct DescriptorArray;
pub struct ShapeStorage;
pub struct WatchpointSetStorage;
pub struct ObjectPropertyIteratorCacheDataStorage;
pub struct DeclarativeEnvironmentRareDataStorage;
pub struct EnvironmentShapeStorage;
pub struct PrivateEnvironmentStorage;
pub struct EcmascriptFunctionObjectStorage;
pub struct SharedFunctionInstanceDataStorage;
pub struct RealmStorage;
pub struct TypedArrayBaseStorage;

/// Mirrors AK::Detail::Utf16StringDataHeader.
#[repr(C, align(8))]
pub struct Utf16StringDataHeader {
    pub reference_count: u32,
    pub length_in_code_units: u32,
    pub length_in_code_points: u32,
    pub hash: u32,
    pub flags: u32,
}

/// The crate's tests never dereference cells, so they hash the compressed address, as cells that never moved do.
///
/// # Safety
///
/// None, unlike the real one.
pub unsafe fn cell_identity_hash(cell: *const core::ffi::c_void) -> u32 {
    (cell.addr() >> 3) as u32
}

/// The compressed cell pointers of LibGC, which the tests take from libgc_api. The build script only lays out the members
/// that hold them, and never encodes or decodes one.
#[cfg(test)]
pub use libgc_api::member as gc_member;
#[cfg(not(test))]
pub mod gc_member {
    pub const COMPRESSED_POINTER_SHIFT: u32 = 3;
    pub const COMPRESSED_POINTER_RANGE: u64 = 1 << (32 + COMPRESSED_POINTER_SHIFT);

    pub fn decompress(_base: usize, _compressed: u32) -> usize {
        unreachable!()
    }

    pub fn write_barrier(_slot: *const u32, _compressed: u32) {}
}

/// The build script never decodes compressed pointers. The crate's tests decode them against this base, which is
/// aligned like LibGC aligns the real one, and where nothing is mapped.
pub fn heap_region_base() -> usize {
    0x7a00_0000_0000
}
