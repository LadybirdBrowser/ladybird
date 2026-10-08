/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use super::cell::{CellHeader, Gc};
use super::shape::Shape;
use super::value::Value;
use crate::layout_forward::{PrivateElements, TypedArrayBaseStorage};

pub mod object_flag {
    pub const IS_EXTENSIBLE: u16 = 1 << 0;
    pub const IS_RAW_NATIVE_FUNCTION: u16 = 1 << 1;
    pub const HAS_MAGICAL_LENGTH_PROPERTY: u16 = 1 << 2;
    pub const IS_TYPED_ARRAY: u16 = 1 << 3;
    pub const MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS: u16 = 1 << 4;
    pub const HAS_INTRINSIC_ACCESSORS: u16 = 1 << 5;
    pub const IS_ECMASCRIPT_FUNCTION_OBJECT: u16 = 1 << 6;
    pub const IS_FUNCTION: u16 = 1 << 7;
    pub const REQUIRES_SLOW_ADD_OWN_PROPERTY: u16 = 1 << 8;
    pub const IS_PLATFORM_OBJECT: u16 = 1 << 9;
    pub const IS_DIRECT_GETTER_FUNCTION: u16 = 1 << 10;
    pub const IS_GLOBAL_OBJECT: u16 = 1 << 11;
    pub const HAS_UNIMPLEMENTED_PROPERTIES: u16 = 1 << 12;
    pub const IS_HTMLDDA: u16 = 1 << 13;
    /// The named or indexed storage is a malloc allocation (rather than inline or a ValueStorage cell), which the
    /// object frees. The flags let a dead object's destructor tell without reading the storage, since a ValueStorage
    /// cell may be swept before its object.
    pub const HAS_MALLOC_NAMED_STORAGE: u16 = 1 << 14;
    pub const HAS_MALLOC_INDEXED_STORAGE: u16 = 1 << 15;
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum IndexedStorageKind {
    None,
    Packed,
    Holey,
    Dictionary,
}

/// Packed and holey indexed storage keeps its capacity in a header right in front of the first element.
pub const INDEXED_ELEMENTS_HEADER_SIZE: usize = 8;

pub const INLINE_NAMED_STORAGE_CAPACITY: usize = 2;

#[repr(C)]
pub struct Object {
    pub header: CellHeader,
    pub flags: Cell<u16>,
    pub indexed_storage_kind: Cell<IndexedStorageKind>,
    /// How many named property values the inline storage holds. It ends the object, and only plain objects have cells
    /// with room for more than INLINE_NAMED_STORAGE_CAPACITY.
    pub inline_named_capacity: Cell<u8>,
    pub indexed_array_like_size: Cell<u32>,
    pub shape: Cell<Gc<Shape>>,
    /// Points at `inline_named_storage` until the object needs more room.
    pub named_properties: Cell<*mut Value>,
    pub indexed_elements: Cell<*mut Value>,
    pub private_elements: PrivateElements,
    pub inline_named_storage: [Cell<Value>; INLINE_NAMED_STORAGE_CAPACITY],
}

// NB: The inline capacity fits in the padding in front of the array-like size, and the inline storage ends the object.
const _: () = assert!(size_of::<Object>() == 72);
const _: () = assert!(
    core::mem::offset_of!(Object, inline_named_storage) + INLINE_NAMED_STORAGE_CAPACITY * size_of::<Value>()
        == size_of::<Object>()
);

/// Mirrors the Variant<Auto, Detached, u32> the interpreter reads: the length first, then the alternative's index.
#[repr(C)]
pub struct ByteLengthSlot {
    pub length: Cell<u32>,
    pub alternative_index: Cell<u8>,
}

pub const BYTE_LENGTH_U32_INDEX: u8 = 2;

/// Mirrors JS::TypedArrayBase::Kind, in the order of JS_ENUMERATE_TYPED_ARRAYS.
pub mod typed_array_kind {
    pub const UINT8: u8 = 0;
    pub const UINT8_CLAMPED: u8 = 1;
    pub const UINT16: u8 = 2;
    pub const UINT32: u8 = 3;
    pub const BIG_UINT64: u8 = 4;
    pub const INT8: u8 = 5;
    pub const INT16: u8 = 6;
    pub const INT32: u8 = 7;
    pub const BIG_INT64: u8 = 8;
    pub const FLOAT16: u8 = 9;
    pub const FLOAT32: u8 = 10;
    pub const FLOAT64: u8 = 11;
}

pub const TYPED_ARRAY_CACHED_DATA_OFFSET_INVALID: usize = usize::MAX;

#[repr(C)]
pub struct TypedArrayBase {
    pub base: Object,
    pub kind: Cell<u8>,
    pub element_size: Cell<u8>,
    pub array_length: ByteLengthSlot,
    pub byte_offset: Cell<u32>,
    pub cached_data_offset: Cell<usize>,
    pub storage: TypedArrayBaseStorage,
}
