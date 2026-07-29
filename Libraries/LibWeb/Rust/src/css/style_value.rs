/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Style value identities are shared across the FFI boundary, but remain confined to the thread
// owning the C++ style objects they may currently retain.
#![allow(clippy::arc_with_non_send_sync)]

//! Rust-owned CSS style value data.
//!
//! The C++ StyleValue subclasses keep their data in a Rust-owned, reference-counted
//! [`StyleValueData`] allocation instead of C++ member variables. The layout of
//! [`StyleValueData`] is exposed to C++
//! through cbindgen so that hot accessors compile to inline field reads with no FFI call.

use std::ffi::c_void;
use std::sync::Arc;

use crate::abort_on_panic;

pub(crate) use crate::css::retained_fly_string::{RetainedUtf16FlyString, RetainedUtf16FlyStringList};

unsafe extern "C" {
    fn ladybird_string_unref(raw: usize);
}

/// A strong reference to immutable Rust-owned style value data.
#[repr(C)]
pub struct RetainedStyleValueData {
    pointer: *const c_void,
}

impl PartialEq for RetainedStyleValueData {
    fn eq(&self, other: &Self) -> bool {
        match (self.optional_data(), other.optional_data()) {
            (Some(first), Some(second)) => std::ptr::eq(first, second) || first == second,
            (None, None) => true,
            _ => false,
        }
    }
}

impl RetainedStyleValueData {
    pub(crate) fn pointer(&self) -> *const StyleValueData {
        self.pointer.cast()
    }

    pub(crate) fn data(&self) -> &StyleValueData {
        unsafe { &*self.pointer.cast::<StyleValueData>() }
    }

    pub(crate) fn optional_data(&self) -> Option<&StyleValueData> {
        unsafe { self.pointer.cast::<StyleValueData>().as_ref() }
    }

    /// Assumes ownership of one strong reference to Rust-owned style value data.
    ///
    /// # Safety
    /// `pointer` must be a strong reference returned by `rust_style_value_retain`.
    pub(crate) unsafe fn from_retained_pointer(pointer: *const StyleValueData) -> Self {
        debug_assert!(!pointer.is_null());
        Self {
            pointer: pointer.cast(),
        }
    }

    /// Assumes ownership of one strong reference when `pointer` is non-null.
    ///
    /// # Safety
    /// `pointer` must be null or a strong reference returned by `rust_style_value_retain`.
    pub(crate) unsafe fn from_retained_optional_pointer(pointer: *const StyleValueData) -> Self {
        Self {
            pointer: pointer.cast(),
        }
    }

    pub(crate) fn clone_retained(&self) -> Self {
        let pointer = unsafe { rust_style_value_retain(self.pointer.cast()) };
        unsafe { Self::from_retained_optional_pointer(pointer) }
    }
}

impl Drop for RetainedStyleValueData {
    fn drop(&mut self) {
        unsafe { rust_style_value_release(self.pointer.cast()) };
    }
}

/// A retained, Rust-owned array of shared style value data references.
#[repr(C)]
pub struct RetainedStyleValueDataList {
    pointer: *mut RetainedStyleValueData,
    length: usize,
}

impl RetainedStyleValueDataList {
    pub(crate) fn as_slice(&self) -> &[RetainedStyleValueData] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }

    pub(crate) fn from_retained_values(values: Vec<RetainedStyleValueData>) -> Self {
        let slice = values.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedStyleValueData;
        Self { pointer, length }
    }

    /// Takes ownership of one strong reference to each value.
    ///
    /// # Safety
    /// `values` must point to `length` strong references returned by `rust_style_value_retain`.
    unsafe fn from_retained_pointers(values: *const *const StyleValueData, length: usize) -> Self {
        let slice: Box<[RetainedStyleValueData]> = (0..length)
            .map(|i| unsafe { RetainedStyleValueData::from_retained_pointer(*values.add(i)) })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedStyleValueData;
        Self { pointer, length }
    }

    /// Takes ownership of one strong reference to each non-null value.
    ///
    /// # Safety
    /// Every non-null entry in `values` must be a strong reference returned by
    /// `rust_style_value_retain`.
    unsafe fn from_retained_optional_pointers(values: *const *const StyleValueData, length: usize) -> Self {
        let slice: Box<[RetainedStyleValueData]> = (0..length)
            .map(|i| RetainedStyleValueData {
                pointer: unsafe { *values.add(i) }.cast(),
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedStyleValueData;
        Self { pointer, length }
    }
}

/// Implements `Drop` for a `Retained*List` struct: releases the Rust-owned boxed slice,
/// dropping each element (which releases the element's own retained references).
macro_rules! retained_list_drop {
    ($list:ident) => {
        impl Drop for $list {
            fn drop(&mut self) {
                if !self.pointer.is_null() {
                    drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.pointer, self.length)) });
                }
            }
        }
    };
}
pub(crate) use retained_list_drop;

retained_list_drop!(RetainedStyleValueDataList);

/// Implements the shared behavior for a `Retained*List` struct whose `from_raw` input is an
/// array of its own element type: `from_raw` copies `length` elements into a Rust-owned boxed
/// slice, assuming ownership of the elements' retained references, and `Drop` releases them.
macro_rules! retained_list {
    ($list:ident, $element:ty) => {
        impl $list {
            /// Takes ownership of the elements' retained references.
            ///
            /// # Safety
            /// `elements` must point to `length` valid elements whose retained references this
            /// list may assume ownership of.
            unsafe fn from_raw(elements: *const $element, length: usize) -> Self {
                let slice: Box<[$element]> = (0..length)
                    .map(|i| unsafe { std::ptr::read(elements.add(i)) })
                    .collect();
                let length = slice.len();
                let pointer = Box::into_raw(slice) as *mut $element;
                Self { pointer, length }
            }
        }
        retained_list_drop!($list);
    };
}

/// A Rust-owned array of C++ PropertyID values (`enum class PropertyID : u16`, opaque to Rust).
#[repr(C)]
pub struct RetainedPropertyIdList {
    pointer: *mut u16,
    length: usize,
}

retained_list!(RetainedPropertyIdList, u16);

impl RetainedPropertyIdList {
    pub(crate) fn as_slice(&self) -> &[u16] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// A retained AK::String, stored as its one-word raw representation. Owns one reference to the
/// underlying string data unless it is a short string; the C++ bridge handles both cases.
#[repr(C)]
pub struct RetainedString {
    raw: usize,
    bytes: *mut u8,
    length: usize,
}

impl RetainedString {
    /// Takes ownership of a leaked AK::String reference and copies its bytes.
    ///
    /// # Safety
    /// `bytes` must point at `length` readable bytes.
    unsafe fn from_raw(raw: usize, bytes: *const u8, length: usize) -> Self {
        let readable = unsafe { RetainedReadableString::from_raw(raw, bytes, length) };
        let result = Self {
            raw: readable.raw,
            bytes: readable.bytes,
            length: readable.length,
        };
        std::mem::forget(readable);
        result
    }

    fn as_bytes(&self) -> &[u8] {
        if self.bytes.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.bytes, self.length) }
    }
}

impl PartialEq for RetainedString {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}

impl Drop for RetainedString {
    fn drop(&mut self) {
        crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StringRetainReleaseCallback);
        unsafe { ladybird_string_unref(self.raw) };
        if !self.bytes.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.bytes, self.length)) });
        }
    }
}

/// A retained AK::String accompanied by a Rust-owned copy of its UTF-8 bytes. This is used when
/// Rust needs to inspect the string instead of only preserving it for C++ access.
#[repr(C)]
pub struct RetainedReadableString {
    raw: usize,
    bytes: *mut u8,
    length: usize,
}

impl RetainedReadableString {
    /// Takes ownership of a leaked AK::String reference and copies its bytes.
    ///
    /// # Safety
    /// `bytes` must point at `length` readable bytes.
    unsafe fn from_raw(raw: usize, bytes: *const u8, length: usize) -> Self {
        let source = unsafe { crate::bytes_from_raw(bytes, length) }.expect("invalid readable string bytes");
        if source.is_empty() {
            return Self {
                raw,
                bytes: std::ptr::null_mut(),
                length: 0,
            };
        }
        let owned = source.to_vec().into_boxed_slice();
        let length = owned.len();
        let bytes = Box::into_raw(owned).cast::<u8>();
        Self { raw, bytes, length }
    }

    pub(crate) fn as_bytes(&self) -> &[u8] {
        if self.bytes.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.bytes, self.length) }
    }
}

impl PartialEq for RetainedReadableString {
    fn eq(&self, other: &Self) -> bool {
        self.as_bytes() == other.as_bytes()
    }
}

impl Drop for RetainedReadableString {
    fn drop(&mut self) {
        crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StringRetainReleaseCallback);
        unsafe { ladybird_string_unref(self.raw) };
        if !self.bytes.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.bytes, self.length)) });
        }
    }
}

