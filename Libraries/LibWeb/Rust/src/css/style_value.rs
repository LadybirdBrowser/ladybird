/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned CSS style value data.
//!
//! The C++ StyleValue subclasses keep their data in a Rust-owned, reference-counted
//! [`StyleValueData`] allocation instead of C++ member variables. The layout of
//! [`StyleValueData`] is exposed to C++
//! through cbindgen so that hot accessors compile to inline field reads with no FFI call.

use std::ffi::c_void;
use std::sync::Arc;

#[cfg(any(test, feature = "style-replay"))]
use std::cell::RefCell;
#[cfg(any(test, feature = "style-replay"))]
use std::collections::HashMap;

use crate::css::css_tokenizer::{ParserSource, ParserTokenKind, SourcePosition, TokenizerInput};
use crate::css::parser::component_value::{ComponentKind, ComponentSerializationMode, ComponentValue};

pub(crate) use crate::css::css_string::{CssString, CssStringList};

/// A Rust-owned component-value slice. C++ retains the opaque allocation as part of an
/// unresolved style value but never inspects its elements.
#[repr(C)]
pub struct RetainedComponentValueList {
    pointer: *mut c_void,
    length: usize,
}

// SAFETY: This handle owns an immutable boxed slice. The bounds cover the erased element type.
unsafe impl Send for RetainedComponentValueList where ComponentValue: Send {}
unsafe impl Sync for RetainedComponentValueList where ComponentValue: Sync {}

impl RetainedComponentValueList {
    pub(crate) fn from_values(values: Vec<ComponentValue>) -> Self {
        let mut values = values.into_boxed_slice();
        let result = Self {
            pointer: values.as_mut_ptr().cast(),
            length: values.len(),
        };
        std::mem::forget(values);
        result
    }

    pub(crate) fn from_source(source: &[u16]) -> Self {
        let values = crate::css::parser::component_value::consume_a_list_of_component_values(
            crate::css::css_tokenizer::tokenize_for_parser(source),
        )
        .unwrap_or_default();
        Self::from_values(values)
    }

    pub(crate) fn as_slice(&self) -> &[ComponentValue] {
        if self.pointer.is_null() {
            return &[];
        }
        // SAFETY: The allocation is owned by this handle and has `length` initialized elements.
        unsafe { std::slice::from_raw_parts(self.pointer.cast(), self.length) }
    }
}

enum ReifiedUnresolvedSegment {
    Text(Vec<u16>),
    Variable {
        name: Vec<u16>,
        fallback: Option<Vec<ReifiedUnresolvedSegment>>,
    },
}

fn synthetic_component(kind: ParserTokenKind) -> ComponentValue {
    ComponentValue {
        kind: ComponentKind::Token(kind),
        original_source_text: ParserSource::empty(),
        opening_source_length: 0,
        closing_source_length: 0,
        start_position: SourcePosition::default(),
        end_position: SourcePosition::default(),
    }
}

fn flush_reification_text(pending: &mut Vec<ComponentValue>, output: &mut Vec<ReifiedUnresolvedSegment>) {
    if pending.is_empty() {
        return;
    }
    output.push(ReifiedUnresolvedSegment::Text(
        crate::css::serialize::serialize_component_values_to_utf16(pending, ComponentSerializationMode::Normalized),
    ));
    pending.clear();
}

fn component_values_without_whitespace(mut values: &[ComponentValue]) -> &[ComponentValue] {
    while values.first().is_some_and(ComponentValue::is_whitespace) {
        values = &values[1..];
    }
    while values.last().is_some_and(ComponentValue::is_whitespace) {
        values = &values[..values.len() - 1];
    }
    values
}

fn utf16_equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left).is_ok_and(|left| left.eq_ignore_ascii_case(&right)))
}

fn split_var_arguments(values: &[ComponentValue]) -> Option<(&[u16], Option<&[ComponentValue]>)> {
    if !crate::css::parser::arbitrary_substitution::arguments_are_valid_for_ffi(5, values) {
        return None;
    }
    let comma = values.iter().position(ComponentValue::is_comma);
    let name_values = component_values_without_whitespace(&values[..comma.unwrap_or(values.len())]);
    let [name_value] = name_values else {
        return None;
    };
    let name = name_value.ident()?;
    if name.len() < 2 || name[0] != u16::from(b'-') || name[1] != u16::from(b'-') {
        return None;
    }
    Some((name, comma.map(|comma| &values[comma + 1..])))
}

fn append_reified_unresolved_segments(
    values: &[ComponentValue],
    pending: &mut Vec<ComponentValue>,
    output: &mut Vec<ReifiedUnresolvedSegment>,
) {
    for value in values {
        match &value.kind {
            ComponentKind::Function { name, values }
                if utf16_equals_ascii_case_insensitive(name, b"var") && split_var_arguments(values).is_some() =>
            {
                let (name, fallback) = split_var_arguments(values).expect("validated var arguments");
                flush_reification_text(pending, output);
                let fallback = fallback.map(reify_unresolved_segments);
                output.push(ReifiedUnresolvedSegment::Variable {
                    name: name.to_vec(),
                    fallback,
                });
            }
            ComponentKind::Function { name, values } => {
                pending.push(synthetic_component(ParserTokenKind::Function(name.clone())));
                append_reified_unresolved_segments(values, pending, output);
                pending.push(synthetic_component(ParserTokenKind::CloseParen));
            }
            ComponentKind::SimpleBlock { opening, values } => {
                pending.push(synthetic_component(opening.clone()));
                append_reified_unresolved_segments(values, pending, output);
                let closing = match opening {
                    ParserTokenKind::OpenSquare => ParserTokenKind::CloseSquare,
                    ParserTokenKind::OpenParen => ParserTokenKind::CloseParen,
                    ParserTokenKind::OpenCurly => ParserTokenKind::CloseCurly,
                    _ => unreachable!(),
                };
                pending.push(synthetic_component(closing));
            }
            ComponentKind::Token(_) => pending.push(value.clone()),
        }
    }
}

fn reify_unresolved_segments(values: &[ComponentValue]) -> Vec<ReifiedUnresolvedSegment> {
    let mut output = Vec::new();
    let mut pending = Vec::new();
    append_reified_unresolved_segments(values, &mut pending, &mut output);
    flush_reification_text(&mut pending, &mut output);
    output
}

fn visit_reified_unresolved_segments(
    segments: &[ReifiedUnresolvedSegment],
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, u8, *const u16, usize, bool),
) {
    for segment in segments {
        match segment {
            ReifiedUnresolvedSegment::Text(text) => unsafe {
                visit(context, 0, text.as_ptr(), text.len(), false);
            },
            ReifiedUnresolvedSegment::Variable { name, fallback } => {
                unsafe {
                    visit(context, 1, name.as_ptr(), name.len(), fallback.is_some());
                }
                if let Some(fallback) = fallback {
                    visit_reified_unresolved_segments(fallback, context, visit);
                }
                unsafe {
                    visit(context, 2, std::ptr::null(), 0, false);
                }
            }
        }
    }
}

fn scan_custom_property_references(
    values: &[ComponentValue],
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const u16, usize),
) -> bool {
    let mut all_references_visible = true;
    for value in values {
        let ComponentKind::Function {
            name,
            values: function_values,
        } = &value.kind
        else {
            if let ComponentKind::SimpleBlock { values, .. } = &value.kind {
                all_references_visible &= scan_custom_property_references(values, context, visit);
            }
            continue;
        };
        if utf16_equals_ascii_case_insensitive(name, b"var") || utf16_equals_ascii_case_insensitive(name, b"inherit") {
            let comma = function_values.iter().position(ComponentValue::is_comma);
            let name_values =
                component_values_without_whitespace(&function_values[..comma.unwrap_or(function_values.len())]);
            if let [name_value] = name_values
                && let Some(name) = name_value.ident()
                && name.len() >= 2
                && name[0] == u16::from(b'-')
                && name[1] == u16::from(b'-')
            {
                unsafe { visit(context, name.as_ptr(), name.len()) };
            } else {
                all_references_visible = false;
            }
        }
        all_references_visible &= scan_custom_property_references(function_values, context, visit);
    }
    all_references_visible
}

pub(crate) fn custom_property_references(value: &StyleValueData) -> Option<(Vec<Vec<u16>>, bool)> {
    let StyleValueData::Unresolved { components, .. } = value else {
        return None;
    };
    let mut references = Vec::new();
    unsafe extern "C" fn collect(context: *mut c_void, name: *const u16, name_length: usize) {
        let references = unsafe { &mut *context.cast::<Vec<Vec<u16>>>() };
        references.push(unsafe { std::slice::from_raw_parts(name, name_length) }.to_vec());
    }
    let all_references_visible =
        scan_custom_property_references(components.as_slice(), (&raw mut references).cast(), collect);
    Some((references, all_references_visible))
}

/// Visits the Typed OM reification of an unresolved style value. Event 0 appends a text segment,
/// event 1 begins a variable reference, and event 2 ends its optional fallback.
///
/// # Safety
/// `value` must point at live unresolved style value data, and `visit` must remain callable for
/// the duration of this function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_unresolved_style_value_visit_reification(
    value: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, u8, *const u16, usize, bool),
) {
    let StyleValueData::Unresolved { components, .. } = (unsafe { &*value.cast::<StyleValueData>() }) else {
        unreachable!("reification requires an unresolved style value");
    };
    let segments = reify_unresolved_segments(components.as_slice());
    visit_reified_unresolved_segments(&segments, context, visit);
}

/// Visits the name of every custom property a `var()` in an unresolved value refers to, fallbacks
/// and nested functions included. Returns whether every reference names its property with a plain
/// identifier; a reference that substitutes its name can read anything.
///
/// # Safety
/// `value` must point at live unresolved style value data, and `visit` must remain callable for
/// the duration of this function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_unresolved_style_value_visit_custom_property_references(
    value: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, *const u16, usize),
) -> bool {
    let StyleValueData::Unresolved { components, .. } = (unsafe { &*value.cast::<StyleValueData>() }) else {
        return false;
    };
    scan_custom_property_references(components.as_slice(), context, visit)
}

#[cfg(test)]
mod unresolved_component_tests {
    use super::*;

    fn components(source: &str) -> Vec<ComponentValue> {
        crate::css::parser::component_value::consume_a_list_of_component_values(
            crate::css::css_tokenizer::tokenize_for_parser(source.as_bytes()),
        )
        .unwrap()
    }

    #[test]
    fn reification_extracts_nested_var_references() {
        let segments = reify_unresolved_segments(&components("outer(var(--name, red))"));
        assert!(matches!(&segments[..], [
            ReifiedUnresolvedSegment::Text(before),
            ReifiedUnresolvedSegment::Variable { name, fallback: Some(fallback) },
            ReifiedUnresolvedSegment::Text(after),
        ] if before == &"outer(".encode_utf16().collect::<Vec<_>>()
            && name == &"--name".encode_utf16().collect::<Vec<_>>()
            && matches!(&fallback[..], [ReifiedUnresolvedSegment::Text(text)] if text == &" red".encode_utf16().collect::<Vec<_>>())
            && after == &")".encode_utf16().collect::<Vec<_>>()));
    }

    #[test]
    fn reification_descends_into_unrepresentable_var() {
        let segments = reify_unresolved_segments(&components("var(var(--name))"));
        assert!(matches!(&segments[..], [
            ReifiedUnresolvedSegment::Text(before),
            ReifiedUnresolvedSegment::Variable { name, fallback: None },
            ReifiedUnresolvedSegment::Text(after),
        ] if before == &"var(".encode_utf16().collect::<Vec<_>>()
            && name == &"--name".encode_utf16().collect::<Vec<_>>()
            && after == &")".encode_utf16().collect::<Vec<_>>()));
    }

    unsafe extern "C" fn collect_reference(context: *mut c_void, name: *const u16, name_length: usize) {
        let names = unsafe { &mut *context.cast::<Vec<Vec<u16>>>() };
        names.push(unsafe { std::slice::from_raw_parts(name, name_length) }.to_vec());
    }

