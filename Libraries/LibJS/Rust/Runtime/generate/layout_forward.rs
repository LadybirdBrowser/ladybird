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
pub struct Script;
pub struct Module;
pub struct ObjectEnvironment;
pub struct Intrinsics;

#[repr(transparent)]
pub struct PrivateElements(usize);

#[repr(transparent)]
pub struct FlyStringSlot(usize);

#[repr(transparent)]
pub struct ForeignCellSlot(usize);

#[repr(transparent)]
pub struct Utf16StringSlot(usize);

#[repr(transparent)]
pub struct RawNativeFunctionPointer(usize);

pub struct ShapeStorage;
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