/// A retained CSS request URL modifier: the modifier type and either an enum value or a retained
/// string value (raw 0 when the value is an enum). All enums are C++ `enum class ... : u8`
/// values, opaque to Rust.
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedRequestUrlModifier {
    modifier_type: u8,
    enum_value: u8,
    string_value: RetainedUtf16FlyString,
}

/// A Rust-owned array of retained request URL modifiers.
#[repr(C)]
pub struct RetainedRequestUrlModifierList {
    pointer: *mut RetainedRequestUrlModifier,
    length: usize,
}

retained_list!(RetainedRequestUrlModifierList, RetainedRequestUrlModifier);

/// A Rust-owned array of bytes, used for lists of C++ u8 enum values.
#[repr(C)]
pub struct RetainedByteList {
    pointer: *mut u8,
    length: usize,
}

retained_list!(RetainedByteList, u8);

impl RetainedByteList {
    pub(crate) fn as_slice(&self) -> &[u8] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// A retained counter definition: the counter name, the reversed flag and an optional retained
/// value (null when absent).
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedCounterDefinition {
    name: RetainedUtf16FlyString,
    is_reversed: bool,
    value: RetainedStyleValueData,
}

/// A Rust-owned array of retained counter definitions.
#[repr(C)]
pub struct RetainedCounterDefinitionList {
    pointer: *mut RetainedCounterDefinition,
    length: usize,
}

retained_list!(RetainedCounterDefinitionList, RetainedCounterDefinition);

/// A retained image-set() option: the image, its resolution and an optional type string (a
/// retained AK::Utf16String raw, 0 when absent, released through the same bridge as fly
/// strings).
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedImageSetOption {
    image: RetainedStyleValueData,
    resolution: RetainedStyleValueData,
    has_type: bool,
    type_string: RetainedUtf16FlyString,
}

/// A Rust-owned array of retained image-set() options.
#[repr(C)]
pub struct RetainedImageSetOptionList {
    pointer: *mut RetainedImageSetOption,
    length: usize,
}

retained_list!(RetainedImageSetOptionList, RetainedImageSetOption);

/// A retained gradient color stop: an optional transition hint, then an optional color,
/// position and second position (each null when absent).
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedColorStop {
    transition_hint: RetainedStyleValueData,
    color: RetainedStyleValueData,
    position: RetainedStyleValueData,
    second_position: RetainedStyleValueData,
}

/// A Rust-owned array of retained gradient color stops.
#[repr(C)]
pub struct RetainedColorStopList {
    pointer: *mut RetainedColorStop,
    length: usize,
}

retained_list!(RetainedColorStopList, RetainedColorStop);

impl RetainedCounterDefinition {
    pub(crate) fn value(&self) -> &RetainedStyleValueData {
        &self.value
    }
}

impl RetainedImageSetOption {
    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 2] {
        [&self.image, &self.resolution]
    }
}

impl RetainedLinearEasingStop {
    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 2] {
        [&self.output, &self.input]
    }
}

macro_rules! retained_list_as_slice {
    ($list:ident, $element:ty) => {
        impl $list {
            pub(crate) fn as_slice(&self) -> &[$element] {
                if self.pointer.is_null() {
                    return &[];
                }
                unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
            }
        }
    };
}
retained_list_as_slice!(RetainedCounterDefinitionList, RetainedCounterDefinition);
retained_list_as_slice!(RetainedImageSetOptionList, RetainedImageSetOption);
retained_list_as_slice!(RetainedLinearEasingStopList, RetainedLinearEasingStop);

impl RetainedShapePoint {
    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 2] {
        [&self.x, &self.y]
    }

    pub(crate) fn from_retained_values(x: RetainedStyleValueData, y: RetainedStyleValueData) -> Self {
        Self { x, y }
    }
}
retained_list_as_slice!(RetainedShapePointList, RetainedShapePoint);

impl RetainedShapePointList {
    pub(crate) fn from_retained_points(points: Vec<RetainedShapePoint>) -> Self {
        let slice = points.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedShapePoint;
        Self { pointer, length }
    }
}

impl RetainedColorStop {
    /// The stop's retained values, absent ones as null retained references.
    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 4] {
        [
            &self.transition_hint,
            &self.color,
            &self.position,
            &self.second_position,
        ]
    }
}

impl RetainedColorStopList {
    pub(crate) fn as_slice(&self) -> &[RetainedColorStop] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// A retained named grid area: the retained area name and its grid line indices.
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedGridArea {
    name: RetainedUtf16FlyString,
    row_start: usize,
    row_end: usize,
    column_start: usize,
    column_end: usize,
}

/// A Rust-owned array of retained named grid areas.
#[repr(C)]
pub struct RetainedGridAreaList {
    pointer: *mut RetainedGridArea,
    length: usize,
}

retained_list!(RetainedGridAreaList, RetainedGridArea);

/// A retained linear() easing stop: the output value and an optional input (null when absent).
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedLinearEasingStop {
    output: RetainedStyleValueData,
    input: RetainedStyleValueData,
}

/// A Rust-owned array of retained linear() easing stops.
#[repr(C)]
pub struct RetainedLinearEasingStopList {
    pointer: *mut RetainedLinearEasingStop,
    length: usize,
}

retained_list!(RetainedLinearEasingStopList, RetainedLinearEasingStop);

/// The kind of one grid track list entry.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
// The C++ constructor supplies every variant through the FFI input.
#[allow(dead_code)]
pub enum GridTrackEntryKind {
    LineNames,
    Size,
    MinMax,
    Repeat,
}

/// Borrowed input description of one grid track list entry, used when creating a grid track
/// size list.
#[repr(C)]
pub struct GridTrackEntryInput {
    kind: GridTrackEntryKind,
    names: *const usize,
    name_count: usize,
    size_value: *const StyleValueData,
    min_value: *const StyleValueData,
    max_value: *const StyleValueData,
    repeat_type: u8,
    repeat_count: *const StyleValueData,
    repeat_is_subgrid: bool,
    repeat_preserve_line_name_sets: bool,
    repeat_entries: *const GridTrackEntryInput,
    repeat_entry_count: usize,
}

/// A Rust-owned array of retained grid track list entries.
#[repr(C)]
pub struct RetainedGridTrackEntryList {
    pointer: *mut RetainedGridTrackEntry,
    length: usize,
}

/// A retained, Rust-owned grid track list entry (see [`GridTrackEntryInput`] for the kinds).
#[repr(C)]
pub struct RetainedGridTrackEntry {
    pub(crate) kind: GridTrackEntryKind,
    pub(crate) names: RetainedUtf16FlyStringList,
    pub(crate) size_value: RetainedStyleValueData,
    pub(crate) min_value: RetainedStyleValueData,
    pub(crate) max_value: RetainedStyleValueData,
    pub(crate) repeat_type: u8,
    pub(crate) repeat_count: RetainedStyleValueData,
    pub(crate) repeat_is_subgrid: bool,
    pub(crate) repeat_preserve_line_name_sets: bool,
    pub(crate) repeat_entries_pointer: *mut RetainedGridTrackEntry,
    pub(crate) repeat_entries_length: usize,
}

impl Drop for RetainedGridTrackEntry {
    fn drop(&mut self) {
        if !self.repeat_entries_pointer.is_null() {
            drop(unsafe {
                Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                    self.repeat_entries_pointer,
                    self.repeat_entries_length,
                ))
            });
        }
    }
}

impl PartialEq for RetainedGridTrackEntry {
    fn eq(&self, other: &Self) -> bool {
        let repeat_entries = |entry: &Self| {
            if entry.repeat_entries_pointer.is_null() {
                &[]
            } else {
                unsafe { std::slice::from_raw_parts(entry.repeat_entries_pointer, entry.repeat_entries_length) }
            }
        };
        self.kind == other.kind
            && self.names == other.names
            && self.size_value == other.size_value
            && self.min_value == other.min_value
            && self.max_value == other.max_value
            && self.repeat_type == other.repeat_type
            && self.repeat_count == other.repeat_count
            && self.repeat_is_subgrid == other.repeat_is_subgrid
            && self.repeat_preserve_line_name_sets == other.repeat_preserve_line_name_sets
            && repeat_entries(self) == repeat_entries(other)
    }
}