    #[test]
    fn reference_scan_reports_dynamic_names() {
        let values = components("var(--one) [inherit(--two)] var(var(--dynamic))");
        let mut names: Vec<Vec<u16>> = Vec::new();
        let all_visible = scan_custom_property_references(&values, (&raw mut names).cast(), collect_reference);
        assert!(!all_visible);
        assert_eq!(
            names,
            ["--one", "--two", "--dynamic"].map(|name| name.encode_utf16().collect::<Vec<_>>())
        );
    }
}

impl Clone for RetainedComponentValueList {
    fn clone(&self) -> Self {
        Self::from_values(self.as_slice().to_vec())
    }
}

impl PartialEq for RetainedComponentValueList {
    fn eq(&self, _other: &Self) -> bool {
        // The retained tree is a parsing/serialization cache. Unresolved value identity is
        // defined by the source and comparison text fields, as it was before this cache existed.
        true
    }
}

impl Drop for RetainedComponentValueList {
    fn drop(&mut self) {
        if self.pointer.is_null() {
            return;
        }
        // SAFETY: `from_values` leaked exactly this boxed slice to the handle.
        unsafe {
            drop(Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                self.pointer.cast::<ComponentValue>(),
                self.length,
            )));
        }
    }
}

#[cfg(any(test, feature = "style-replay"))]
thread_local! {
    static REPLAY_STYLE_VALUES: RefCell<HashMap<u64, usize>> = RefCell::new(HashMap::new());
}

#[cfg(any(test, feature = "style-replay"))]
#[derive(Clone, Copy)]
struct ReplayStyleValue {
    token: u64,
    dependency_flags: u8,
}

#[cfg(any(test, feature = "style-replay"))]
fn replay_style_values() -> &'static std::sync::Mutex<HashMap<usize, Box<ReplayStyleValue>>> {
    static VALUES: std::sync::OnceLock<std::sync::Mutex<HashMap<usize, Box<ReplayStyleValue>>>> =
        std::sync::OnceLock::new();
    VALUES.get_or_init(Default::default)
}

#[cfg(any(test, feature = "style-replay"))]
pub(crate) fn register_replay_style_value(token: u64, dependency_flags: u8) -> *const StyleValueData {
    assert!(token != 0, "style-value tokens are nonzero");
    // Replay sessions may reuse token numbers. Allocate distinct opaque identities per session
    // thread, but keep their metadata accessible when a retained handle moves to another thread.
    let pointer = REPLAY_STYLE_VALUES.with(|values| {
        *values.borrow_mut().entry(token).or_insert_with(|| {
            let value = Box::new(ReplayStyleValue {
                token,
                dependency_flags,
            });
            let pointer = (&*value as *const ReplayStyleValue) as usize;
            replay_style_values().lock().unwrap().insert(pointer, value);
            pointer
        })
    });
    assert_eq!(
        replay_style_value_metadata(pointer as *const StyleValueData)
            .unwrap()
            .dependency_flags,
        dependency_flags
    );
    pointer as *const StyleValueData
}

#[cfg(any(test, feature = "style-replay"))]
fn replay_style_value_metadata(value: *const StyleValueData) -> Option<ReplayStyleValue> {
    replay_style_values()
        .lock()
        .unwrap()
        .get(&(value as usize))
        .map(|value| **value)
}

#[cfg(any(test, feature = "style-replay"))]
pub(crate) fn replay_style_value_token(value: *const StyleValueData) -> Option<u64> {
    replay_style_value_metadata(value).map(|value| value.token)
}

fn replay_style_value_dependency_flags(value: *const StyleValueData) -> Option<u8> {
    #[cfg(any(test, feature = "style-replay"))]
    return replay_style_value_metadata(value).map(|value| value.dependency_flags);
    #[cfg(not(any(test, feature = "style-replay")))]
    {
        let _ = value;
        None
    }
}

/// # Safety
/// `value` must be null, a registered replay token, or point to a live `StyleValueData`.
pub(crate) unsafe fn style_value_content_hash(value: *const StyleValueData) -> u64 {
    if value.is_null() {
        return 0;
    }
    #[cfg(any(test, feature = "style-replay"))]
    if let Some(value) = replay_style_value_metadata(value) {
        return value.token;
    }
    unsafe { &*value }.content_hash()
}

/// A strong reference to immutable Rust-owned style value data.
#[repr(C)]
pub struct RetainedStyleValueData {
    pointer: *const c_void,
}

// SAFETY: This handle owns an Arc reference and only exposes shared access to its pointee.
unsafe impl Send for RetainedStyleValueData where StyleValueData: Send + Sync {}
unsafe impl Sync for RetainedStyleValueData where StyleValueData: Send + Sync {}

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
    pub(crate) fn none() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    pub(crate) fn from_owned(data: StyleValueData) -> Self {
        let pointer = Arc::into_raw(Arc::new(data));
        // SAFETY: Arc::into_raw transfers one strong reference to this handle.
        unsafe { Self::from_retained_pointer(pointer) }
    }

    /// Borrow the handles' pointer storage without copying or transferring ownership.
    pub(crate) fn pointer_slice(values: &[Self]) -> &[*const c_void] {
        const {
            assert!(size_of::<RetainedStyleValueData>() == size_of::<*const c_void>());
            assert!(align_of::<RetainedStyleValueData>() == align_of::<*const c_void>());
        }
        // SAFETY: This repr(C) type has exactly one field, a *const c_void, so its
        //         size, alignment and layout match that pointer. The shared slice
        //         borrows the handles and cannot outlive or modify their storage.
        unsafe { std::slice::from_raw_parts(values.as_ptr().cast(), values.len()) }
    }

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
        let pointer = unsafe { retain_style_value(self.pointer.cast()) };
        unsafe { Self::from_retained_optional_pointer(pointer) }
    }

    /// Converts the retained reference into an Arc, transferring the strong reference.
    pub(crate) fn into_arc(self) -> std::sync::Arc<StyleValueData> {
        let pointer = self.pointer();
        std::mem::forget(self);
        // SAFETY: The retained reference owns exactly one strong reference.
        unsafe { std::sync::Arc::from_raw(pointer) }
    }
}

impl Clone for RetainedStyleValueData {
    fn clone(&self) -> Self {
        self.clone_retained()
    }
}

impl Drop for RetainedStyleValueData {
    fn drop(&mut self) {
        unsafe { release_style_value(self.pointer.cast()) };
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

impl Clone for RetainedStyleValueDataList {
    fn clone(&self) -> Self {
        let mut cloned_by_pointer = Vec::new();
        let values = self
            .as_slice()
            .iter()
            .map(|value| {
                let Some(data) = value.optional_data() else {
                    return unsafe { RetainedStyleValueData::from_retained_optional_pointer(std::ptr::null()) };
                };
                let pointer = data as *const StyleValueData as usize;
                if let Some((_, cloned)) = cloned_by_pointer.iter().find(|(original, _)| *original == pointer) {
                    return unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(*cloned)) };
                }
                let cloned = Arc::into_raw(Arc::new(data.clone()));
                cloned_by_pointer.push((pointer, cloned));
                unsafe { RetainedStyleValueData::from_retained_pointer(cloned) }
            })
            .collect();
        Self::from_retained_values(values)
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
        impl Clone for $list {
            fn clone(&self) -> Self {
                if self.pointer.is_null() {
                    return Self {
                        pointer: std::ptr::null_mut(),
                        length: 0,
                    };
                }
                let elements = unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
                    .to_vec()
                    .into_boxed_slice();
                let length = elements.len();
                let pointer = Box::into_raw(elements).cast::<$element>();
                Self { pointer, length }
            }
        }
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
    pub(crate) fn from_property_ids(property_ids: Vec<u16>) -> Self {
        let slice = property_ids.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice).cast::<u16>();
        Self { pointer, length }
    }

    pub(crate) fn as_slice(&self) -> &[u16] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
    }
}

/// Shared native URL text, independent of host strings and the atom table.
#[repr(C)]
pub struct RetainedString {
    storage: *const c_void,
}

// SAFETY: This handle owns an Arc to immutable ASCII or UTF-16 storage.
unsafe impl Send for RetainedString {}
unsafe impl Sync for RetainedString {}

impl RetainedString {
    fn from_storage(storage: crate::css::css_tokenizer::SourceStorage) -> Self {
        Self {
            storage: Arc::into_raw(Arc::new(storage)).cast(),
        }
    }

    fn from_units(units: TokenizerInput<'_>) -> Self {
        use crate::css::css_tokenizer::SourceStorage;
        Self::from_storage(match units {
            TokenizerInput::Ascii(bytes) => {
                assert!(bytes.is_ascii());
                SourceStorage::Ascii(bytes.into())
            }
            TokenizerInput::Utf16(units) if units.iter().all(|unit| *unit <= 0x7f) => {
                SourceStorage::Ascii(units.iter().map(|unit| *unit as u8).collect())
            }
            TokenizerInput::Utf16(units) => SourceStorage::Utf16(units.into()),
        })
    }

    pub(crate) fn from_ascii(string: String) -> Self {
        assert!(string.is_ascii());
        Self::from_storage(crate::css::css_tokenizer::SourceStorage::Ascii(
            string.into_bytes().into_boxed_slice(),
        ))
    }

    pub(crate) fn from_utf16(string: &[u16]) -> Option<Self> {
        if char::decode_utf16(string.iter().copied()).any(|character| character.is_err()) {
            return None;
        }
        Some(Self::from_units(TokenizerInput::Utf16(string)))
    }

    pub(crate) fn units(&self) -> TokenizerInput<'_> {
        use crate::css::css_tokenizer::SourceStorage;
        match unsafe { &*self.storage.cast::<SourceStorage>() } {
            SourceStorage::Ascii(bytes) => TokenizerInput::Ascii(bytes),
            SourceStorage::Utf16(units) => TokenizerInput::Utf16(units),
        }
    }

    pub(crate) fn url_input(&self) -> liburl_rust::url::UrlInput<'_> {
        match self.units() {
            TokenizerInput::Ascii(bytes) => {
                liburl_rust::url::UrlInput::Utf8(unsafe { std::str::from_utf8_unchecked(bytes) })
            }
            TokenizerInput::Utf16(units) => liburl_rust::url::UrlInput::Utf16(units),
        }
    }

    pub(crate) fn ascii_bytes(&self) -> &[u8] {
        let TokenizerInput::Ascii(bytes) = self.units() else {
            panic!("Expected a serialized ASCII URL")
        };
        bytes
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.units().is_empty()
    }

    pub(crate) fn is_fragment(&self) -> bool {
        !self.is_empty() && self.units().code_unit_at(0) == u16::from(b'#')
    }

    fn hash(&self, hasher: &mut impl std::hash::Hasher) {
        let units = self.units();
        for index in 0..units.len() {
            hasher.write_u16(units.code_unit_at(index));
        }
    }
}

impl PartialEq for RetainedString {
    fn eq(&self, other: &Self) -> bool {
        if self.storage == other.storage {
            return true;
        }
        let left = self.units();
        let right = other.units();
        left.len() == right.len() && (0..left.len()).all(|index| left.code_unit_at(index) == right.code_unit_at(index))
    }
}

impl Clone for RetainedString {
    fn clone(&self) -> Self {
        unsafe { Arc::increment_strong_count(self.storage.cast::<crate::css::css_tokenizer::SourceStorage>()) };
        Self { storage: self.storage }
    }
}