impl RetainedGridTrackEntryList {
    pub(crate) fn as_slice(&self) -> &[RetainedGridTrackEntry] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }

    pub(crate) fn from_retained_entries(entries: Vec<RetainedGridTrackEntry>) -> Self {
        let slice = entries.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedGridTrackEntry;
        Self { pointer, length }
    }

    /// Takes ownership of the entries' retained values and names, recursively for nested
    /// repeat lists.
    ///
    /// # Safety
    /// `entries` must point to `length` valid entry descriptions.
    unsafe fn from_raw(entries: *const GridTrackEntryInput, length: usize) -> Self {
        let slice: Box<[RetainedGridTrackEntry]> = (0..length)
            .map(|i| {
                let input = unsafe { &*entries.add(i) };
                RetainedGridTrackEntry {
                    kind: input.kind,
                    names: unsafe { RetainedUtf16FlyStringList::from_raw(input.names, input.name_count) },
                    size_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.size_value) },
                    min_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.min_value) },
                    max_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.max_value) },
                    repeat_type: input.repeat_type,
                    repeat_count: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.repeat_count) },
                    repeat_is_subgrid: input.repeat_is_subgrid,
                    repeat_preserve_line_name_sets: input.repeat_preserve_line_name_sets,
                    repeat_entries_pointer: {
                        let nested = unsafe {
                            RetainedGridTrackEntryList::from_raw(input.repeat_entries, input.repeat_entry_count)
                        };
                        let pointer = nested.pointer;
                        std::mem::forget(nested);
                        pointer
                    },
                    repeat_entries_length: input.repeat_entry_count,
                }
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedGridTrackEntry;
        Self { pointer, length }
    }
}

impl RetainedGridTrackEntry {
    pub(crate) fn repeat_entries(&self) -> &[RetainedGridTrackEntry] {
        if self.repeat_entries_pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.repeat_entries_pointer, self.repeat_entries_length) }
    }
}

retained_list_drop!(RetainedGridTrackEntryList);

/// A retained polygon point: the x and y style values.
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedShapePoint {
    x: RetainedStyleValueData,
    y: RetainedStyleValueData,
}

/// A Rust-owned array of retained polygon points.
#[repr(C)]
pub struct RetainedShapePointList {
    pointer: *mut RetainedShapePoint,
    length: usize,
}

retained_list!(RetainedShapePointList, RetainedShapePoint);

/// An accepted numeric range for one value type (the C++ `enum class ValueType : u8`, opaque
/// to Rust).
#[repr(C)]
#[derive(PartialEq)]
pub struct RetainedNumericRangeByType {
    value_type: u8,
    min: f64,
    max: f64,
}

impl RetainedNumericRangeByType {
    pub(crate) fn value_type(&self) -> u8 {
        self.value_type
    }

    pub(crate) fn range(&self) -> (f64, f64) {
        (self.min, self.max)
    }
}

/// A Rust-owned array of accepted numeric ranges.
#[repr(C)]
pub struct RetainedNumericRangeList {
    pointer: *mut RetainedNumericRangeByType,
    length: usize,
}

retained_list!(RetainedNumericRangeList, RetainedNumericRangeByType);

impl RetainedNumericRangeList {
    pub(crate) fn empty() -> Self {
        Self {
            pointer: std::ptr::null_mut(),
            length: 0,
        }
    }

    pub(crate) fn as_slice(&self) -> &[RetainedNumericRangeByType] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }

    pub(crate) fn clone_owned(&self) -> Self {
        let ranges = self
            .as_slice()
            .iter()
            .map(|range| RetainedNumericRangeByType {
                value_type: range.value_type,
                min: range.min,
                max: range.max,
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();
        let length = ranges.len();
        let pointer = Box::into_raw(ranges) as *mut RetainedNumericRangeByType;
        Self { pointer, length }
    }
}

macro_rules! retained_list_partial_eq {
    ($list:ty, $element:ty) => {
        impl PartialEq for $list {
            fn eq(&self, other: &Self) -> bool {
                let as_slice = |list: &Self| -> &[$element] {
                    if list.pointer.is_null() {
                        &[]
                    } else {
                        unsafe { std::slice::from_raw_parts(list.pointer, list.length) }
                    }
                };
                as_slice(self) == as_slice(other)
            }
        }
    };
}
pub(crate) use retained_list_partial_eq;

retained_list_partial_eq!(RetainedStyleValueDataList, RetainedStyleValueData);
retained_list_partial_eq!(RetainedPropertyIdList, u16);
retained_list_partial_eq!(RetainedRequestUrlModifierList, RetainedRequestUrlModifier);
retained_list_partial_eq!(RetainedByteList, u8);
retained_list_partial_eq!(RetainedCounterDefinitionList, RetainedCounterDefinition);
retained_list_partial_eq!(RetainedImageSetOptionList, RetainedImageSetOption);
retained_list_partial_eq!(RetainedColorStopList, RetainedColorStop);
retained_list_partial_eq!(RetainedGridAreaList, RetainedGridArea);
retained_list_partial_eq!(RetainedLinearEasingStopList, RetainedLinearEasingStop);
retained_list_partial_eq!(RetainedGridTrackEntryList, RetainedGridTrackEntry);
retained_list_partial_eq!(RetainedShapePointList, RetainedShapePoint);
retained_list_partial_eq!(RetainedNumericRangeList, RetainedNumericRangeByType);

/// The shared leading fields of every color variant payload: the optional color type and the
/// color syntax. Placing this first in each color payload lets C++ read it without knowing
/// which color variant it has.
#[repr(C)]
#[derive(PartialEq)]
pub struct ColorBase {
    pub(crate) has_color_type: bool,
    pub(crate) color_type: u8,
    pub(crate) color_syntax: u8,
}

/// The data of a single immutable CSS style value.
///
/// Variant payload fields are read directly by the corresponding C++ StyleValue subclass, so
/// changing a payload changes the C++ accessors reading it.
#[repr(C, u8)]
// NB: Variant payload fields are only read by C++ through the exposed layout.
#[allow(dead_code)]
#[derive(PartialEq)]
pub enum StyleValueData {
    /// A CSS keyword. The value is the generated C++ `enum class Keyword : u16`, opaque to Rust.
    Keyword { keyword: u16 },
    /// A CSS `<number>`.
    Number { value: f64 },
    /// A CSS `<integer>`.
    Integer { value: i32 },
    /// A CSS `<angle>`. The unit is the C++ `enum class AngleUnit : u8`, opaque to Rust.
    Angle { value: f64, unit: u8 },
    /// A CSS `<flex>`. The unit is the C++ `enum class FlexUnit : u8`, opaque to Rust.
    Flex { value: f64, unit: u8 },
    /// A CSS `<frequency>`. The unit is the C++ `enum class FrequencyUnit : u8`, opaque to Rust.
    Frequency { value: f64, unit: u8 },
    /// A CSS `<length>`. The unit is the C++ `enum class LengthUnit : u8`, opaque to Rust.
    Length { value: f64, unit: u8 },
    /// A CSS `<percentage>`.
    Percentage { value: f64 },
    /// A CSS `<resolution>`. The unit is the C++ `enum class ResolutionUnit : u8`, opaque to Rust.
    Resolution { value: f64, unit: u8 },
    /// A CSS `<time>`. The unit is the C++ `enum class TimeUnit : u8`, opaque to Rust.
    Time { value: f64, unit: u8 },
    /// A basic shape. Kinds: inset (0), xywh (1) and rect (2) use five retained value slots;
    /// circle (3) and ellipse (4) use two; polygon (5) uses the fill rule and points; path (6)
    /// uses the fill rule and the retained serialized path data string.
    BasicShape {
        kind: u8,
        v0: RetainedStyleValueData,
        v1: RetainedStyleValueData,
        v2: RetainedStyleValueData,
        v3: RetainedStyleValueData,
        v4: RetainedStyleValueData,
        fill_rule: u8,
        points: RetainedShapePointList,
        path_string: RetainedUtf16FlyString,
    },
    /// A calc() or other math function: the retained calculation node tree root, its resolved
    /// numeric type, and the parse-time calculation context.
    Calculated {
        rust_calculation: crate::css::calc::CalcNodeHandle,
        /// The resolve-against target, base-mapped at creation: whether one
        /// exists, whether it is the number type, and otherwise its base type
        /// index in the numeric type order.
        resolve_as_is_number: bool,
        resolve_as_base: u8,
        resolved_type: crate::css::calc::FfiNumericType,
        has_percentages_resolve_as: bool,
        percentages_resolve_as: u8,
        resolve_numbers_as_integers: bool,
        accepted_ranges: RetainedNumericRangeList,
    },
    /// A CSS `<ratio>`, e.g. `16 / 9`. The numerator and denominator are style values.
    Ratio {
        numerator: RetainedStyleValueData,
        denominator: RetainedStyleValueData,
    },
    /// A unicode-range, e.g. `U+0025-00FF`.
    UnicodeRange { min_code_point: u32, max_code_point: u32 },
    /// A CSS `<opacity-value>`: a number, percentage or calculated style value.
    OpacityValue { value: RetainedStyleValueData },
    /// One edge of a CSS `<position>`: an optional edge keyword (the C++ `enum class
    /// PositionEdge : u8`, opaque to Rust) and an optional offset style value (null when absent).
    Edge {
        has_edge: bool,
        edge: u8,
        offset: RetainedStyleValueData,
    },
    /// The guaranteed-invalid value: https://drafts.csswg.org/css-variables/#guaranteed-invalid-value
    GuaranteedInvalid,
    /// An absent optional value in a shorthand.
    EmptyOptional,
    /// grid-auto-flow.
    GridAutoFlow { row: bool, dense: bool },
    /// text-underline-position. Both fields are C++ `enum class ... : u8` values, opaque to Rust.
    TextUnderlinePosition { horizontal: u8, vertical: u8 },
    /// contrast-color() with its retained color style value.
    ContrastColor {
        color_base: ColorBase,
        color: RetainedStyleValueData,
    },
    /// superellipse() with its retained parameter style value.
    Superellipse { parameter: RetainedStyleValueData },
    /// A pending-substitution value retaining the shorthand value it came from.
    PendingSubstitution {
        original_shorthand_value: RetainedStyleValueData,
    },
    /// scrollbar-color with retained thumb and track color values.
    ScrollbarColor {
        thumb_color: RetainedStyleValueData,
        track_color: RetainedStyleValueData,
    },
    /// rect() with four retained edge style values.
    Rect {
        top: RetainedStyleValueData,
        right: RetainedStyleValueData,
        bottom: RetainedStyleValueData,
        left: RetainedStyleValueData,
    },
    /// A CSS `<string>`.
    String { string: RetainedUtf16FlyString },
    /// An unrecognized CSS function, kept as its name and argument value.
    Function {
        name: RetainedUtf16FlyString,
        value: RetainedStyleValueData,
    },
    /// An OpenType tag with its value, from font-feature-settings or font-variation-settings.
    /// The mode is the C++ OpenTypeTaggedStyleValue::Mode, opaque to Rust.
    OpenTypeTagged {
        mode: u8,
        tag: RetainedUtf16FlyString,
        packed_tag: u32,
        value: RetainedStyleValueData,
    },
    /// font-style: a keyword (the C++ `enum class FontStyleKeyword : u8`, opaque to Rust) and
    /// an optional oblique angle style value (null when absent).
    FontStyle {
        font_style: u8,
        angle_value: RetainedStyleValueData,
    },
    /// text-indent: a length-percentage style value plus the hanging and each-line flags.
    TextIndent {
        length_percentage: RetainedStyleValueData,
        hanging: bool,
        each_line: bool,
    },
    /// overflow-clip-margin: an optional visual box (the C++ `enum class BackgroundBox : u8`,
    /// opaque to Rust) and an offset style value.
    OverflowClipMargin {
        has_visual_box: bool,
        visual_box: u8,
        offset: RetainedStyleValueData,
    },
    /// sibling-count() or sibling-index(). Both fields are C++ `enum class ... : u8` values,
    /// opaque to Rust.
    TreeCountingFunction { function: u8, computed_type: u8 },
    /// background-size with its two retained size style values.
    BackgroundSize {
        size_x: RetainedStyleValueData,
        size_y: RetainedStyleValueData,
    },
    /// A background repeat-style. Both fields are the C++ `enum class Repetition : u8`, opaque
    /// to Rust.
    RepeatStyle { repeat_x: u8, repeat_y: u8 },
    /// border-image-slice: four retained offset data allocations and the fill keyword.
    BorderImageSlice {
        top: RetainedStyleValueData,
        right: RetainedStyleValueData,
        bottom: RetainedStyleValueData,
        left: RetainedStyleValueData,
        fill: bool,
    },
    /// anchor-size(): an optional anchor name, an optional size keyword (the C++ `enum class
    /// AnchorSize : u8`, opaque to Rust) and an optional retained fallback value.
    AnchorSize {
        has_anchor_name: bool,
        anchor_name: RetainedUtf16FlyString,
        has_anchor_size: bool,
        anchor_size: u8,
        fallback_value: RetainedStyleValueData,
    },
    /// anchor(): an optional anchor name, the retained side style value and an optional
    /// retained fallback value.
    Anchor {
        has_anchor_name: bool,
        anchor_name: RetainedUtf16FlyString,
        anchor_side: RetainedStyleValueData,
        fallback_value: RetainedStyleValueData,
    },
    /// A CSS `<position>` with its two retained edge style values.
    Position {
        edge_x: RetainedStyleValueData,
        edge_y: RetainedStyleValueData,
    },
    /// A shadow. The type and placement are C++ enums, opaque to Rust; the color, blur radius
    /// and spread distance are optional retained style values (null when absent).
    Shadow {
        shadow_type: u8,
        color: RetainedStyleValueData,
        offset_x: RetainedStyleValueData,
        offset_y: RetainedStyleValueData,
        blur_radius: RetainedStyleValueData,
        spread_distance: RetainedStyleValueData,
        placement: u8,
    },
    /// content with its retained content list and optional alt-text list (null when absent).
    Content {
        content: RetainedStyleValueData,
        alt_text: RetainedStyleValueData,
    },
    /// A @counter-style system descriptor: a plain system keyword (kind 0, the C++ `enum class
    /// CounterStyleSystem : u8`, opaque to Rust), fixed with an optional retained first symbol
    /// (kind 1), or extends with a retained counter style name (kind 2).
    CounterStyleSystem {
        kind: u8,
        system: u8,
        first_symbol: RetainedStyleValueData,
        name: RetainedUtf16FlyString,
    },
    /// A counter style reference: either a retained counter style name, or a symbols() function
    /// with its type (the C++ `enum class SymbolsType : u8`, opaque to Rust) and retained
    /// symbol strings.
    CounterStyle {
        is_symbols: bool,
        name: RetainedUtf16FlyString,
        symbols_type: u8,
        symbols: RetainedUtf16FlyStringList,
    },
    /// A color function such as rgb() or oklch(): three retained channel values, an optional
    /// retained alpha, an optional name and an optional retained origin color for relative
    /// color syntax.
    ColorFunction {
        color_base: ColorBase,
        channel_0: RetainedStyleValueData,
        channel_1: RetainedStyleValueData,
        channel_2: RetainedStyleValueData,
        alpha: RetainedStyleValueData,
        has_name: bool,
        name: RetainedUtf16FlyString,
        origin_color: RetainedStyleValueData,
    },
    /// color-mix() with its optional retained interpolation method value and two components,
    /// each a retained color with an optional retained percentage.
    ColorMix {
        color_base: ColorBase,
        color_interpolation_method: RetainedStyleValueData,
        first_color: RetainedStyleValueData,
        first_percentage: RetainedStyleValueData,
        second_color: RetainedStyleValueData,
        second_percentage: RetainedStyleValueData,
    },
    /// The shared data of every color style value: an optional color type and the color syntax
    /// (both C++ enums on ColorStyleValue, opaque to Rust).
    /// linear-gradient(): a direction (either a retained angle value or a side-or-corner
    /// keyword), the retained color stops, the gradient type, the repeating flag, an optional
    /// retained interpolation method and the color syntax. Enums are C++ types, opaque to Rust.
    LinearGradient {
        has_direction_value: bool,
        direction_value: RetainedStyleValueData,
        side_or_corner: u8,
        color_stop_list: RetainedColorStopList,
        gradient_type: u8,
        repeating: bool,
        color_interpolation_method: RetainedStyleValueData,
        color_syntax: u8,
    },
    /// conic-gradient(): an optional retained from-angle, the retained position, the retained
    /// color stops, the repeating flag, an optional retained interpolation method and the color
    /// syntax (a C++ enum, opaque to Rust).
    ConicGradient {
        from_angle: RetainedStyleValueData,
        position: RetainedStyleValueData,
        color_stop_list: RetainedColorStopList,
        repeating: bool,
        color_interpolation_method: RetainedStyleValueData,
        color_syntax: u8,
    },
    /// radial-gradient(): the ending shape (a C++ enum, opaque to Rust), the retained size and
    /// position values, the retained color stops, the repeating flag, an optional retained
    /// interpolation method and the color syntax.
    RadialGradient {
        ending_shape: u8,
        size: RetainedStyleValueData,
        position: RetainedStyleValueData,
        color_stop_list: RetainedColorStopList,
        repeating: bool,
        color_interpolation_method: RetainedStyleValueData,
        color_syntax: u8,
    },
    /// A url() image. Only the CSS URL is immutable value data; the style sheet attachment and
    /// loading state stay on the C++ side.
    Image {
        url: RetainedString,
        url_type: u8,
        url_modifiers: RetainedRequestUrlModifierList,
    },
    /// image-set() with its retained options.
    ImageSet { options: RetainedImageSetOptionList },
    /// An easing function: linear() with its retained stops (kind 0), cubic-bezier() with four
    /// retained control values (kind 1), or steps() with a retained interval count and a step
    /// position (kind 2, the C++ `enum class StepPosition : u8`, opaque to Rust).
    Easing {
        kind: u8,
        linear_stops: RetainedLinearEasingStopList,
        x1: RetainedStyleValueData,
        y1: RetainedStyleValueData,
        x2: RetainedStyleValueData,
        y2: RetainedStyleValueData,
        number_of_intervals: RetainedStyleValueData,
        step_position: u8,
    },
    /// A cursor with its retained image value and optional retained hotspot coordinates (both
    /// null or both non-null).
    Cursor {
        image: RetainedStyleValueData,
        x: RetainedStyleValueData,
        y: RetainedStyleValueData,
    },
    /// A grid track size list: the subgrid and preserve-line-name-sets flags and the retained
    /// track entries.
    GridTrackSizeList {
        is_subgrid: bool,
        preserve_line_name_sets: bool,
        entries: RetainedGridTrackEntryList,
    },
    /// grid-template-areas with its retained named areas and the row and column counts.
    GridTemplateArea {
        grid_areas: RetainedGridAreaList,
        row_count: usize,
        column_count: usize,
    },
    /// counter-increment, counter-reset or counter-set with its retained counter definitions.
    CounterDefinitions {
        counter_definitions: RetainedCounterDefinitionList,
    },
    /// A grid-row/grid-column placement: auto (0), a span (1) or an area/line (2), with an
    /// optional retained line value and an optional retained name.
    GridTrackPlacement {
        kind: u8,
        value: RetainedStyleValueData,
        has_name: bool,
        name: RetainedUtf16FlyString,
    },
    /// counter() or counters(). The function is the C++ CounterFunction enum, opaque to Rust;
    /// the join string is empty for counter().
    Counter {
        function: u8,
        counter_name: RetainedUtf16FlyString,
        counter_style: RetainedStyleValueData,
        join_string: RetainedUtf16FlyString,
    },
    /// light-dark() with its two retained color style values.
    LightDark {
        color_base: ColorBase,
        light: RetainedStyleValueData,
        dark: RetainedStyleValueData,
    },
    /// random-value-sharing: an optional retained fixed value (null when absent), the auto flag,
    /// an optional name and the element-shared flag.
    RandomValueSharing {
        fixed_value: RetainedStyleValueData,
        is_auto: bool,
        has_name: bool,
        name: RetainedUtf16FlyString,
        element_shared: bool,
    },
    /// scrollbar-gutter. The value is the C++ `enum class ScrollbarGutter : u8`, opaque to Rust.
    ScrollbarGutter { value: u8 },
    /// A color interpolation method: either a rectangular color space, or a polar color space
    /// with a hue interpolation method. All fields are C++ `enum class ... : u8` values, opaque
    /// to Rust.
    ColorInterpolationMethod {
        is_polar: bool,
        color_space: u8,
        hue_interpolation_method: u8,
    },
    /// A list of style values. The separator and collapsible flag come from the C++
    /// StyleValueList enums, opaque to Rust.
    ValueList {
        values: RetainedStyleValueDataList,
        separator: u8,
        collapsible: bool,
    },
    /// A tuple of optional style values (null entries represent absent optionals).
    Tuple { values: RetainedStyleValueDataList },
    /// A display value: the raw bytes of the C++ Display value type (a tag plus a union of
    /// packed u8 enums), opaque to Rust.
    Display { raw: u32 },
    /// color-scheme with its retained scheme names and the only keyword flag.
    ColorScheme {
        schemes: RetainedUtf16FlyStringList,
        scheme_codes: RetainedByteList,
        only: bool,
    },
    /// An unresolved value containing arbitrary substitution functions, kept as its retained
    /// source text, an optional normalized comparison text (empty when absent), the presence
    /// flags of each substitution function, the attr-taint flag, and an optional parsed value
    /// cached for an attr()-tainted registered custom property.
    Unresolved {
        source_text: RetainedReadableString,
        value_comparison_text: RetainedReadableString,
        presence_attr: bool,
        presence_dashed_function: bool,
        presence_env: bool,
        presence_if: bool,
        presence_inherit: bool,
        presence_var: bool,
        contains_attr_tainted_values: bool,
        parsed_value: RetainedStyleValueData,
    },
    /// A CSS url() or src() with its retained URL string, type (the C++ URL::Type, opaque to
    /// Rust) and request URL modifiers.
    Url {
        url: RetainedString,
        url_type: u8,
        modifiers: RetainedRequestUrlModifierList,
    },
    /// A @font-face source: either local() with a retained family name value, or a URL (encoded
    /// as in [`StyleValueData::Url`]) with an optional format string and a list of font
    /// technologies (C++ `enum class FontTech : u8`, opaque to Rust).
    FontSource {
        is_local: bool,
        local_name: RetainedStyleValueData,
        url: RetainedString,
        url_type: u8,
        url_modifiers: RetainedRequestUrlModifierList,
        has_format: bool,
        format: RetainedUtf16FlyString,
        tech: RetainedByteList,
    },
    /// A radial gradient size: one or two components, each either a RadialExtent keyword (the
    /// C++ `enum class RadialExtent : u8`, opaque to Rust) or a retained style value.
    RadialSize {
        component_count: u8,
        is_extent_0: bool,
        extent_0: u8,
        value_0: RetainedStyleValueData,
        is_extent_1: bool,
        extent_1: u8,
        value_1: RetainedStyleValueData,
    },
    /// A transform function with its argument values. The property (PropertyID : u16) and
    /// function are C++ enums, opaque to Rust.
    Transformation {
        property: u16,
        transform_function: u8,
        values: RetainedStyleValueDataList,
    },
    /// A shorthand property value: the shorthand id, its longhand ids (both C++
    /// `enum class PropertyID : u16`, opaque to Rust) and their values.
    Shorthand {
        shorthand_property: u16,
        sub_properties: RetainedPropertyIdList,
        values: RetainedStyleValueDataList,
    },
    /// A CSS `<custom-ident>`.
    CustomIdent { custom_ident: RetainedUtf16FlyString },
    /// A border-radius rect of four retained corner radius data allocations.
    BorderRadiusRect {
        top_left: RetainedStyleValueData,
        top_right: RetainedStyleValueData,
        bottom_right: RetainedStyleValueData,
        bottom_left: RetainedStyleValueData,
    },
    /// A single corner radius: the horizontal and vertical radii and whether they differ.
    BorderRadius {
        is_elliptical: bool,
        horizontal_radius: RetainedStyleValueData,
        vertical_radius: RetainedStyleValueData,
    },
    /// A filter function. Kinds: blur (0, value = radius), drop-shadow (1, value = shadow),
    /// hue-rotate (2, value = angle), color (3, value = amount, with the color operation).
    /// The kind and the color operation are C++ enum values, opaque to Rust.
    Filter {
        kind: u8,
        color_operation: u8,
        value: RetainedStyleValueData,
    },
}

impl StyleValueData {
    pub(crate) fn unresolved_token_source(&self) -> Option<&[u8]> {
        let Self::Unresolved {
            source_text,
            value_comparison_text,
            ..
        } = self
        else {
            return None;
        };
        if value_comparison_text.as_bytes().is_empty() {
            Some(source_text.as_bytes())
        } else {
            Some(value_comparison_text.as_bytes())
        }
    }

    pub(crate) fn unresolved_var_source(&self) -> Option<(&[u8], bool)> {
        let Self::Unresolved {
            presence_attr,
            presence_dashed_function,
            presence_env,
            presence_if,
            presence_inherit,
            presence_var,
            contains_attr_tainted_values,
            ..
        } = self
        else {
            return None;
        };
        let cannot_resolve_natively = *presence_attr
            || *presence_dashed_function
            || *presence_env
            || *presence_if
            || *presence_inherit
            || *contains_attr_tainted_values;
        if cannot_resolve_natively {
            return None;
        }
        Some((self.unresolved_token_source().unwrap_or_default(), *presence_var))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_keyword(keyword: u16) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Keyword { keyword })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_number(value: f64) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Number { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_integer(value: i32) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Integer { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_angle(value: f64, unit: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Angle { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_flex(value: f64, unit: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Flex { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_frequency(value: f64, unit: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Frequency { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_length(value: f64, unit: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Length { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_percentage(value: f64) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Percentage { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_resolution(value: f64, unit: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Resolution { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_time(value: f64, unit: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Time { value, unit })))
}

/// Takes ownership of one strong reference to each of the numerator and denominator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_ratio(
    numerator: *const StyleValueData,
    denominator: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Ratio {
            numerator: unsafe { RetainedStyleValueData::from_retained_pointer(numerator) },
            denominator: unsafe { RetainedStyleValueData::from_retained_pointer(denominator) },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_unicode_range(
    min_code_point: u32,
    max_code_point: u32,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::UnicodeRange {
            min_code_point,
            max_code_point,
        }))
    })
}

/// Takes ownership of one strong reference to the value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_opacity_value(value: *const StyleValueData) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::OpacityValue {
            value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
        }))
    })
}

/// Takes ownership of one strong reference to the offset data if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_edge(
    has_edge: bool,
    edge: u8,
    offset: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Edge {
            has_edge,
            edge,
            offset: unsafe { RetainedStyleValueData::from_retained_optional_pointer(offset) },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_guaranteed_invalid() -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::GuaranteedInvalid)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_empty_optional() -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::EmptyOptional)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_grid_auto_flow(row: bool, dense: bool) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::GridAutoFlow { row, dense })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_text_underline_position(
    horizontal: u8,
    vertical: u8,
) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::TextUnderlinePosition { horizontal, vertical })))
}