impl Drop for RetainedString {
    fn drop(&mut self) {
        unsafe { Arc::decrement_strong_count(self.storage.cast::<crate::css::css_tokenizer::SourceStorage>()) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_url_text_view(text: &RetainedString) -> crate::css::ffi_support::FfiUtf16View {
    use crate::css::ffi_support::FfiUtf16View;
    match text.units() {
        TokenizerInput::Ascii(bytes) => FfiUtf16View {
            ascii: bytes.as_ptr(),
            utf16: std::ptr::null(),
            length: bytes.len(),
        },
        TokenizerInput::Utf16(units) => FfiUtf16View {
            ascii: std::ptr::null(),
            utf16: units.as_ptr(),
            length: units.len(),
        },
    }
}

/// Rust-owned text preserving its ASCII or UTF-16 representation.
#[repr(C)]
pub struct RetainedReadableString {
    ascii_units: *mut u8,
    code_units: *mut u16,
    length: usize,
}

// SAFETY: Both pointers refer exclusively to owned code-unit buffers, with no host references.
unsafe impl Send for RetainedReadableString {}
unsafe impl Sync for RetainedReadableString {}

impl RetainedReadableString {
    fn from_units(source: TokenizerInput<'_>) -> Self {
        let (ascii_units, code_units, length) = match source {
            TokenizerInput::Ascii(units) => {
                let units = units.to_vec().into_boxed_slice();
                let length = units.len();
                (Box::into_raw(units).cast::<u8>(), std::ptr::null_mut(), length)
            }
            TokenizerInput::Utf16(units) => {
                let units = units.to_vec().into_boxed_slice();
                let length = units.len();
                (std::ptr::null_mut(), Box::into_raw(units).cast::<u16>(), length)
            }
        };
        Self {
            ascii_units,
            code_units,
            length,
        }
    }

    pub(crate) fn from_utf16(code_units: &[u16]) -> Self {
        Self::from_units(TokenizerInput::Utf16(code_units))
    }

    pub(crate) fn as_units(&self) -> TokenizerInput<'_> {
        unsafe { TokenizerInput::from_raw_parts(self.ascii_units, self.code_units, self.length) }.unwrap()
    }
}

impl PartialEq for RetainedReadableString {
    fn eq(&self, other: &Self) -> bool {
        let mut left = Vec::with_capacity(self.length);
        let mut right = Vec::with_capacity(other.length);
        self.as_units().append_to(&mut left);
        other.as_units().append_to(&mut right);
        left == right
    }
}

impl Clone for RetainedReadableString {
    fn clone(&self) -> Self {
        Self::from_units(self.as_units())
    }
}

impl Drop for RetainedReadableString {
    fn drop(&mut self) {
        if !self.ascii_units.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.ascii_units, self.length)) });
        } else if !self.code_units.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.code_units, self.length)) });
        }
    }
}

/// A retained CSS request URL modifier: the modifier type and either an enum value or a retained
/// string value (raw 0 when the value is an enum). All enums are C++ `enum class ... : u8`
/// values, opaque to Rust.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RetainedRequestUrlModifier {
    modifier_type: u8,
    enum_value: u8,
    string_value: CssString,
}

/// Host input records carry leaked AK string references, never Rust string handles.
#[repr(C)]
pub struct FfiRequestUrlModifier {
    modifier_type: u8,
    enum_value: u8,
    string_value: usize,
}

#[repr(C)]
pub struct FfiCounterDefinition {
    name: usize,
    is_reversed: bool,
    value: RetainedStyleValueData,
}

#[repr(C)]
pub struct FfiImageSetOption {
    image: RetainedStyleValueData,
    resolution: RetainedStyleValueData,
    has_type: bool,
    type_string: usize,
}

impl RetainedRequestUrlModifier {
    pub(crate) fn from_enum(modifier_type: u8, enum_value: u8) -> Self {
        Self {
            modifier_type,
            enum_value,
            string_value: CssString::none(),
        }
    }

    pub(crate) fn from_string(modifier_type: u8, string_value: CssString) -> Self {
        Self {
            modifier_type,
            enum_value: 0,
            string_value,
        }
    }

    pub(crate) fn modifier_type(&self) -> u8 {
        self.modifier_type
    }

    pub(crate) fn enum_value(&self) -> u8 {
        self.enum_value
    }

    pub(crate) fn string_value(&self) -> &CssString {
        &self.string_value
    }
}

/// A Rust-owned array of retained request URL modifiers.
#[repr(C)]
pub struct RetainedRequestUrlModifierList {
    pointer: *mut RetainedRequestUrlModifier,
    length: usize,
}

impl RetainedRequestUrlModifierList {
    pub(crate) fn from_retained_modifiers(modifiers: Vec<RetainedRequestUrlModifier>) -> Self {
        let slice = modifiers.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedRequestUrlModifier;
        Self { pointer, length }
    }

    /// Copies and releases each modifier's leaked host string reference.
    unsafe fn from_raw(elements: *const FfiRequestUrlModifier, length: usize) -> Self {
        let slice: Box<[RetainedRequestUrlModifier]> = (0..length)
            .map(|i| {
                let element = unsafe { &*elements.add(i) };
                RetainedRequestUrlModifier {
                    modifier_type: element.modifier_type,
                    enum_value: element.enum_value,
                    string_value: unsafe { CssString::from_leaked_raw(element.string_value) },
                }
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedRequestUrlModifier;
        Self { pointer, length }
    }
}

retained_list_drop!(RetainedRequestUrlModifierList);

impl Clone for RetainedRequestUrlModifierList {
    fn clone(&self) -> Self {
        let slice = self.as_slice().to_vec().into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedRequestUrlModifier;
        Self { pointer, length }
    }
}

/// A Rust-owned array of bytes, used for lists of C++ u8 enum values.
#[repr(C)]
pub struct RetainedByteList {
    pointer: *mut u8,
    length: usize,
}

retained_list_drop!(RetainedByteList);

impl Clone for RetainedByteList {
    fn clone(&self) -> Self {
        Self::from_bytes(self.as_slice().to_vec())
    }
}

impl RetainedByteList {
    pub(crate) fn from_bytes(bytes: Vec<u8>) -> Self {
        let slice = bytes.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice).cast::<u8>();
        Self { pointer, length }
    }

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
#[derive(Clone, PartialEq)]
pub struct RetainedCounterDefinition {
    name: CssString,
    is_reversed: bool,
    value: RetainedStyleValueData,
}

impl RetainedCounterDefinition {
    pub(crate) fn new(name: CssString, is_reversed: bool, value: RetainedStyleValueData) -> Self {
        Self {
            name,
            is_reversed,
            value,
        }
    }

    pub(crate) fn name(&self) -> &CssString {
        &self.name
    }

    pub(crate) fn is_reversed(&self) -> bool {
        self.is_reversed
    }

    pub(crate) fn with_value(&self, value: RetainedStyleValueData) -> Self {
        Self {
            name: self.name.clone(),
            is_reversed: self.is_reversed,
            value,
        }
    }
}

/// A Rust-owned array of retained counter definitions.
#[repr(C)]
pub struct RetainedCounterDefinitionList {
    pointer: *mut RetainedCounterDefinition,
    length: usize,
}

impl RetainedCounterDefinitionList {
    unsafe fn from_raw(elements: *const FfiCounterDefinition, length: usize) -> Self {
        let elements = (0..length)
            .map(|i| {
                let element = unsafe { &*elements.add(i) };
                RetainedCounterDefinition {
                    name: unsafe { CssString::from_leaked_raw(element.name) },
                    is_reversed: element.is_reversed,
                    value: unsafe { std::ptr::read(&raw const element.value) },
                }
            })
            .collect();
        Self::from_retained_elements(elements)
    }
}

retained_list_drop!(RetainedCounterDefinitionList);

impl Clone for RetainedCounterDefinitionList {
    fn clone(&self) -> Self {
        Self::from_retained_elements(self.as_slice().to_vec())
    }
}

/// A retained image-set() option: the image, its resolution and an optional Rust-owned type string.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RetainedImageSetOption {
    image: RetainedStyleValueData,
    resolution: RetainedStyleValueData,
    has_type: bool,
    type_string: CssString,
}

/// A Rust-owned array of retained image-set() options.
#[repr(C)]
pub struct RetainedImageSetOptionList {
    pointer: *mut RetainedImageSetOption,
    length: usize,
}

impl RetainedImageSetOptionList {
    unsafe fn from_raw(elements: *const FfiImageSetOption, length: usize) -> Self {
        let elements = (0..length)
            .map(|i| {
                let element = unsafe { &*elements.add(i) };
                RetainedImageSetOption {
                    image: unsafe { std::ptr::read(&raw const element.image) },
                    resolution: unsafe { std::ptr::read(&raw const element.resolution) },
                    has_type: element.has_type,
                    type_string: unsafe { CssString::from_leaked_raw(element.type_string) },
                }
            })
            .collect();
        Self::from_retained_elements(elements)
    }
}

retained_list_drop!(RetainedImageSetOptionList);

impl Clone for RetainedImageSetOptionList {
    fn clone(&self) -> Self {
        Self::from_retained_elements(self.as_slice().to_vec())
    }
}

/// A retained gradient color stop: an optional transition hint, then an optional color,
/// position and second position (each null when absent).
#[repr(C)]
#[derive(Clone, PartialEq)]
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
    pub(crate) fn from_retained_values(
        image: RetainedStyleValueData,
        resolution: RetainedStyleValueData,
        type_string: Option<CssString>,
    ) -> Self {
        Self {
            image,
            resolution,
            has_type: type_string.is_some(),
            type_string: type_string.unwrap_or_else(CssString::none),
        }
    }

    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 2] {
        [&self.image, &self.resolution]
    }

    pub(crate) fn type_string(&self) -> Option<&CssString> {
        self.has_type.then_some(&self.type_string)
    }

    pub(crate) fn with_values(&self, image: RetainedStyleValueData, resolution: RetainedStyleValueData) -> Self {
        Self {
            image,
            resolution,
            has_type: self.has_type,
            type_string: self.type_string.clone(),
        }
    }
}