/// Takes ownership of one strong reference to the color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_contrast_color(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    color: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ContrastColor {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            color: unsafe { RetainedStyleValueData::from_retained_pointer(color) },
        }))
    })
}

/// Takes ownership of one strong reference to the parameter data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_superellipse(
    parameter: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Superellipse {
            parameter: unsafe { RetainedStyleValueData::from_retained_pointer(parameter) },
        }))
    })
}

/// Takes ownership of one strong reference to the original shorthand value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_pending_substitution(
    original_shorthand_value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::PendingSubstitution {
            original_shorthand_value: unsafe {
                RetainedStyleValueData::from_retained_pointer(original_shorthand_value)
            },
        }))
    })
}

/// Takes ownership of one strong reference to each color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_scrollbar_color(
    thumb_color: *const StyleValueData,
    track_color: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ScrollbarColor {
            thumb_color: unsafe { RetainedStyleValueData::from_retained_pointer(thumb_color) },
            track_color: unsafe { RetainedStyleValueData::from_retained_pointer(track_color) },
        }))
    })
}

/// Takes ownership of one strong reference to each edge data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_rect(
    top: *const StyleValueData,
    right: *const StyleValueData,
    bottom: *const StyleValueData,
    left: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Rect {
            top: unsafe { RetainedStyleValueData::from_retained_pointer(top) },
            right: unsafe { RetainedStyleValueData::from_retained_pointer(right) },
            bottom: unsafe { RetainedStyleValueData::from_retained_pointer(bottom) },
            left: unsafe { RetainedStyleValueData::from_retained_pointer(left) },
        }))
    })
}

/// Takes ownership of one strong reference to the filter's value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_filter(
    kind: u8,
    color_operation: u8,
    value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Filter {
            kind,
            color_operation,
            value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
        }))
    })
}

/// Takes ownership of one strong reference to each radius data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius(
    is_elliptical: bool,
    horizontal_radius: *const StyleValueData,
    vertical_radius: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::BorderRadius {
            is_elliptical,
            horizontal_radius: unsafe { RetainedStyleValueData::from_retained_pointer(horizontal_radius) },
            vertical_radius: unsafe { RetainedStyleValueData::from_retained_pointer(vertical_radius) },
        }))
    })
}

/// Takes ownership of one strong reference to each corner radius data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius_rect(
    top_left: *const StyleValueData,
    top_right: *const StyleValueData,
    bottom_right: *const StyleValueData,
    bottom_left: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::BorderRadiusRect {
            top_left: unsafe { RetainedStyleValueData::from_retained_pointer(top_left) },
            top_right: unsafe { RetainedStyleValueData::from_retained_pointer(top_right) },
            bottom_right: unsafe { RetainedStyleValueData::from_retained_pointer(bottom_right) },
            bottom_left: unsafe { RetainedStyleValueData::from_retained_pointer(bottom_left) },
        }))
    })
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_string(string: usize) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::String {
            string: unsafe { RetainedUtf16FlyString::from_leaked_raw(string) },
        }))
    })
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_custom_ident(custom_ident: usize) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::CustomIdent {
            custom_ident: unsafe { RetainedUtf16FlyString::from_leaked_raw(custom_ident) },
        }))
    })
}

/// Takes ownership of one leaked reference to the name and one strong reference to the value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_function(
    name: usize,
    value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Function {
            name: unsafe { RetainedUtf16FlyString::from_leaked_raw(name) },
            value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
        }))
    })
}

/// Takes ownership of one leaked reference to the tag and one strong reference to the value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_open_type_tagged(
    mode: u8,
    tag: usize,
    packed_tag: u32,
    value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::OpenTypeTagged {
            mode,
            tag: unsafe { RetainedUtf16FlyString::from_leaked_raw(tag) },
            packed_tag,
            value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
        }))
    })
}

/// Takes ownership of one strong reference to the angle value data if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_font_style(
    font_style: u8,
    angle_value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::FontStyle {
            font_style,
            angle_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(angle_value) },
        }))
    })
}

/// Takes ownership of one strong reference to the length-percentage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_text_indent(
    length_percentage: *const StyleValueData,
    hanging: bool,
    each_line: bool,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::TextIndent {
            length_percentage: unsafe { RetainedStyleValueData::from_retained_pointer(length_percentage) },
            hanging,
            each_line,
        }))
    })
}

/// Takes ownership of one strong reference to the offset data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_overflow_clip_margin(
    has_visual_box: bool,
    visual_box: u8,
    offset: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::OverflowClipMargin {
            has_visual_box,
            visual_box,
            offset: unsafe { RetainedStyleValueData::from_retained_pointer(offset) },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_tree_counting_function(
    function: u8,
    computed_type: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::TreeCountingFunction {
            function,
            computed_type,
        }))
    })
}

/// Takes ownership of one strong reference to each size.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_background_size(
    size_x: *const StyleValueData,
    size_y: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::BackgroundSize {
            size_x: unsafe { RetainedStyleValueData::from_retained_pointer(size_x) },
            size_y: unsafe { RetainedStyleValueData::from_retained_pointer(size_y) },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_repeat_style(repeat_x: u8, repeat_y: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::RepeatStyle { repeat_x, repeat_y })))
}

/// Takes ownership of one strong reference to each offset data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_image_slice(
    top: *const StyleValueData,
    right: *const StyleValueData,
    bottom: *const StyleValueData,
    left: *const StyleValueData,
    fill: bool,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::BorderImageSlice {
            top: unsafe { RetainedStyleValueData::from_retained_pointer(top) },
            right: unsafe { RetainedStyleValueData::from_retained_pointer(right) },
            bottom: unsafe { RetainedStyleValueData::from_retained_pointer(bottom) },
            left: unsafe { RetainedStyleValueData::from_retained_pointer(left) },
            fill,
        }))
    })
}

/// Takes ownership of one leaked reference to the anchor name (0 when absent) and one strong
/// reference to the fallback value if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_anchor_size(
    has_anchor_name: bool,
    anchor_name: usize,
    has_anchor_size: bool,
    anchor_size: u8,
    fallback_value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::AnchorSize {
            has_anchor_name,
            anchor_name: unsafe { RetainedUtf16FlyString::from_leaked_raw(anchor_name) },
            has_anchor_size,
            anchor_size,
            fallback_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(fallback_value) },
        }))
    })
}

/// Takes ownership of one leaked reference to the anchor name (0 when absent), one strong
/// reference to the side and one strong reference to the fallback value if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_anchor(
    has_anchor_name: bool,
    anchor_name: usize,
    anchor_side: *const StyleValueData,
    fallback_value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Anchor {
            has_anchor_name,
            anchor_name: unsafe { RetainedUtf16FlyString::from_leaked_raw(anchor_name) },
            anchor_side: unsafe { RetainedStyleValueData::from_retained_pointer(anchor_side) },
            fallback_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(fallback_value) },
        }))
    })
}

/// Takes ownership of one strong reference to each edge data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_position(
    edge_x: *const StyleValueData,
    edge_y: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Position {
            edge_x: unsafe { RetainedStyleValueData::from_retained_pointer(edge_x) },
            edge_y: unsafe { RetainedStyleValueData::from_retained_pointer(edge_y) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null style value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_shadow(
    shadow_type: u8,
    color: *const StyleValueData,
    offset_x: *const StyleValueData,
    offset_y: *const StyleValueData,
    blur_radius: *const StyleValueData,
    spread_distance: *const StyleValueData,
    placement: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Shadow {
            shadow_type,
            color: unsafe { RetainedStyleValueData::from_retained_optional_pointer(color) },
            offset_x: unsafe { RetainedStyleValueData::from_retained_pointer(offset_x) },
            offset_y: unsafe { RetainedStyleValueData::from_retained_pointer(offset_y) },
            blur_radius: unsafe { RetainedStyleValueData::from_retained_optional_pointer(blur_radius) },
            spread_distance: unsafe { RetainedStyleValueData::from_retained_optional_pointer(spread_distance) },
            placement,
        }))
    })
}

/// Takes ownership of one strong reference to the content list and, when non-null, the
/// alt-text list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_content(
    content: *const StyleValueData,
    alt_text: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Content {
            content: unsafe { RetainedStyleValueData::from_retained_pointer(content) },
            alt_text: unsafe { RetainedStyleValueData::from_retained_optional_pointer(alt_text) },
        }))
    })
}

/// Takes ownership of one leaked reference to each string and one strong reference to the
/// counter style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter(
    function: u8,
    counter_name: usize,
    counter_style: *const StyleValueData,
    join_string: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Counter {
            function,
            counter_name: unsafe { RetainedUtf16FlyString::from_leaked_raw(counter_name) },
            counter_style: unsafe { RetainedStyleValueData::from_retained_pointer(counter_style) },
            join_string: unsafe { RetainedUtf16FlyString::from_leaked_raw(join_string) },
        }))
    })
}