impl RetainedLinearEasingStop {
    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 2] {
        [&self.output, &self.input]
    }

    pub(crate) fn from_retained_values(output: RetainedStyleValueData, input: RetainedStyleValueData) -> Self {
        Self { output, input }
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
retained_list_as_slice!(RetainedRequestUrlModifierList, RetainedRequestUrlModifier);
retained_list_as_slice!(RetainedGridAreaList, RetainedGridArea);

macro_rules! retained_list_from_vec {
    ($list:ident, $element:ty) => {
        impl $list {
            pub(crate) fn from_retained_elements(elements: Vec<$element>) -> Self {
                let slice = elements.into_boxed_slice();
                let length = slice.len();
                let pointer = Box::into_raw(slice) as *mut $element;
                Self { pointer, length }
            }
        }
    };
}
retained_list_from_vec!(RetainedLinearEasingStopList, RetainedLinearEasingStop);
retained_list_from_vec!(RetainedImageSetOptionList, RetainedImageSetOption);
retained_list_from_vec!(RetainedCounterDefinitionList, RetainedCounterDefinition);
retained_list_from_vec!(RetainedColorStopList, RetainedColorStop);

impl RetainedShapePointList {
    pub(crate) fn from_retained_points(points: Vec<RetainedShapePoint>) -> Self {
        let slice = points.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedShapePoint;
        Self { pointer, length }
    }
}

impl RetainedColorStop {
    pub(crate) fn transition_hint_value(&self) -> Option<&StyleValueData> {
        self.transition_hint.optional_data()
    }

    pub(crate) fn color_value(&self) -> &StyleValueData {
        self.color.data()
    }

    pub(crate) fn position_value(&self) -> Option<&StyleValueData> {
        self.position.optional_data()
    }

    pub(crate) fn second_position_value(&self) -> Option<&StyleValueData> {
        self.second_position.optional_data()
    }

    /// The stop's retained values, absent ones as null retained references.
    pub(crate) fn values(&self) -> [&RetainedStyleValueData; 4] {
        [
            &self.transition_hint,
            &self.color,
            &self.position,
            &self.second_position,
        ]
    }

    pub(crate) fn from_retained_values(
        transition_hint: RetainedStyleValueData,
        color: RetainedStyleValueData,
        position: RetainedStyleValueData,
        second_position: RetainedStyleValueData,
    ) -> Self {
        Self {
            transition_hint,
            color,
            position,
            second_position,
        }
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
#[derive(Clone, PartialEq)]
pub struct RetainedGridArea {
    name: CssString,
    implicit_start_name: CssString,
    implicit_end_name: CssString,
    row_start: usize,
    row_end: usize,
    column_start: usize,
    column_end: usize,
}

impl RetainedGridArea {
    pub(crate) fn new(
        name: CssString,
        implicit_start_name: CssString,
        implicit_end_name: CssString,
        row_start: usize,
        row_end: usize,
        column_start: usize,
        column_end: usize,
    ) -> Self {
        Self {
            name,
            implicit_start_name,
            implicit_end_name,
            row_start,
            row_end,
            column_start,
            column_end,
        }
    }

    pub(crate) fn name(&self) -> &CssString {
        &self.name
    }

    pub(crate) fn implicit_start_name(&self) -> &CssString {
        &self.implicit_start_name
    }

    pub(crate) fn implicit_end_name(&self) -> &CssString {
        &self.implicit_end_name
    }

    pub(crate) fn grid_lines(&self) -> [usize; 4] {
        [self.row_start, self.row_end, self.column_start, self.column_end]
    }

    pub(crate) fn covers_cell(&self, row: usize, column: usize) -> bool {
        row >= self.row_start && row < self.row_end && column >= self.column_start && column < self.column_end
    }
}

/// A Rust-owned array of retained named grid areas.
#[repr(C)]
pub struct RetainedGridAreaList {
    pointer: *mut RetainedGridArea,
    length: usize,
}

impl RetainedGridAreaList {
    pub(crate) fn from_retained_elements(elements: Vec<RetainedGridArea>) -> Self {
        let slice = elements.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice) as *mut RetainedGridArea;
        Self { pointer, length }
    }
}

retained_list_drop!(RetainedGridAreaList);

impl Clone for RetainedGridAreaList {
    fn clone(&self) -> Self {
        Self::from_retained_elements(self.as_slice().to_vec())
    }
}

/// A retained linear() easing stop: the output value and an optional input (null when absent).
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RetainedLinearEasingStop {
    output: RetainedStyleValueData,
    input: RetainedStyleValueData,
}

impl RetainedLinearEasingStop {
    pub(crate) fn output(&self) -> &RetainedStyleValueData {
        &self.output
    }

    pub(crate) fn input(&self) -> &RetainedStyleValueData {
        &self.input
    }
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
    // Opaque at the FFI boundary to avoid a circular C++ definition with RetainedGridTrackEntry.
    pointer: *mut c_void,
    length: usize,
}

/// A retained, Rust-owned grid track list entry (see [`GridTrackEntryInput`] for the kinds).
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RetainedGridTrackEntry {
    pub(crate) kind: GridTrackEntryKind,
    pub(crate) names: CssStringList,
    pub(crate) size_value: RetainedStyleValueData,
    pub(crate) min_value: RetainedStyleValueData,
    pub(crate) max_value: RetainedStyleValueData,
    pub(crate) repeat_type: u8,
    pub(crate) repeat_count: RetainedStyleValueData,
    pub(crate) repeat_is_subgrid: bool,
    pub(crate) repeat_preserve_line_name_sets: bool,
    pub(crate) repeat_entries: RetainedGridTrackEntryList,
}

impl RetainedGridTrackEntryList {
    pub(crate) fn as_slice(&self) -> &[RetainedGridTrackEntry] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer.cast(), self.length) }
    }

    pub(crate) fn from_retained_entries(entries: Vec<RetainedGridTrackEntry>) -> Self {
        let slice = entries.into_boxed_slice();
        let length = slice.len();
        let pointer = Box::into_raw(slice).cast();
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
                    names: unsafe { CssStringList::from_raw(input.names, input.name_count) },
                    size_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.size_value) },
                    min_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.min_value) },
                    max_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.max_value) },
                    repeat_type: input.repeat_type,
                    repeat_count: unsafe { RetainedStyleValueData::from_retained_optional_pointer(input.repeat_count) },
                    repeat_is_subgrid: input.repeat_is_subgrid,
                    repeat_preserve_line_name_sets: input.repeat_preserve_line_name_sets,
                    repeat_entries: unsafe {
                        RetainedGridTrackEntryList::from_raw(input.repeat_entries, input.repeat_entry_count)
                    },
                }
            })
            .collect();
        let length = slice.len();
        let pointer = Box::into_raw(slice).cast();
        Self { pointer, length }
    }
}

impl Clone for RetainedGridTrackEntryList {
    fn clone(&self) -> Self {
        Self::from_retained_entries(self.as_slice().to_vec())
    }
}

impl RetainedGridTrackEntry {
    pub(crate) fn line_names(names: Vec<CssString>) -> Self {
        Self {
            kind: GridTrackEntryKind::LineNames,
            names: CssStringList::from_strings(names),
            size_value: RetainedStyleValueData::none(),
            min_value: RetainedStyleValueData::none(),
            max_value: RetainedStyleValueData::none(),
            repeat_type: 0,
            repeat_count: RetainedStyleValueData::none(),
            repeat_is_subgrid: false,
            repeat_preserve_line_name_sets: false,
            repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
        }
    }

    pub(crate) fn size(value: StyleValueData) -> Self {
        Self {
            kind: GridTrackEntryKind::Size,
            names: CssStringList::from_strings(Vec::new()),
            size_value: RetainedStyleValueData::from_owned(value),
            min_value: RetainedStyleValueData::none(),
            max_value: RetainedStyleValueData::none(),
            repeat_type: 0,
            repeat_count: RetainedStyleValueData::none(),
            repeat_is_subgrid: false,
            repeat_preserve_line_name_sets: false,
            repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
        }
    }

    pub(crate) fn minmax(min: StyleValueData, max: StyleValueData) -> Self {
        Self {
            kind: GridTrackEntryKind::MinMax,
            names: CssStringList::from_strings(Vec::new()),
            size_value: RetainedStyleValueData::none(),
            min_value: RetainedStyleValueData::from_owned(min),
            max_value: RetainedStyleValueData::from_owned(max),
            repeat_type: 0,
            repeat_count: RetainedStyleValueData::none(),
            repeat_is_subgrid: false,
            repeat_preserve_line_name_sets: false,
            repeat_entries: RetainedGridTrackEntryList::from_retained_entries(Vec::new()),
        }
    }

    pub(crate) fn repeat(
        repeat_type: u8,
        repeat_count: Option<StyleValueData>,
        repeat_is_subgrid: bool,
        repeat_preserve_line_name_sets: bool,
        repeat_entries: Vec<Self>,
    ) -> Self {
        Self {
            kind: GridTrackEntryKind::Repeat,
            names: CssStringList::from_strings(Vec::new()),
            size_value: RetainedStyleValueData::none(),
            min_value: RetainedStyleValueData::none(),
            max_value: RetainedStyleValueData::none(),
            repeat_type,
            repeat_count: repeat_count.map_or_else(RetainedStyleValueData::none, RetainedStyleValueData::from_owned),
            repeat_is_subgrid,
            repeat_preserve_line_name_sets,
            repeat_entries: RetainedGridTrackEntryList::from_retained_entries(repeat_entries),
        }
    }

    pub(crate) fn repeat_entries(&self) -> &[RetainedGridTrackEntry] {
        self.repeat_entries.as_slice()
    }
}

impl Drop for RetainedGridTrackEntryList {
    fn drop(&mut self) {
        if !self.pointer.is_null() {
            // SAFETY: The handle owns exactly this boxed slice of initialized entries.
            drop(unsafe {
                Box::from_raw(std::ptr::slice_from_raw_parts_mut(
                    self.pointer.cast::<RetainedGridTrackEntry>(),
                    self.length,
                ))
            });
        }
    }
}

/// A retained polygon point: the x and y style values.
#[repr(C)]
#[derive(Clone, PartialEq)]
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
#[derive(Clone, PartialEq)]
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

    pub(crate) fn from_single_numeric_range(value_type: u8, min: f64, max: f64) -> Self {
        let ranges = vec![RetainedNumericRangeByType { value_type, min, max }].into_boxed_slice();
        let length = ranges.len();
        let pointer = Box::into_raw(ranges) as *mut RetainedNumericRangeByType;
        Self { pointer, length }
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
                        unsafe { std::slice::from_raw_parts(list.pointer.cast::<$element>(), list.length) }
                    }
                };
                as_slice(self) == as_slice(other)
            }
        }
    };
}
pub(crate) use retained_list_partial_eq;

macro_rules! retained_list_send_sync {
    ($list:ty, $element:ty) => {
        // SAFETY: The list owns its boxed slice and only exposes shared access to its elements.
        unsafe impl Send for $list where $element: Send {}
        unsafe impl Sync for $list where $element: Sync {}
    };
}

retained_list_send_sync!(RetainedStyleValueDataList, RetainedStyleValueData);
retained_list_send_sync!(RetainedPropertyIdList, u16);
retained_list_send_sync!(RetainedRequestUrlModifierList, RetainedRequestUrlModifier);
retained_list_send_sync!(RetainedByteList, u8);
retained_list_send_sync!(RetainedCounterDefinitionList, RetainedCounterDefinition);
retained_list_send_sync!(RetainedImageSetOptionList, RetainedImageSetOption);
retained_list_send_sync!(RetainedColorStopList, RetainedColorStop);
retained_list_send_sync!(RetainedGridAreaList, RetainedGridArea);
retained_list_send_sync!(RetainedLinearEasingStopList, RetainedLinearEasingStop);
retained_list_send_sync!(RetainedGridTrackEntryList, RetainedGridTrackEntry);
retained_list_send_sync!(RetainedShapePointList, RetainedShapePoint);
retained_list_send_sync!(RetainedNumericRangeList, RetainedNumericRangeByType);

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
#[derive(Clone, Copy, PartialEq)]
pub struct ColorBase {
    pub(crate) has_color_type: bool,
    pub(crate) color_type: u8,
    pub(crate) color_syntax: u8,
}

/// Fetch context carried by a computed url() image. This is not part of the CSS value's
/// identity, so equality deliberately ignores it.
#[repr(C)]
#[derive(Clone)]
pub struct ImageResourceContext {
    pub(crate) base_url: RetainedString,
    pub(crate) has_base_url: bool,
    pub(crate) has_parent_style_sheet_origin_clean: bool,
    pub(crate) parent_style_sheet_origin_clean: bool,
    pub(crate) should_absolutize_url_for_computed_value: bool,
}

impl PartialEq for ImageResourceContext {
    fn eq(&self, _other: &Self) -> bool {
        true
    }
}

/// The data of a single immutable CSS style value.
///
/// Variant payload fields are read directly by the corresponding C++ StyleValue subclass, so
/// changing a payload changes the C++ accessors reading it.
#[repr(C, u8)]
// NB: Variant payload fields are only read by C++ through the exposed layout.
#[allow(dead_code)]
#[derive(Clone, PartialEq)]
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
        path_string: CssString,
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
    /// A CSS `<string>`, with its animation-name custom-ident classification captured when C++
    /// creates the immutable value so computation does not read the string across FFI.
    String {
        string: CssString,
        is_valid_animation_name_custom_ident: bool,
    },
    /// An unrecognized CSS function, kept as its name and argument value.
    Function {
        name: CssString,
        value: RetainedStyleValueData,
    },
    /// An OpenType tag with its value, from font-feature-settings or font-variation-settings.
    /// The mode is the C++ OpenTypeTaggedStyleValue::Mode, opaque to Rust.
    OpenTypeTagged {
        mode: u8,
        tag: CssString,
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
        anchor_name: CssString,
        has_anchor_size: bool,
        anchor_size: u8,
        fallback_value: RetainedStyleValueData,
    },
    /// anchor(): an optional anchor name, the retained side style value and an optional
    /// retained fallback value.
    Anchor {
        has_anchor_name: bool,
        anchor_name: CssString,
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
        name: CssString,
    },
    /// A counter style reference: either a retained counter style name, or a symbols() function
    /// with its type (the C++ `enum class SymbolsType : u8`, opaque to Rust) and retained
    /// symbol strings.
    CounterStyle {
        is_symbols: bool,
        name: CssString,
        symbols_type: u8,
        symbols: CssStringList,
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
        name: CssString,
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
    /// A url() image. The resource context is snapshotted into computed images so a later C++
    /// wrapper can remain a lazy consumer adapter. Loading state stays on the C++ side.
    Image {
        url: RetainedString,
        url_type: u8,
        url_modifiers: RetainedRequestUrlModifierList,
        resource_context: ImageResourceContext,
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
        name: CssString,
        implicit_start_name: CssString,
        implicit_end_name: CssString,
    },
    /// counter() or counters(). The function is the C++ CounterFunction enum, opaque to Rust;
    /// the join string is empty for counter().
    Counter {
        function: u8,
        counter_name: CssString,
        counter_style: RetainedStyleValueData,
        join_string: CssString,
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
        name: CssString,
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
        schemes: CssStringList,
        scheme_codes: RetainedByteList,
        only: bool,
    },
    /// An unresolved value containing arbitrary substitution functions, kept as its retained
    /// source text, an optional normalized comparison text (empty when absent), the presence
    /// flags of each substitution function, the attr-taint flag, and an optional parsed value
    /// cached for an attr()-tainted registered custom property.
    Unresolved {
        components: RetainedComponentValueList,
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
        format: CssString,
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
    CustomIdent { custom_ident: CssString },
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

/// One entry in a computed font-family list. A generic family carries its Keyword code;
/// a named family borrows the Rust string retained by the style value.
#[repr(C)]
pub struct FfiComputedFontFamilyEntry {
    pub kind: u8,
    pub keyword: u16,
    pub string: *const c_void,
}

pub const COMPUTED_FONT_FAMILY_GENERIC: u8 = 0;
pub const COMPUTED_FONT_FAMILY_CUSTOM_IDENT: u8 = 1;
pub const COMPUTED_FONT_FAMILY_STRING: u8 = 2;

/// Copies the entries of a computed font-family value without constructing C++ StyleValues.
///
/// # Safety
/// `value` must point at live font-family StyleValueData. When `output` is non-null, it must
/// point at space for at least `capacity` entries.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_copy_computed_font_families(
    value: *const c_void,
    output: *mut FfiComputedFontFamilyEntry,
    capacity: usize,
) -> usize {
    let StyleValueData::ValueList { values, .. } = (unsafe { &*(value as *const StyleValueData) }) else {
        unreachable!("computed font-family must be a value list");
    };
    if output.is_null() {
        return values.as_slice().len();
    }
    assert!(capacity >= values.as_slice().len());
    for (index, value) in values.as_slice().iter().enumerate() {
        let entry = match value.data() {
            StyleValueData::Keyword { keyword } => FfiComputedFontFamilyEntry {
                kind: COMPUTED_FONT_FAMILY_GENERIC,
                keyword: *keyword,
                string: std::ptr::null(),
            },
            StyleValueData::CustomIdent { custom_ident } => FfiComputedFontFamilyEntry {
                kind: COMPUTED_FONT_FAMILY_CUSTOM_IDENT,
                keyword: 0,
                string: custom_ident.as_ptr(),
            },
            StyleValueData::String { string, .. } => FfiComputedFontFamilyEntry {
                kind: COMPUTED_FONT_FAMILY_STRING,
                keyword: 0,
                string: string.as_ptr(),
            },
            _ => unreachable!("computed font-family entry must be a generic or named family"),
        };
        unsafe { output.add(index).write(entry) };
    }
    values.as_slice().len()
}

/// Reads the primitive payload of a computed absolute length.
///
/// # Safety
/// `value` must point at live Length StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_computed_length_value(value: *const c_void) -> f64 {
    match unsafe { &*(value as *const StyleValueData) } {
        StyleValueData::Length { value, .. } => *value,
        _ => unreachable!("computed value must be a length"),
    }
}

/// Reads the primitive payload of a computed number.
///
/// # Safety
/// `value` must point at live Number StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_computed_number(value: *const c_void) -> f64 {
    match unsafe { &*(value as *const StyleValueData) } {
        StyleValueData::Number { value } => *value,
        _ => unreachable!("computed value must be a number"),
    }
}

/// Reads the primitive payload of a computed percentage.
///
/// # Safety
/// `value` must point at live Percentage StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_computed_percentage(value: *const c_void) -> f64 {
    match unsafe { &*(value as *const StyleValueData) } {
        StyleValueData::Percentage { value } => *value,
        _ => unreachable!("computed value must be a percentage"),
    }
}

impl StyleValueData {
    pub(crate) fn unresolved_authored_source(&self) -> Option<TokenizerInput<'_>> {
        let Self::Unresolved { source_text, .. } = self else {
            return None;
        };
        Some(source_text.as_units())
    }

    pub(crate) fn unresolved_token_source(&self) -> Option<TokenizerInput<'_>> {
        let Self::Unresolved {
            source_text,
            value_comparison_text,
            ..
        } = self
        else {
            return None;
        };
        if value_comparison_text.as_units().is_empty() {
            Some(source_text.as_units())
        } else {
            Some(value_comparison_text.as_units())
        }
    }
}

impl StyleValueData {
    /// Whether this is an `<image>` value: a URL image, an `image-set()` or a gradient.
    pub(crate) fn is_image(&self) -> bool {
        matches!(
            self,
            Self::Image { .. }
                | Self::ImageSet { .. }
                | Self::LinearGradient { .. }
                | Self::RadialGradient { .. }
                | Self::ConicGradient { .. }
        )
    }
}

impl StyleValueData {
    /// A content hash consistent with `PartialEq`: equal values always produce equal hashes.
    ///
    /// Consistency holds by construction because the hash only ever omits fields, and derived
    /// equality requires every field to be equal, so any hashed subset agrees on equal values.
    /// Opaque payloads (calc trees, geometry and stop lists) contribute nothing beyond the
    /// variant discriminant; deep equality settles whatever shares a bucket. Floats hash by bit
    /// pattern with negative zero normalized to zero; NaN never equals anything, so its hash is
    /// unconstrained.
    pub(crate) fn content_hash(&self) -> u64 {
        use std::hash::Hasher;
        let mut hasher = crate::css::style::fast_hash::fast_hasher();
        self.write_content_hash(&mut hasher);
        hasher.finish()
    }