/// Takes ownership of one strong reference to each color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_light_dark(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    light: *const StyleValueData,
    dark: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::LightDark {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            light: unsafe { RetainedStyleValueData::from_retained_pointer(light) },
            dark: unsafe { RetainedStyleValueData::from_retained_pointer(dark) },
        }))
    })
}

/// Takes ownership of one strong reference to the fixed value and one leaked reference to the
/// name when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_random_value_sharing(
    fixed_value: *const StyleValueData,
    is_auto: bool,
    has_name: bool,
    name: usize,
    element_shared: bool,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::RandomValueSharing {
            fixed_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(fixed_value) },
            is_auto,
            has_name,
            name: unsafe { RetainedUtf16FlyString::from_leaked_raw(name) },
            element_shared,
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_scrollbar_gutter(value: u8) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::ScrollbarGutter { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_color_interpolation_method(
    is_polar: bool,
    color_space: u8,
    hue_interpolation_method: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ColorInterpolationMethod {
            is_polar,
            color_space,
            hue_interpolation_method,
        }))
    })
}

/// Takes ownership of one strong reference to each of the `length` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_value_list(
    values: *const *const StyleValueData,
    length: usize,
    separator: u8,
    collapsible: bool,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ValueList {
            values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, length) },
            separator,
            collapsible,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_tuple(
    values: *const *const StyleValueData,
    length: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Tuple {
            values: unsafe { RetainedStyleValueDataList::from_retained_optional_pointers(values, length) },
        }))
    })
}

/// Takes ownership of one strong reference to each value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_transformation(
    property: u16,
    transform_function: u8,
    values: *const *const StyleValueData,
    length: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Transformation {
            property,
            transform_function,
            values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, length) },
        }))
    })
}

/// Takes ownership of one strong reference to each value; the property ids are copied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_shorthand(
    shorthand_property: u16,
    sub_properties: *const u16,
    sub_property_count: usize,
    values: *const *const StyleValueData,
    value_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Shorthand {
            shorthand_property,
            sub_properties: unsafe { RetainedPropertyIdList::from_raw(sub_properties, sub_property_count) },
            values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, value_count) },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_display(raw: u32) -> *const StyleValueData {
    abort_on_panic(|| Arc::into_raw(Arc::new(StyleValueData::Display { raw })))
}

/// Takes ownership of one strong reference to each non-null component value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_radial_size(
    component_count: u8,
    is_extent_0: bool,
    extent_0: u8,
    value_0: *const StyleValueData,
    is_extent_1: bool,
    extent_1: u8,
    value_1: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::RadialSize {
            component_count,
            is_extent_0,
            extent_0,
            value_0: unsafe { RetainedStyleValueData::from_retained_optional_pointer(value_0) },
            is_extent_1,
            extent_1,
            value_1: unsafe { RetainedStyleValueData::from_retained_optional_pointer(value_1) },
        }))
    })
}

/// Takes ownership of one leaked reference to the URL string and to each modifier's retained
/// string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_url(
    url: usize,
    url_bytes: *const u8,
    url_length: usize,
    url_type: u8,
    modifiers: *const RetainedRequestUrlModifier,
    modifier_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Url {
            url: unsafe { RetainedString::from_raw(url, url_bytes, url_length) },
            url_type,
            modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(modifiers, modifier_count) },
        }))
    })
}

/// Takes ownership of one strong reference to the local name if local, or one leaked reference
/// to the URL string and each modifier string otherwise, plus the format string when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_font_source(
    is_local: bool,
    local_name: *const StyleValueData,
    url: usize,
    url_bytes: *const u8,
    url_length: usize,
    url_type: u8,
    url_modifiers: *const RetainedRequestUrlModifier,
    url_modifier_count: usize,
    has_format: bool,
    format: usize,
    tech: *const u8,
    tech_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::FontSource {
            is_local,
            local_name: unsafe { RetainedStyleValueData::from_retained_optional_pointer(local_name) },
            url: unsafe { RetainedString::from_raw(url, url_bytes, url_length) },
            url_type,
            url_modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(url_modifiers, url_modifier_count) },
            has_format,
            format: unsafe { RetainedUtf16FlyString::from_leaked_raw(format) },
            tech: unsafe { RetainedByteList::from_raw(tech, tech_count) },
        }))
    })
}

/// Takes ownership of one leaked reference to each scheme name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_scheme(
    schemes: *const usize,
    scheme_codes: *const u8,
    scheme_count: usize,
    only: bool,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ColorScheme {
            schemes: unsafe { RetainedUtf16FlyStringList::from_raw(schemes, scheme_count) },
            scheme_codes: unsafe { RetainedByteList::from_raw(scheme_codes, scheme_count) },
            only,
        }))
    })
}

/// Takes ownership of one leaked reference to each string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_unresolved(
    source_text: usize,
    source_text_bytes: *const u8,
    source_text_length: usize,
    value_comparison_text: usize,
    value_comparison_text_bytes: *const u8,
    value_comparison_text_length: usize,
    presence_attr: bool,
    presence_dashed_function: bool,
    presence_env: bool,
    presence_if: bool,
    presence_inherit: bool,
    presence_var: bool,
    contains_attr_tainted_values: bool,
    parsed_value: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Unresolved {
            source_text: unsafe {
                RetainedReadableString::from_raw(source_text, source_text_bytes, source_text_length)
            },
            value_comparison_text: unsafe {
                RetainedReadableString::from_raw(
                    value_comparison_text,
                    value_comparison_text_bytes,
                    value_comparison_text_length,
                )
            },
            presence_attr,
            presence_dashed_function,
            presence_env,
            presence_if,
            presence_inherit,
            presence_var,
            contains_attr_tainted_values,
            parsed_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(parsed_value) },
        }))
    })
}

/// Takes ownership of the definitions' retained strings and values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_definitions(
    definitions: *const RetainedCounterDefinition,
    length: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::CounterDefinitions {
            counter_definitions: unsafe { RetainedCounterDefinitionList::from_raw(definitions, length) },
        }))
    })
}

/// Takes ownership of one strong reference to the value and one leaked reference to the name
/// when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_track_placement(
    kind: u8,
    value: *const StyleValueData,
    has_name: bool,
    name: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::GridTrackPlacement {
            kind,
            value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(value) },
            has_name,
            name: unsafe { RetainedUtf16FlyString::from_leaked_raw(name) },
        }))
    })
}

/// Takes ownership of one strong reference to the first symbol and one leaked reference to the
/// name when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_style_system(
    kind: u8,
    system: u8,
    first_symbol: *const StyleValueData,
    name: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::CounterStyleSystem {
            kind,
            system,
            first_symbol: unsafe { RetainedStyleValueData::from_retained_optional_pointer(first_symbol) },
            name: unsafe { RetainedUtf16FlyString::from_leaked_raw(name) },
        }))
    })
}

/// Takes ownership of one leaked reference to the name (0 when this is a symbols() function)
/// and to each symbol string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_style(
    is_symbols: bool,
    name: usize,
    symbols_type: u8,
    symbols: *const usize,
    symbol_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::CounterStyle {
            is_symbols,
            name: unsafe { RetainedUtf16FlyString::from_leaked_raw(name) },
            symbols_type,
            symbols: unsafe { RetainedUtf16FlyStringList::from_raw(symbols, symbol_count) },
        }))
    })
}

/// Takes ownership of one strong reference to the image and to each non-null coordinate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_cursor(
    image: *const StyleValueData,
    x: *const StyleValueData,
    y: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Cursor {
            image: unsafe { RetainedStyleValueData::from_retained_pointer(image) },
            x: unsafe { RetainedStyleValueData::from_retained_optional_pointer(x) },
            y: unsafe { RetainedStyleValueData::from_retained_optional_pointer(y) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and one leaked reference to
/// the name when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_function(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    channel_0: *const StyleValueData,
    channel_1: *const StyleValueData,
    channel_2: *const StyleValueData,
    alpha: *const StyleValueData,
    has_name: bool,
    name: usize,
    origin_color: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ColorFunction {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            channel_0: unsafe { RetainedStyleValueData::from_retained_pointer(channel_0) },
            channel_1: unsafe { RetainedStyleValueData::from_retained_pointer(channel_1) },
            channel_2: unsafe { RetainedStyleValueData::from_retained_pointer(channel_2) },
            alpha: unsafe { RetainedStyleValueData::from_retained_optional_pointer(alpha) },
            has_name,
            name: unsafe { RetainedUtf16FlyString::from_leaked_raw(name) },
            origin_color: unsafe { RetainedStyleValueData::from_retained_optional_pointer(origin_color) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_mix(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    color_interpolation_method: *const StyleValueData,
    first_color: *const StyleValueData,
    first_percentage: *const StyleValueData,
    second_color: *const StyleValueData,
    second_percentage: *const StyleValueData,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ColorMix {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            color_interpolation_method: unsafe {
                RetainedStyleValueData::from_retained_optional_pointer(color_interpolation_method)
            },
            first_color: unsafe { RetainedStyleValueData::from_retained_pointer(first_color) },
            first_percentage: unsafe { RetainedStyleValueData::from_retained_optional_pointer(first_percentage) },
            second_color: unsafe { RetainedStyleValueData::from_retained_pointer(second_color) },
            second_percentage: unsafe { RetainedStyleValueData::from_retained_optional_pointer(second_percentage) },
        }))
    })
}