    fn write_content_hash(&self, hasher: &mut crate::css::style::fast_hash::FastHasher) {
        use std::hash::Hash;
        use std::hash::Hasher;

        fn write_f64(hasher: &mut crate::css::style::fast_hash::FastHasher, value: f64) {
            hasher.write_u64((if value == 0.0 { 0.0_f64 } else { value }).to_bits());
        }
        fn write_bool(hasher: &mut crate::css::style::fast_hash::FastHasher, value: bool) {
            hasher.write_u8(value as u8);
        }
        fn hash_string_units(hasher: &mut crate::css::style::fast_hash::FastHasher, units: TokenizerInput<'_>) {
            match units {
                TokenizerInput::Ascii(units) => {
                    for &unit in units {
                        hasher.write_u16(u16::from(unit));
                    }
                }
                TokenizerInput::Utf16(units) => {
                    for &unit in units {
                        hasher.write_u16(unit);
                    }
                }
            }
        }
        fn write_value(hasher: &mut crate::css::style::fast_hash::FastHasher, value: &RetainedStyleValueData) {
            match value.optional_data() {
                None => hasher.write_u8(0xA5),
                Some(data) => data.write_content_hash(hasher),
            }
        }
        fn write_values(hasher: &mut crate::css::style::fast_hash::FastHasher, values: &RetainedStyleValueDataList) {
            hasher.write_usize(values.as_slice().len());
            for value in values.as_slice() {
                write_value(hasher, value);
            }
        }
        fn write_fly(hasher: &mut crate::css::style::fast_hash::FastHasher, string: &CssString) {
            string.units().hash(hasher);
        }
        fn write_color_base(hasher: &mut crate::css::style::fast_hash::FastHasher, base: &ColorBase) {
            write_bool(hasher, base.has_color_type);
            hasher.write_u8(base.color_type);
            hasher.write_u8(base.color_syntax);
        }

        std::mem::discriminant(self).hash(hasher);
        match self {
            Self::Keyword { keyword } => hasher.write_u16(*keyword),
            Self::Number { value } => write_f64(hasher, *value),
            Self::Integer { value } => hasher.write_u32(*value as u32),
            Self::Angle { value, unit }
            | Self::Flex { value, unit }
            | Self::Frequency { value, unit }
            | Self::Length { value, unit }
            | Self::Resolution { value, unit }
            | Self::Time { value, unit } => {
                write_f64(hasher, *value);
                hasher.write_u8(*unit);
            }
            Self::Percentage { value } => write_f64(hasher, *value),
            Self::BasicShape {
                kind,
                v0,
                v1,
                v2,
                v3,
                v4,
                fill_rule,
                points: _,
                path_string,
            } => {
                hasher.write_u8(*kind);
                write_value(hasher, v0);
                write_value(hasher, v1);
                write_value(hasher, v2);
                write_value(hasher, v3);
                write_value(hasher, v4);
                hasher.write_u8(*fill_rule);
                write_fly(hasher, path_string);
            }
            Self::Calculated {
                rust_calculation: _,
                resolve_as_is_number,
                resolve_as_base,
                resolved_type: _,
                has_percentages_resolve_as,
                percentages_resolve_as,
                resolve_numbers_as_integers,
                accepted_ranges: _,
            } => {
                write_bool(hasher, *resolve_as_is_number);
                hasher.write_u8(*resolve_as_base);
                write_bool(hasher, *has_percentages_resolve_as);
                hasher.write_u8(*percentages_resolve_as);
                write_bool(hasher, *resolve_numbers_as_integers);
            }
            Self::Ratio { numerator, denominator } => {
                write_value(hasher, numerator);
                write_value(hasher, denominator);
            }
            Self::UnicodeRange {
                min_code_point,
                max_code_point,
            } => {
                hasher.write_u32(*min_code_point);
                hasher.write_u32(*max_code_point);
            }
            Self::OpacityValue { value } => write_value(hasher, value),
            Self::Edge { has_edge, edge, offset } => {
                write_bool(hasher, *has_edge);
                hasher.write_u8(*edge);
                write_value(hasher, offset);
            }
            Self::GuaranteedInvalid | Self::EmptyOptional => {}
            Self::GridAutoFlow { row, dense } => {
                write_bool(hasher, *row);
                write_bool(hasher, *dense);
            }
            Self::TextUnderlinePosition { horizontal, vertical } => {
                hasher.write_u8(*horizontal);
                hasher.write_u8(*vertical);
            }
            Self::ContrastColor { color_base, color } => {
                write_color_base(hasher, color_base);
                write_value(hasher, color);
            }
            Self::Superellipse { parameter } => write_value(hasher, parameter),
            Self::PendingSubstitution {
                original_shorthand_value,
            } => write_value(hasher, original_shorthand_value),
            Self::ScrollbarColor {
                thumb_color,
                track_color,
            } => {
                write_value(hasher, thumb_color);
                write_value(hasher, track_color);
            }
            Self::Rect {
                top,
                right,
                bottom,
                left,
            } => {
                write_value(hasher, top);
                write_value(hasher, right);
                write_value(hasher, bottom);
                write_value(hasher, left);
            }
            Self::String { string, .. } => write_fly(hasher, string),
            Self::Function { name, value } => {
                write_fly(hasher, name);
                write_value(hasher, value);
            }
            Self::OpenTypeTagged {
                mode,
                tag,
                packed_tag,
                value,
            } => {
                hasher.write_u8(*mode);
                write_fly(hasher, tag);
                hasher.write_u32(*packed_tag);
                write_value(hasher, value);
            }
            Self::FontStyle {
                font_style,
                angle_value,
            } => {
                hasher.write_u8(*font_style);
                write_value(hasher, angle_value);
            }
            Self::TextIndent {
                length_percentage,
                hanging,
                each_line,
            } => {
                write_value(hasher, length_percentage);
                write_bool(hasher, *hanging);
                write_bool(hasher, *each_line);
            }
            Self::OverflowClipMargin {
                has_visual_box,
                visual_box,
                offset,
            } => {
                write_bool(hasher, *has_visual_box);
                hasher.write_u8(*visual_box);
                write_value(hasher, offset);
            }
            Self::TreeCountingFunction {
                function,
                computed_type,
            } => {
                hasher.write_u8(*function);
                hasher.write_u8(*computed_type);
            }
            Self::BackgroundSize { size_x, size_y } => {
                write_value(hasher, size_x);
                write_value(hasher, size_y);
            }
            Self::RepeatStyle { repeat_x, repeat_y } => {
                hasher.write_u8(*repeat_x);
                hasher.write_u8(*repeat_y);
            }
            Self::BorderImageSlice {
                top,
                right,
                bottom,
                left,
                fill,
            } => {
                write_value(hasher, top);
                write_value(hasher, right);
                write_value(hasher, bottom);
                write_value(hasher, left);
                write_bool(hasher, *fill);
            }
            Self::AnchorSize {
                has_anchor_name,
                anchor_name,
                has_anchor_size,
                anchor_size,
                fallback_value,
            } => {
                write_bool(hasher, *has_anchor_name);
                write_fly(hasher, anchor_name);
                write_bool(hasher, *has_anchor_size);
                hasher.write_u8(*anchor_size);
                write_value(hasher, fallback_value);
            }
            Self::Anchor {
                has_anchor_name,
                anchor_name,
                anchor_side,
                fallback_value,
            } => {
                write_bool(hasher, *has_anchor_name);
                write_fly(hasher, anchor_name);
                write_value(hasher, anchor_side);
                write_value(hasher, fallback_value);
            }
            Self::Position { edge_x, edge_y } => {
                write_value(hasher, edge_x);
                write_value(hasher, edge_y);
            }
            Self::Shadow {
                shadow_type,
                color,
                offset_x,
                offset_y,
                blur_radius,
                spread_distance,
                placement,
            } => {
                hasher.write_u8(*shadow_type);
                write_value(hasher, color);
                write_value(hasher, offset_x);
                write_value(hasher, offset_y);
                write_value(hasher, blur_radius);
                write_value(hasher, spread_distance);
                hasher.write_u8(*placement);
            }
            Self::Content { content, alt_text } => {
                write_value(hasher, content);
                write_value(hasher, alt_text);
            }
            Self::CounterStyleSystem {
                kind,
                system,
                first_symbol,
                name,
            } => {
                hasher.write_u8(*kind);
                hasher.write_u8(*system);
                write_value(hasher, first_symbol);
                write_fly(hasher, name);
            }
            Self::CounterStyle {
                is_symbols,
                name,
                symbols_type,
                symbols,
            } => {
                write_bool(hasher, *is_symbols);
                write_fly(hasher, name);
                hasher.write_u8(*symbols_type);
                for string in symbols.as_slice() {
                    write_fly(hasher, string);
                }
            }
            Self::ColorFunction {
                color_base,
                channel_0,
                channel_1,
                channel_2,
                alpha,
                has_name,
                name,
                origin_color,
            } => {
                write_color_base(hasher, color_base);
                write_value(hasher, channel_0);
                write_value(hasher, channel_1);
                write_value(hasher, channel_2);
                write_value(hasher, alpha);
                write_bool(hasher, *has_name);
                write_fly(hasher, name);
                write_value(hasher, origin_color);
            }
            Self::ColorMix {
                color_base,
                color_interpolation_method,
                first_color,
                first_percentage,
                second_color,
                second_percentage,
            } => {
                write_color_base(hasher, color_base);
                write_value(hasher, color_interpolation_method);
                write_value(hasher, first_color);
                write_value(hasher, first_percentage);
                write_value(hasher, second_color);
                write_value(hasher, second_percentage);
            }
            Self::LinearGradient {
                has_direction_value,
                direction_value,
                side_or_corner,
                color_stop_list: _,
                gradient_type,
                repeating,
                color_interpolation_method,
                color_syntax,
            } => {
                write_bool(hasher, *has_direction_value);
                write_value(hasher, direction_value);
                hasher.write_u8(*side_or_corner);
                hasher.write_u8(*gradient_type);
                write_bool(hasher, *repeating);
                write_value(hasher, color_interpolation_method);
                hasher.write_u8(*color_syntax);
            }
            Self::ConicGradient {
                from_angle,
                position,
                color_stop_list: _,
                repeating,
                color_interpolation_method,
                color_syntax,
            } => {
                write_value(hasher, from_angle);
                write_value(hasher, position);
                write_bool(hasher, *repeating);
                write_value(hasher, color_interpolation_method);
                hasher.write_u8(*color_syntax);
            }
            Self::RadialGradient {
                ending_shape,
                size,
                position,
                color_stop_list: _,
                repeating,
                color_interpolation_method,
                color_syntax,
            } => {
                hasher.write_u8(*ending_shape);
                write_value(hasher, size);
                write_value(hasher, position);
                write_bool(hasher, *repeating);
                write_value(hasher, color_interpolation_method);
                hasher.write_u8(*color_syntax);
            }
            Self::Image { url, url_type, .. } => {
                url.hash(hasher);
                hasher.write_u8(*url_type);
            }
            Self::ImageSet { options: _ } => {}
            Self::Easing {
                kind,
                linear_stops: _,
                x1,
                y1,
                x2,
                y2,
                number_of_intervals,
                step_position,
            } => {
                hasher.write_u8(*kind);
                write_value(hasher, x1);
                write_value(hasher, y1);
                write_value(hasher, x2);
                write_value(hasher, y2);
                write_value(hasher, number_of_intervals);
                hasher.write_u8(*step_position);
            }
            Self::Cursor { image, x, y } => {
                write_value(hasher, image);
                write_value(hasher, x);
                write_value(hasher, y);
            }
            Self::GridTrackSizeList {
                is_subgrid,
                preserve_line_name_sets,
                entries: _,
            } => {
                write_bool(hasher, *is_subgrid);
                write_bool(hasher, *preserve_line_name_sets);
            }
            Self::GridTemplateArea {
                grid_areas: _,
                row_count,
                column_count,
            } => {
                hasher.write_usize(*row_count);
                hasher.write_usize(*column_count);
            }
            Self::CounterDefinitions { counter_definitions: _ } => {}
            Self::GridTrackPlacement {
                kind,
                value,
                has_name,
                name,
                implicit_start_name: _,
                implicit_end_name: _,
            } => {
                hasher.write_u8(*kind);
                write_value(hasher, value);
                write_bool(hasher, *has_name);
                write_fly(hasher, name);
            }
            Self::Counter {
                function,
                counter_name,
                counter_style,
                join_string,
            } => {
                hasher.write_u8(*function);
                write_fly(hasher, counter_name);
                write_value(hasher, counter_style);
                write_fly(hasher, join_string);
            }
            Self::LightDark {
                color_base,
                light,
                dark,
            } => {
                write_color_base(hasher, color_base);
                write_value(hasher, light);
                write_value(hasher, dark);
            }
            Self::RandomValueSharing {
                fixed_value,
                is_auto,
                has_name,
                name,
                element_shared,
            } => {
                write_value(hasher, fixed_value);
                write_bool(hasher, *is_auto);
                write_bool(hasher, *has_name);
                write_fly(hasher, name);
                write_bool(hasher, *element_shared);
            }
            Self::ScrollbarGutter { value } => hasher.write_u8(*value),
            Self::ColorInterpolationMethod {
                is_polar,
                color_space,
                hue_interpolation_method,
            } => {
                write_bool(hasher, *is_polar);
                hasher.write_u8(*color_space);
                hasher.write_u8(*hue_interpolation_method);
            }
            Self::ValueList {
                values,
                separator,
                collapsible,
            } => {
                write_values(hasher, values);
                hasher.write_u8(*separator);
                write_bool(hasher, *collapsible);
            }
            Self::Tuple { values } => write_values(hasher, values),
            Self::Display { raw } => hasher.write_u32(*raw),
            Self::ColorScheme {
                schemes,
                scheme_codes,
                only,
            } => {
                for string in schemes.as_slice() {
                    write_fly(hasher, string);
                }
                hasher.write(scheme_codes.as_slice());
                write_bool(hasher, *only);
            }
            Self::Unresolved {
                components: _,
                source_text,
                value_comparison_text,
                presence_attr,
                presence_dashed_function,
                presence_env,
                presence_if,
                presence_inherit,
                presence_var,
                contains_attr_tainted_values,
                parsed_value: _,
            } => {
                hash_string_units(hasher, source_text.as_units());
                hash_string_units(hasher, value_comparison_text.as_units());
                write_bool(hasher, *presence_attr);
                write_bool(hasher, *presence_dashed_function);
                write_bool(hasher, *presence_env);
                write_bool(hasher, *presence_if);
                write_bool(hasher, *presence_inherit);
                write_bool(hasher, *presence_var);
                write_bool(hasher, *contains_attr_tainted_values);
            }
            Self::Url {
                url,
                url_type,
                modifiers: _,
            } => {
                url.hash(hasher);
                hasher.write_u8(*url_type);
            }
            Self::FontSource {
                is_local,
                local_name,
                url,
                url_type,
                url_modifiers: _,
                has_format,
                format,
                tech,
            } => {
                write_bool(hasher, *is_local);
                write_value(hasher, local_name);
                url.hash(hasher);
                hasher.write_u8(*url_type);
                write_bool(hasher, *has_format);
                write_fly(hasher, format);
                hasher.write(tech.as_slice());
            }
            Self::RadialSize {
                component_count,
                is_extent_0,
                extent_0,
                value_0,
                is_extent_1,
                extent_1,
                value_1,
            } => {
                hasher.write_u8(*component_count);
                write_bool(hasher, *is_extent_0);
                hasher.write_u8(*extent_0);
                write_value(hasher, value_0);
                write_bool(hasher, *is_extent_1);
                hasher.write_u8(*extent_1);
                write_value(hasher, value_1);
            }
            Self::Transformation {
                property,
                transform_function,
                values,
            } => {
                hasher.write_u16(*property);
                hasher.write_u8(*transform_function);
                write_values(hasher, values);
            }
            Self::Shorthand {
                shorthand_property,
                sub_properties,
                values,
            } => {
                hasher.write_u16(*shorthand_property);
                for id in sub_properties.as_slice() {
                    hasher.write_u16(*id);
                }
                write_values(hasher, values);
            }
            Self::CustomIdent { custom_ident } => write_fly(hasher, custom_ident),
            Self::BorderRadiusRect {
                top_left,
                top_right,
                bottom_right,
                bottom_left,
            } => {
                write_value(hasher, top_left);
                write_value(hasher, top_right);
                write_value(hasher, bottom_right);
                write_value(hasher, bottom_left);
            }
            Self::BorderRadius {
                is_elliptical,
                horizontal_radius,
                vertical_radius,
            } => {
                write_bool(hasher, *is_elliptical);
                write_value(hasher, horizontal_radius);
                write_value(hasher, vertical_radius);
            }
            Self::Filter {
                kind,
                color_operation,
                value,
            } => {
                hasher.write_u8(*kind);
                hasher.write_u8(*color_operation);
                write_value(hasher, value);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_keyword(keyword: u16) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Keyword { keyword }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_number(value: f64) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Number { value }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_integer(value: i32) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Integer { value }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_angle(value: f64, unit: u8) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Angle { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_flex(value: f64, unit: u8) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Flex { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_frequency(value: f64, unit: u8) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Frequency { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_length(value: f64, unit: u8) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Length { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_percentage(value: f64) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Percentage { value }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_resolution(value: f64, unit: u8) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Resolution { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_time(value: f64, unit: u8) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Time { value, unit }))
}

/// Takes ownership of one strong reference to each of the numerator and denominator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_ratio(
    numerator: *const StyleValueData,
    denominator: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Ratio {
        numerator: unsafe { RetainedStyleValueData::from_retained_pointer(numerator) },
        denominator: unsafe { RetainedStyleValueData::from_retained_pointer(denominator) },
    }))
}

/// Takes ownership of one strong reference to the value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_opacity_value(value: *const StyleValueData) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::OpacityValue {
        value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
    }))
}

/// Takes ownership of one strong reference to the offset data if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_edge(
    has_edge: bool,
    edge: u8,
    offset: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Edge {
        has_edge,
        edge,
        offset: unsafe { RetainedStyleValueData::from_retained_optional_pointer(offset) },
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_guaranteed_invalid() -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::GuaranteedInvalid))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_empty_optional() -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::EmptyOptional))
}

/// Takes ownership of one strong reference to the color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_contrast_color(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    color: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::ContrastColor {
        color_base: ColorBase {
            has_color_type,
            color_type,
            color_syntax,
        },
        color: unsafe { RetainedStyleValueData::from_retained_pointer(color) },
    }))
}

/// Takes ownership of one strong reference to the parameter data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_superellipse(
    parameter: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Superellipse {
        parameter: unsafe { RetainedStyleValueData::from_retained_pointer(parameter) },
    }))
}

/// Takes ownership of one strong reference to the original shorthand value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_pending_substitution(
    original_shorthand_value: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::PendingSubstitution {
        original_shorthand_value: unsafe { RetainedStyleValueData::from_retained_pointer(original_shorthand_value) },
    }))
}

/// Takes ownership of one strong reference to each color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_scrollbar_color(
    thumb_color: *const StyleValueData,
    track_color: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::ScrollbarColor {
        thumb_color: unsafe { RetainedStyleValueData::from_retained_pointer(thumb_color) },
        track_color: unsafe { RetainedStyleValueData::from_retained_pointer(track_color) },
    }))
}

/// Takes ownership of one strong reference to each edge data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_rect(
    top: *const StyleValueData,
    right: *const StyleValueData,
    bottom: *const StyleValueData,
    left: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Rect {
        top: unsafe { RetainedStyleValueData::from_retained_pointer(top) },
        right: unsafe { RetainedStyleValueData::from_retained_pointer(right) },
        bottom: unsafe { RetainedStyleValueData::from_retained_pointer(bottom) },
        left: unsafe { RetainedStyleValueData::from_retained_pointer(left) },
    }))
}

/// Takes ownership of one strong reference to the filter's value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_filter(
    kind: u8,
    color_operation: u8,
    value: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Filter {
        kind,
        color_operation,
        value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
    }))
}

/// Takes ownership of one strong reference to each radius data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius(
    is_elliptical: bool,
    horizontal_radius: *const StyleValueData,
    vertical_radius: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::BorderRadius {
        is_elliptical,
        horizontal_radius: unsafe { RetainedStyleValueData::from_retained_pointer(horizontal_radius) },
        vertical_radius: unsafe { RetainedStyleValueData::from_retained_pointer(vertical_radius) },
    }))
}

/// Takes ownership of one strong reference to each corner radius data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius_rect(
    top_left: *const StyleValueData,
    top_right: *const StyleValueData,
    bottom_right: *const StyleValueData,
    bottom_left: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::BorderRadiusRect {
        top_left: unsafe { RetainedStyleValueData::from_retained_pointer(top_left) },
        top_right: unsafe { RetainedStyleValueData::from_retained_pointer(top_right) },
        bottom_right: unsafe { RetainedStyleValueData::from_retained_pointer(bottom_right) },
        bottom_left: unsafe { RetainedStyleValueData::from_retained_pointer(bottom_left) },
    }))
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_string(
    string: usize,
    is_valid_animation_name_custom_ident: bool,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::String {
        string: unsafe { CssString::from_leaked_raw(string) },
        is_valid_animation_name_custom_ident,
    }))
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_custom_ident(custom_ident: usize) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::CustomIdent {
        custom_ident: unsafe { CssString::from_leaked_raw(custom_ident) },
    }))
}

/// Takes ownership of one leaked reference to the name and one strong reference to the value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_function(
    name: usize,
    value: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Function {
        name: unsafe { CssString::from_leaked_raw(name) },
        value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
    }))
}

/// Takes ownership of one leaked reference to the tag and one strong reference to the value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_open_type_tagged(
    mode: u8,
    tag: usize,
    packed_tag: u32,
    value: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::OpenTypeTagged {
        mode,
        tag: unsafe { CssString::from_leaked_raw(tag) },
        packed_tag,
        value: unsafe { RetainedStyleValueData::from_retained_pointer(value) },
    }))
}

/// Takes ownership of one strong reference to the angle value data if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_font_style(
    font_style: u8,
    angle_value: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::FontStyle {
        font_style,
        angle_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(angle_value) },
    }))
}

/// Takes ownership of one strong reference to the length-percentage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_text_indent(
    length_percentage: *const StyleValueData,
    hanging: bool,
    each_line: bool,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::TextIndent {
        length_percentage: unsafe { RetainedStyleValueData::from_retained_pointer(length_percentage) },
        hanging,
        each_line,
    }))
}

/// Takes ownership of one strong reference to the offset data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_overflow_clip_margin(
    has_visual_box: bool,
    visual_box: u8,
    offset: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::OverflowClipMargin {
        has_visual_box,
        visual_box,
        offset: unsafe { RetainedStyleValueData::from_retained_pointer(offset) },
    }))
}

/// Takes ownership of one strong reference to each size.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_background_size(
    size_x: *const StyleValueData,
    size_y: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::BackgroundSize {
        size_x: unsafe { RetainedStyleValueData::from_retained_pointer(size_x) },
        size_y: unsafe { RetainedStyleValueData::from_retained_pointer(size_y) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::BorderImageSlice {
        top: unsafe { RetainedStyleValueData::from_retained_pointer(top) },
        right: unsafe { RetainedStyleValueData::from_retained_pointer(right) },
        bottom: unsafe { RetainedStyleValueData::from_retained_pointer(bottom) },
        left: unsafe { RetainedStyleValueData::from_retained_pointer(left) },
        fill,
    }))
}

/// Takes ownership of one strong reference to each edge data allocation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_position(
    edge_x: *const StyleValueData,
    edge_y: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Position {
        edge_x: unsafe { RetainedStyleValueData::from_retained_pointer(edge_x) },
        edge_y: unsafe { RetainedStyleValueData::from_retained_pointer(edge_y) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::Shadow {
        shadow_type,
        color: unsafe { RetainedStyleValueData::from_retained_optional_pointer(color) },
        offset_x: unsafe { RetainedStyleValueData::from_retained_pointer(offset_x) },
        offset_y: unsafe { RetainedStyleValueData::from_retained_pointer(offset_y) },
        blur_radius: unsafe { RetainedStyleValueData::from_retained_optional_pointer(blur_radius) },
        spread_distance: unsafe { RetainedStyleValueData::from_retained_optional_pointer(spread_distance) },
        placement,
    }))
}

/// Takes ownership of one strong reference to the content list and, when non-null, the
/// alt-text list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_content(
    content: *const StyleValueData,
    alt_text: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Content {
        content: unsafe { RetainedStyleValueData::from_retained_pointer(content) },
        alt_text: unsafe { RetainedStyleValueData::from_retained_optional_pointer(alt_text) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::Counter {
        function,
        counter_name: unsafe { CssString::from_leaked_raw(counter_name) },
        counter_style: unsafe { RetainedStyleValueData::from_retained_pointer(counter_style) },
        join_string: unsafe { CssString::from_leaked_raw(join_string) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::LightDark {
        color_base: ColorBase {
            has_color_type,
            color_type,
            color_syntax,
        },
        light: unsafe { RetainedStyleValueData::from_retained_pointer(light) },
        dark: unsafe { RetainedStyleValueData::from_retained_pointer(dark) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::RandomValueSharing {
        fixed_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(fixed_value) },
        is_auto,
        has_name,
        name: unsafe { CssString::from_leaked_raw(name) },
        element_shared,
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_color_interpolation_method(
    is_polar: bool,
    color_space: u8,
    hue_interpolation_method: u8,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::ColorInterpolationMethod {
        is_polar,
        color_space,
        hue_interpolation_method,
    }))
}

/// Takes ownership of one strong reference to each of the `length` values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_value_list(
    values: *const *const StyleValueData,
    length: usize,
    separator: u8,
    collapsible: bool,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::ValueList {
        values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, length) },
        separator,
        collapsible,
    }))
}

/// Takes ownership of one strong reference to each non-null value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_tuple(
    values: *const *const StyleValueData,
    length: usize,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Tuple {
        values: unsafe { RetainedStyleValueDataList::from_retained_optional_pointers(values, length) },
    }))
}

/// Takes ownership of one strong reference to each value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_transformation(
    property: u16,
    transform_function: u8,
    values: *const *const StyleValueData,
    length: usize,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Transformation {
        property,
        transform_function,
        values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, length) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::Shorthand {
        shorthand_property,
        sub_properties: unsafe { RetainedPropertyIdList::from_raw(sub_properties, sub_property_count) },
        values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, value_count) },
    }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_display(raw: u32) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Display { raw }))
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
    Arc::into_raw(Arc::new(StyleValueData::RadialSize {
        component_count,
        is_extent_0,
        extent_0,
        value_0: unsafe { RetainedStyleValueData::from_retained_optional_pointer(value_0) },
        is_extent_1,
        extent_1,
        value_1: unsafe { RetainedStyleValueData::from_retained_optional_pointer(value_1) },
    }))
}

/// Creates an unresolved value from borrowed token source. Rust derives the source and comparison
/// strings according to the requested SourceTextMode and retains the component tree.
///
/// Takes ownership of one strong reference to `parsed_value` when it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_unresolved_from_source(
    token_source_ascii_units: *const u8,
    token_source_code_units: *const u16,
    token_source_length: usize,
    has_original_source_text: bool,
    original_source_text_ascii_units: *const u8,
    original_source_text_code_units: *const u16,
    original_source_text_length: usize,
    source_text_mode: u8,
    presence_attr: bool,
    presence_dashed_function: bool,
    presence_env: bool,
    presence_if: bool,
    presence_inherit: bool,
    presence_var: bool,
    contains_attr_tainted_values: bool,
    parsed_value: *const StyleValueData,
) -> *const StyleValueData {
    let token_source = unsafe {
        TokenizerInput::from_raw_parts(token_source_ascii_units, token_source_code_units, token_source_length)
    }
    .unwrap_or_default();
    let components = crate::css::parser::component_value::consume_a_list_of_component_values(
        crate::css::css_tokenizer::tokenize_for_parser(token_source),
    )
    .unwrap_or_default();
    let normalized = crate::css::serialize::serialize_component_values_to_utf16(
        &components,
        crate::css::parser::component_value::ComponentSerializationMode::Normalized,
    );
    let (source_text, value_comparison_text) = if has_original_source_text {
        let original_source_text = unsafe {
            TokenizerInput::from_raw_parts(
                original_source_text_ascii_units,
                original_source_text_code_units,
                original_source_text_length,
            )
        }
        .unwrap_or_default();
        let mut source_text = Vec::with_capacity(original_source_text.len());
        original_source_text.append_to(&mut source_text);
        (
            crate::css::serialize::trim_ascii_whitespace(&source_text).to_vec(),
            crate::css::serialize::trim_ascii_whitespace(&normalized).to_vec(),
        )
    } else {
        let source_text = match source_text_mode {
            0 | 1 => crate::css::serialize::serialize_component_values_to_utf16(
                &components,
                crate::css::parser::component_value::ComponentSerializationMode::PreserveNumericSource,
            ),
            2 => crate::css::serialize::original_component_values_source(&components)
                .unwrap_or_else(|| normalized.clone()),
            _ => unreachable!("unknown unresolved source text mode"),
        };
        let source_text = if source_text_mode == 0 {
            crate::css::serialize::trim_ascii_whitespace(&source_text)
        } else if source_text_mode == 1 {
            crate::css::serialize::trim_ascii_whitespace_start(&source_text)
        } else {
            &source_text
        };
        (source_text.to_vec(), Vec::new())
    };
    let component_source = if value_comparison_text.is_empty() {
        &source_text
    } else {
        &value_comparison_text
    };
    Arc::into_raw(Arc::new(StyleValueData::Unresolved {
        components: RetainedComponentValueList::from_source(component_source),
        source_text: RetainedReadableString::from_utf16(&source_text),
        value_comparison_text: RetainedReadableString::from_utf16(&value_comparison_text),
        presence_attr,
        presence_dashed_function,
        presence_env,
        presence_if,
        presence_inherit,
        presence_var,
        contains_attr_tainted_values,
        parsed_value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(parsed_value) },
    }))
}

/// Takes ownership of the definitions' retained strings and values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_definitions(
    definitions: *const FfiCounterDefinition,
    length: usize,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::CounterDefinitions {
        counter_definitions: unsafe { RetainedCounterDefinitionList::from_raw(definitions, length) },
    }))
}