/// Takes ownership of the options' retained values and strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_image_set(
    options: *const RetainedImageSetOption,
    length: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ImageSet {
            options: unsafe { RetainedImageSetOptionList::from_raw(options, length) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_linear_gradient(
    has_direction_value: bool,
    direction_value: *const StyleValueData,
    side_or_corner: u8,
    stops: *const RetainedColorStop,
    stop_count: usize,
    gradient_type: u8,
    repeating: bool,
    color_interpolation_method: *const StyleValueData,
    color_syntax: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::LinearGradient {
            has_direction_value,
            direction_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(direction_value) },
            side_or_corner,
            color_stop_list: unsafe { RetainedColorStopList::from_raw(stops, stop_count) },
            gradient_type,
            repeating,
            color_interpolation_method: unsafe {
                RetainedStyleValueData::from_retained_optional_pointer(color_interpolation_method)
            },
            color_syntax,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_conic_gradient(
    from_angle: *const StyleValueData,
    position: *const StyleValueData,
    stops: *const RetainedColorStop,
    stop_count: usize,
    repeating: bool,
    color_interpolation_method: *const StyleValueData,
    color_syntax: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::ConicGradient {
            from_angle: unsafe { RetainedStyleValueData::from_retained_optional_pointer(from_angle) },
            position: unsafe { RetainedStyleValueData::from_retained_pointer(position) },
            color_stop_list: unsafe { RetainedColorStopList::from_raw(stops, stop_count) },
            repeating,
            color_interpolation_method: unsafe {
                RetainedStyleValueData::from_retained_optional_pointer(color_interpolation_method)
            },
            color_syntax,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_radial_gradient(
    ending_shape: u8,
    size: *const StyleValueData,
    position: *const StyleValueData,
    stops: *const RetainedColorStop,
    stop_count: usize,
    repeating: bool,
    color_interpolation_method: *const StyleValueData,
    color_syntax: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::RadialGradient {
            ending_shape,
            size: unsafe { RetainedStyleValueData::from_retained_pointer(size) },
            position: unsafe { RetainedStyleValueData::from_retained_pointer(position) },
            color_stop_list: unsafe { RetainedColorStopList::from_raw(stops, stop_count) },
            repeating,
            color_interpolation_method: unsafe {
                RetainedStyleValueData::from_retained_optional_pointer(color_interpolation_method)
            },
            color_syntax,
        }))
    })
}

/// Takes ownership of the areas' retained names.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_template_area(
    areas: *const RetainedGridArea,
    area_count: usize,
    row_count: usize,
    column_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::GridTemplateArea {
            grid_areas: unsafe { RetainedGridAreaList::from_raw(areas, area_count) },
            row_count,
            column_count,
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value and of the stops' retained
/// values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_easing(
    kind: u8,
    linear_stops: *const RetainedLinearEasingStop,
    linear_stop_count: usize,
    x1: *const StyleValueData,
    y1: *const StyleValueData,
    x2: *const StyleValueData,
    y2: *const StyleValueData,
    number_of_intervals: *const StyleValueData,
    step_position: u8,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Easing {
            kind,
            linear_stops: unsafe { RetainedLinearEasingStopList::from_raw(linear_stops, linear_stop_count) },
            x1: unsafe { RetainedStyleValueData::from_retained_optional_pointer(x1) },
            y1: unsafe { RetainedStyleValueData::from_retained_optional_pointer(y1) },
            x2: unsafe { RetainedStyleValueData::from_retained_optional_pointer(x2) },
            y2: unsafe { RetainedStyleValueData::from_retained_optional_pointer(y2) },
            number_of_intervals: unsafe { RetainedStyleValueData::from_retained_optional_pointer(number_of_intervals) },
            step_position,
        }))
    })
}

/// Takes ownership of the entries' retained values and names, recursively.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_track_size_list(
    is_subgrid: bool,
    preserve_line_name_sets: bool,
    entries: *const GridTrackEntryInput,
    entry_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::GridTrackSizeList {
            is_subgrid,
            preserve_line_name_sets,
            entries: unsafe { RetainedGridTrackEntryList::from_raw(entries, entry_count) },
        }))
    })
}

/// Takes ownership of one strong reference to each non-null value, of the points' retained
/// values and of one leaked reference to the path string when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_basic_shape(
    kind: u8,
    v0: *const StyleValueData,
    v1: *const StyleValueData,
    v2: *const StyleValueData,
    v3: *const StyleValueData,
    v4: *const StyleValueData,
    fill_rule: u8,
    points: *const RetainedShapePoint,
    point_count: usize,
    path_string: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::BasicShape {
            kind,
            v0: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v0) },
            v1: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v1) },
            v2: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v2) },
            v3: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v3) },
            v4: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v4) },
            fill_rule,
            points: unsafe { RetainedShapePointList::from_raw(points, point_count) },
            path_string: unsafe { RetainedUtf16FlyString::from_leaked_raw(path_string) },
        }))
    })
}

/// Takes ownership of one strong reference to the calculation node; the byte blob and ranges
/// are copied.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_calculated(
    rust_calculation: *const crate::css::calc::CalcNode,
    resolved_type: crate::css::calc::FfiNumericType,
    has_percentages_resolve_as: bool,
    resolve_as_is_number: bool,
    resolve_as_base: u8,
    percentages_resolve_as: u8,
    resolve_numbers_as_integers: bool,
    accepted_ranges: *const RetainedNumericRangeByType,
    accepted_range_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Calculated {
            rust_calculation: unsafe { crate::css::calc::CalcNodeHandle::from_raw(rust_calculation) },
            resolve_as_is_number,
            resolve_as_base,
            resolved_type,
            has_percentages_resolve_as,
            percentages_resolve_as,
            resolve_numbers_as_integers,
            accepted_ranges: unsafe { RetainedNumericRangeList::from_raw(accepted_ranges, accepted_range_count) },
        }))
    })
}

/// Takes ownership of one leaked reference to the URL string and each modifier string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_image(
    url: usize,
    url_bytes: *const u8,
    url_length: usize,
    url_type: u8,
    url_modifiers: *const RetainedRequestUrlModifier,
    url_modifier_count: usize,
) -> *const StyleValueData {
    abort_on_panic(|| {
        Arc::into_raw(Arc::new(StyleValueData::Image {
            url: unsafe { RetainedString::from_raw(url, url_bytes, url_length) },
            url_type,
            url_modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(url_modifiers, url_modifier_count) },
        }))
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_release(value: *const StyleValueData) {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueDestroyEntry);
    abort_on_panic(|| {
        if value.is_null() {
            return;
        }
        unsafe { Arc::decrement_strong_count(value) };
    });
}

/// Retains one reference to a shared style value allocation.
///
/// # Safety
/// `value` must be null or point at a live `StyleValueData` allocated by this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_retain(value: *const StyleValueData) -> *const StyleValueData {
    abort_on_panic(|| {
        if !value.is_null() {
            unsafe { Arc::increment_strong_count(value) };
        }
        value
    })
}

/// Whether a value's computed color depends on the element's used currentcolor: the
/// currentcolor keyword itself, or a color function, color-mix(), contrast-color() or
/// light-dark() whose nested colors do.
pub(crate) fn value_depends_on_current_color(value: &StyleValueData) -> bool {
    let retained_data_depends =
        |retained: &RetainedStyleValueData| -> bool { value_depends_on_current_color(retained.data()) };
    match value {
        StyleValueData::Keyword { keyword } => *keyword == crate::css::style_compute::keyword::CURRENTCOLOR,
        StyleValueData::ColorFunction { origin_color, .. } => {
            origin_color.optional_data().is_some_and(value_depends_on_current_color)
        }
        StyleValueData::ColorMix {
            first_color,
            second_color,
            ..
        } => retained_data_depends(first_color) || retained_data_depends(second_color),
        StyleValueData::ContrastColor { color, .. } => retained_data_depends(color),
        StyleValueData::LightDark { light, dark, .. } => retained_data_depends(light) || retained_data_depends(dark),
        _ => false,
    }
}

/// # Safety
/// `data` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_depends_on_current_color(data: *const c_void) -> bool {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueQueryEntry);
    crate::abort_on_panic(|| value_depends_on_current_color(unsafe { &*(data as *const StyleValueData) }))
}