/// Takes ownership of one strong reference to the value and one leaked reference to the name
/// when they are present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_track_placement(
    kind: u8,
    value: *const StyleValueData,
    has_name: bool,
    name: usize,
    implicit_start_name: usize,
    implicit_end_name: usize,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::GridTrackPlacement {
        kind,
        value: unsafe { RetainedStyleValueData::from_retained_optional_pointer(value) },
        has_name,
        name: unsafe { CssString::from_leaked_raw(name) },
        implicit_start_name: unsafe { CssString::from_leaked_raw(implicit_start_name) },
        implicit_end_name: unsafe { CssString::from_leaked_raw(implicit_end_name) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::CounterStyleSystem {
        kind,
        system,
        first_symbol: unsafe { RetainedStyleValueData::from_retained_optional_pointer(first_symbol) },
        name: unsafe { CssString::from_leaked_raw(name) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::CounterStyle {
        is_symbols,
        name: unsafe { CssString::from_leaked_raw(name) },
        symbols_type,
        symbols: unsafe { CssStringList::from_raw(symbols, symbol_count) },
    }))
}

/// Takes ownership of one strong reference to the image and to each non-null coordinate.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_cursor(
    image: *const StyleValueData,
    x: *const StyleValueData,
    y: *const StyleValueData,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Cursor {
        image: unsafe { RetainedStyleValueData::from_retained_pointer(image) },
        x: unsafe { RetainedStyleValueData::from_retained_optional_pointer(x) },
        y: unsafe { RetainedStyleValueData::from_retained_optional_pointer(y) },
    }))
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
        name: unsafe { CssString::from_leaked_raw(name) },
        origin_color: unsafe { RetainedStyleValueData::from_retained_optional_pointer(origin_color) },
    }))
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
}

/// Takes ownership of the options' retained values and strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_image_set(
    options: *const FfiImageSetOption,
    length: usize,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::ImageSet {
        options: unsafe { RetainedImageSetOptionList::from_raw(options, length) },
    }))
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
}

/// Takes ownership of the entries' retained values and names, recursively.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_grid_track_size_list(
    is_subgrid: bool,
    preserve_line_name_sets: bool,
    entries: *const GridTrackEntryInput,
    entry_count: usize,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::GridTrackSizeList {
        is_subgrid,
        preserve_line_name_sets,
        entries: unsafe { RetainedGridTrackEntryList::from_raw(entries, entry_count) },
    }))
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
    Arc::into_raw(Arc::new(StyleValueData::BasicShape {
        kind,
        v0: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v0) },
        v1: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v1) },
        v2: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v2) },
        v3: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v3) },
        v4: unsafe { RetainedStyleValueData::from_retained_optional_pointer(v4) },
        fill_rule,
        points: unsafe { RetainedShapePointList::from_raw(points, point_count) },
        path_string: unsafe { CssString::from_leaked_raw(path_string) },
    }))
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
}

/// Copies the borrowed native URL text and takes ownership of each modifier string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_image(
    url: crate::css::ffi_support::FfiUtf16View,
    url_type: u8,
    url_modifiers: *const FfiRequestUrlModifier,
    url_modifier_count: usize,
    resource_base_url: crate::css::ffi_support::FfiUtf16View,
    has_resource_base_url: bool,
    has_parent_style_sheet_origin_clean: bool,
    parent_style_sheet_origin_clean: bool,
    should_absolutize_url_for_computed_value: bool,
) -> *const StyleValueData {
    Arc::into_raw(Arc::new(StyleValueData::Image {
        url: unsafe { RetainedString::from_units(url.units().expect("invalid URL text")) },
        url_type,
        url_modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(url_modifiers, url_modifier_count) },
        resource_context: ImageResourceContext {
            base_url: unsafe { RetainedString::from_units(resource_base_url.units().expect("invalid base URL text")) },
            has_base_url: has_resource_base_url,
            has_parent_style_sheet_origin_clean,
            parent_style_sheet_origin_clean,
            should_absolutize_url_for_computed_value,
        },
    }))
}

pub(crate) unsafe fn release_style_value(value: *const StyleValueData) {
    if value.is_null() {
        return;
    }
    if replay_style_value_dependency_flags(value).is_some() {
        return;
    }
    let value_reference = std::mem::ManuallyDrop::new(unsafe { Arc::from_raw(value) });
    if Arc::strong_count(&value_reference) == 1 {
        crate::css::style::record_replay::invalidate_pointer(value as usize);
    }
    unsafe { Arc::decrement_strong_count(value) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_release(value: *const StyleValueData) {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueDestroyEntry);
    unsafe { release_style_value(value) };
}

/// Whether two shared style values hold the same value.
///
/// The addresses are compared first because a shared allocation is the common case, and the values
/// otherwise: two allocations built from the same declaration are equal, and a caller holding a
/// freshly computed one has no way to know that from the pointer alone.
///
/// # Safety
/// `first` and `second` must be null or point at live `StyleValueData` allocated by this module.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_equals(first: *const StyleValueData, second: *const StyleValueData) -> bool {
    if std::ptr::eq(first, second) {
        return true;
    }
    if first.is_null() || second.is_null() {
        return false;
    }
    // Replay pointers are opaque identity tokens rather than readable StyleValueData.
    if replay_style_value_dependency_flags(first).is_some() || replay_style_value_dependency_flags(second).is_some() {
        return false;
    }
    unsafe { *first == *second }
}

/// Retains one reference to a shared style value allocation.
///
/// # Safety
/// `value` must be null or point at a live `StyleValueData` allocated by this module.
pub(crate) unsafe fn retain_style_value(value: *const StyleValueData) -> *const StyleValueData {
    if !value.is_null() {
        if replay_style_value_dependency_flags(value).is_some() {
            return value;
        }
        unsafe { Arc::increment_strong_count(value) };
    }
    value
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_retain(value: *const StyleValueData) -> *const StyleValueData {
    unsafe { retain_style_value(value) }
}

#[cfg(test)]
mod substitution_clone_tests {
    use super::*;

    #[test]
    fn shorthand_clone_preserves_repeated_child_aliases() {
        let child = Arc::new(StyleValueData::Keyword { keyword: 1 });
        let properties = [1, 2];
        let children = [Arc::into_raw(child.clone()), Arc::into_raw(child)];
        let shorthand = unsafe {
            rust_style_value_create_shorthand(
                3,
                properties.as_ptr(),
                properties.len(),
                children.as_ptr(),
                children.len(),
            )
        };
        let cloned_shorthand = Arc::into_raw(Arc::new(unsafe { (*shorthand).clone() }));

        let StyleValueData::Shorthand { values: original, .. } = (unsafe { &*shorthand }) else {
            unreachable!()
        };
        let StyleValueData::Shorthand {
            values: cloned_values, ..
        } = (unsafe { &*cloned_shorthand })
        else {
            unreachable!()
        };
        assert_eq!(original.as_slice()[0].pointer(), original.as_slice()[1].pointer());
        assert_eq!(
            cloned_values.as_slice()[0].pointer(),
            cloned_values.as_slice()[1].pointer()
        );
        assert_ne!(original.as_slice()[0].pointer(), cloned_values.as_slice()[0].pointer());

        unsafe {
            rust_style_value_release(shorthand);
            rust_style_value_release(cloned_shorthand);
        }
    }
}

#[cfg(test)]
mod replay_tests {
    use super::*;

    #[test]
    fn retained_replay_tokens_can_move_between_threads() {
        let pointer = register_replay_style_value(0x1234, 0);
        let value = unsafe { RetainedStyleValueData::from_retained_pointer(pointer) };
        std::thread::spawn(move || {
            let copy = value.clone();
            assert_eq!(unsafe { style_value_content_hash(copy.pointer()) }, 0x1234);
            assert_eq!(replay_style_value_token(copy.pointer()), Some(0x1234));
        })
        .join()
        .unwrap();
    }

    #[test]
    fn distinct_replay_tokens_compare_unequal_without_being_dereferenced() {
        let first = register_replay_style_value(0x1234, 0);
        let second = register_replay_style_value(0x5678, 0);

        assert_eq!(replay_style_value_token(first), Some(0x1234));
        assert_eq!(replay_style_value_token(second), Some(0x5678));
        assert_eq!(replay_style_value_token(std::ptr::null()), None);
        assert!(unsafe { rust_style_value_equals(first, first) });
        assert!(!unsafe { rust_style_value_equals(first, second) });
    }
}

/// Whether a value's computed color depends on the element's used currentcolor: the
/// currentcolor keyword itself, a color function whose nested colors do, or an Effects
/// list whose shadow or filter colors do.
pub(crate) fn value_depends_on_current_color(value: &StyleValueData) -> bool {
    let retained_data_depends =
        |retained: &RetainedStyleValueData| retained.optional_data().is_some_and(value_depends_on_current_color);
    let list_depends = |values: &RetainedStyleValueDataList| values.as_slice().iter().any(retained_data_depends);
    match value {
        StyleValueData::Keyword { keyword } => *keyword == crate::css::style_compute::keyword::CURRENTCOLOR,
        StyleValueData::ColorFunction { origin_color, .. } => retained_data_depends(origin_color),
        StyleValueData::ColorMix {
            first_color,
            second_color,
            ..
        } => retained_data_depends(first_color) || retained_data_depends(second_color),
        StyleValueData::ContrastColor { color, .. } => retained_data_depends(color),
        StyleValueData::LightDark { light, dark, .. } => retained_data_depends(light) || retained_data_depends(dark),
        StyleValueData::Shadow { color, .. } => retained_data_depends(color),
        StyleValueData::Filter { value, .. } => retained_data_depends(value),
        StyleValueData::ValueList { values, .. } => list_depends(values),
        _ => false,
    }
}

/// Whether computing a value reads the element's effective color scheme.
pub(crate) fn value_depends_on_color_scheme(value: &StyleValueData) -> bool {
    let retained_data_depends =
        |retained: &RetainedStyleValueData| -> bool { value_depends_on_color_scheme(retained.data()) };
    match value {
        StyleValueData::Keyword { keyword } => {
            *keyword != crate::css::style_compute::keyword::CURRENTCOLOR
                && crate::css::style_compute::keyword_is_color(*keyword)
        }
        StyleValueData::ColorFunction { origin_color, .. } => {
            origin_color.optional_data().is_some_and(value_depends_on_color_scheme)
        }
        StyleValueData::ColorMix {
            first_color,
            second_color,
            ..
        } => retained_data_depends(first_color) || retained_data_depends(second_color),
        StyleValueData::ContrastColor { color, .. } => retained_data_depends(color),
        StyleValueData::LightDark { .. } => true,
        _ => false,
    }
}

/// Whether a retained specified value may read font metrics while computing.
pub(crate) fn value_may_depend_on_font_metrics(value: &StyleValueData) -> bool {
    !crate::css::style_compute::value_is_computationally_independent(value)
        .expect("font dependency requested for an unsupported value")
}

pub(crate) fn style_value_dependency_flags(value: *const StyleValueData) -> u8 {
    if let Some(flags) = replay_style_value_dependency_flags(value) {
        return flags;
    }
    let value = unsafe { &*value };
    u8::from(value_depends_on_current_color(value))
        | (u8::from(value_depends_on_color_scheme(value)) << 1)
        | (u8::from(value_may_depend_on_font_metrics(value)) << 2)
}

pub(crate) fn retained_value_depends_on_current_color(value: &RetainedStyleValueData) -> bool {
    style_value_dependency_flags(value.pointer()) & 1 != 0
}

pub(crate) fn retained_value_depends_on_color_scheme(value: &RetainedStyleValueData) -> bool {
    style_value_dependency_flags(value.pointer()) & 2 != 0
}

pub(crate) fn retained_value_may_depend_on_font_metrics(value: &RetainedStyleValueData) -> bool {
    style_value_dependency_flags(value.pointer()) & 4 != 0
}

/// # Safety
/// `data` must point at a valid StyleValueData.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_depends_on_current_color(data: *const c_void) -> bool {
    crate::css::ffi_stats::bump(crate::css::ffi_stats::FfiOp::StyleValueQueryEntry);
    value_depends_on_current_color(unsafe { &*(data as *const StyleValueData) })
}
