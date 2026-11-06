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

use crate::css::style_value_arc::OwnedStyleValue;
use crate::css::style_value_arc::StyleValueArc;
use std::ffi::c_void;

use crate::css::css_tokenizer::{
    ParserSource, ParserTokenKind, SourcePosition, TokenizerInput, trim_component_value_boundary_whitespace,
};
use crate::css::parser::arbitrary_substitution::SubstitutionFunctionsPresence;
use crate::css::parser::component_value::{
    ComponentKind, ComponentSerializationMode, ComponentValue, ComponentValueList,
};
use crate::css::serialize::{
    original_component_values_source, serialize_component_values_to_utf16, trim_ascii_whitespace_start,
};

pub(crate) use crate::css::css_string::{CssString, CssStringList};

pub enum ReifiedUnresolvedSegment {
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

pub(crate) fn utf16_equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left).is_ok_and(|left| left.eq_ignore_ascii_case(&right)))
}

pub(crate) fn split_var_arguments(values: &[ComponentValue]) -> Option<(&[u16], Option<&[ComponentValue]>)> {
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

// Appends the name of every custom property a `var()` or `inherit()` in `values` refers to, and returns whether every
// reference names its property with a plain identifier.
fn scan_custom_property_references<'a>(
    values: &'a [ComponentValue],
    references: &mut smallvec::SmallVec<[&'a [u16]; 4]>,
) -> bool {
    let mut all_references_visible = true;
    for value in values {
        let ComponentKind::Function {
            name,
            values: function_values,
        } = &value.kind
        else {
            if let ComponentKind::SimpleBlock { values, .. } = &value.kind {
                all_references_visible &= scan_custom_property_references(values, references);
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
                references.push(name);
            } else {
                all_references_visible = false;
            }
        }
        all_references_visible &= scan_custom_property_references(function_values, references);
    }
    all_references_visible
}

pub(crate) fn custom_property_references(value: &StyleValueData) -> Option<(impl Iterator<Item = &[u16]>, bool)> {
    let StyleValueData::Unresolved(unresolved_payload) = value else {
        return None;
    };
    Some((unresolved_payload.references(), unresolved_payload.references_visible()))
}

/// The text and the variable references of an unresolved value, for a CSSUnparsedValue.
pub(crate) fn reified_unresolved_segments(value: &StyleValueData) -> Vec<ReifiedUnresolvedSegment> {
    let StyleValueData::Unresolved(unresolved_payload) = value else {
        unreachable!("reification requires an unresolved style value");
    };
    reify_unresolved_segments(&unresolved_payload.components())
}

#[cfg(test)]
mod unresolved_component_tests {
    use super::*;

    #[test]
    fn unresolved_text_is_kept_as_ascii_however_it_arrives() {
        let units: Vec<u16> = "var(--x, 1px)".encode_utf16().collect();
        let from_utf16 = RetainedString::from_text(TokenizerInput::Utf16(&units));
        let from_ascii = RetainedString::from_text(TokenizerInput::Latin1(b"var(--x, 1px)"));
        assert!(matches!(from_utf16.units(), TokenizerInput::Latin1(_)));
        assert!(from_utf16 == from_ascii);
        let wide: Vec<u16> = "var(--\u{96ea})".encode_utf16().chain([0xd800]).collect();
        assert_eq!(RetainedString::from_text(TokenizerInput::Utf16(&wide)).to_utf16(), wide);
    }

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

    #[test]
    fn reference_scan_reports_dynamic_names() {
        let values = components("var(--one) [inherit(--two)] var(var(--dynamic))");
        let references = CustomPropertyReferences::scan(&values);
        assert!(!references.all_visible);
        assert_eq!(
            references.names.to_vec(),
            ["--one", "--two", "--dynamic"].map(|name| name.encode_utf16().collect::<Vec<_>>())
        );
    }

    fn unresolved(source: &str) -> StyleValueData {
        let units: Vec<u16> = source.encode_utf16().collect();
        crate::css::parser::value_parser::unresolved_value_from_string(
            &units,
            crate::css::parser::arbitrary_substitution::SubstitutionFunctionsPresence::default(),
        )
    }

    fn reference_names(value: &StyleValueData) -> (Vec<String>, bool) {
        let (names, all_visible) = custom_property_references(value).unwrap();
        (
            names.map(|name| String::from_utf16(name).unwrap()).collect(),
            all_visible,
        )
    }

    #[test]
    fn a_value_notes_the_custom_properties_it_references() {
        assert_eq!(
            reference_names(&unresolved("calc(var(--a) + inherit(--bc, 1px)) var(--a)")),
            (vec!["--a".to_owned(), "--bc".to_owned(), "--a".to_owned()], true)
        );
        assert_eq!(reference_names(&unresolved("1px solid")), (vec![], true));
        assert_eq!(
            reference_names(&unresolved("var(var(--d))")),
            (vec!["--d".to_owned()], false)
        );
        // NB: A name is noted with its escapes decoded, and a text that is not all ASCII is kept as
        //     UTF-16, after which the names follow aligned all the same.
        assert_eq!(
            reference_names(&unresolved("var(--\\61 b)")),
            (vec!["--ab".to_owned()], true)
        );
        assert_eq!(
            reference_names(&unresolved("var(--\u{96ea}) var(--abc)")),
            (vec!["--\u{96ea}".to_owned(), "--abc".to_owned()], true)
        );
        let copied = unresolved("var(--\u{96ea}, x) var(--abc)").clone();
        assert_eq!(
            reference_names(&copied),
            (vec!["--\u{96ea}".to_owned(), "--abc".to_owned()], true)
        );
    }

    #[test]
    fn a_value_notes_what_its_text_holds() {
        let facts = |source: &str| {
            let StyleValueData::Unresolved(payload) = unresolved(source) else {
                unreachable!();
            };
            (
                payload.facts().is_lone_css_wide_keyword(),
                payload.facts().calls_tree_counting_function(),
            )
        };
        assert_eq!(facts(" inherit "), (true, false));
        assert_eq!(facts("inherit green"), (false, false));
        assert_eq!(facts("calc(sibling-index() * 1px)"), (false, true));
    }

    #[test]
    fn a_value_made_of_its_serialization_tokenizes_back_to_its_components() {
        use crate::css::parser::component_value::ComponentSerializationMode;
        // NB: Two numbers with nothing between them, as a substitution leaves `var(--n)0`, which their
        //     serialization keeps apart with a comment.
        let values: Vec<ComponentValue> = components("12 1.0")
            .into_iter()
            .filter(|value| !value.is_whitespace())
            .collect();
        let source =
            crate::css::serialize::serialize_component_values_to_utf16(&values, ComponentSerializationMode::Retokenize);
        assert_eq!(String::from_utf16_lossy(&source), "12/**/1.0");
        let made_of = |components| {
            UnresolvedBox::new(
                TokenizerInput::Utf16(&source),
                components,
                SubstitutionFunctionsPresence::default(),
                false,
                RetainedStyleValueData::none(),
            )
        };
        let serialized = made_of(UnresolvedValueComponents::OfSerialization(&values));
        assert!(components_equal(&serialized.components(), &values));
        let written = made_of(UnresolvedValueComponents::OfText(&values));
        assert_eq!(written.components().len(), 3);
    }
}

/// Whether two lists hold the same components, by their token kinds, wherever their text came from.
#[cfg(test)]
pub(crate) fn components_equal(first: &[ComponentValue], second: &[ComponentValue]) -> bool {
    first.len() == second.len()
        && first
            .iter()
            .zip(second)
            .all(|(first, second)| match (&first.kind, &second.kind) {
                (ComponentKind::Token(first), ComponentKind::Token(second)) => first == second,
                (
                    ComponentKind::Function {
                        name: first_name,
                        values: first,
                    },
                    ComponentKind::Function {
                        name: second_name,
                        values: second,
                    },
                ) => first_name == second_name && components_equal(first, second),
                (
                    ComponentKind::SimpleBlock {
                        opening: first_opening,
                        values: first,
                    },
                    ComponentKind::SimpleBlock {
                        opening: second_opening,
                        values: second,
                    },
                ) => first_opening == second_opening && components_equal(first, second),
                _ => false,
            })
}

pub(crate) fn style_value_content_hash(value: Option<&StyleValueData>) -> u64 {
    let Some(value) = value else {
        return 0;
    };
    value.content_hash()
}

/// A strong reference to immutable Rust-owned style value data.
#[repr(C)]
pub struct RetainedStyleValueData {
    pointer: *const c_void,
}

impl crate::heap_size::HeapSize for RetainedStyleValueData {
    /// Counts the value's allocation aside, the first time it is reached, as every owner holding
    /// it shares it. The values a value holds are not counted.
    fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        if sizer.first_sight(self.pointer()) {
            sizer.style_value_bytes += crate::heap_size::STYLE_VALUE_HEADER_BYTES
                + size_of::<StyleValueData>()
                + self.optional_data().map_or(0, StyleValueData::boxed_payload_bytes);
        }
        0
    }
}

// SAFETY: This handle owns an Arc reference and only exposes shared access to its pointee.
unsafe impl Send for RetainedStyleValueData where StyleValueData: Send + Sync {}
unsafe impl Sync for RetainedStyleValueData where StyleValueData: Send + Sync {}

impl PartialEq for RetainedStyleValueData {
    fn eq(&self, other: &Self) -> bool {
        match (self.optional_data(), other.optional_data()) {
            (Some(first), Some(second)) => style_values_equal(first, second),
            (None, None) => true,
            _ => false,
        }
    }
}

// Common scalars recur in parsed values, computed styles, and values created through FFI.
// Direct indexing shares them without hashing every property or retaining arbitrary input.
const SHARED_KEYWORDS: usize = 1024;
const SHARED_INTEGERS: usize = 384;
const SHARED_NUMBERS: usize = 256;
const SHARED_LENGTHS: usize = 1024;
const SHARED_PERCENTAGES: usize = 101;
const SHARED_VALUE_COUNT: usize =
    SHARED_KEYWORDS + SHARED_INTEGERS + SHARED_NUMBERS + SHARED_LENGTHS + SHARED_PERCENTAGES;
static VALUES: [std::sync::OnceLock<StyleValueArc>; SHARED_VALUE_COUNT] =
    [const { std::sync::OnceLock::new() }; SHARED_VALUE_COUNT];

pub(crate) fn shared_style_value(value: StyleValueData) -> StyleValueArc {
    let integer_index = |value: f64, count: usize| {
        (value >= 0.0 && value < count as f64 && value.to_bits() == (value as u16 as f64).to_bits())
            .then_some(value as usize)
    };
    let index = match &value {
        StyleValueData::Keyword { keyword } if usize::from(*keyword) < SHARED_KEYWORDS => Some(usize::from(*keyword)),
        StyleValueData::Integer { value } if (-128..256).contains(value) => {
            Some(SHARED_KEYWORDS + (*value + 128) as usize)
        }
        StyleValueData::Number { value } => {
            integer_index(*value, SHARED_NUMBERS).map(|index| SHARED_KEYWORDS + SHARED_INTEGERS + index)
        }
        StyleValueData::Length { value, unit } if *unit == crate::css::style_compute::px_length_unit() => {
            integer_index(*value, SHARED_LENGTHS)
                .map(|index| SHARED_KEYWORDS + SHARED_INTEGERS + SHARED_NUMBERS + index)
        }
        StyleValueData::Percentage { value } => integer_index(*value, SHARED_PERCENTAGES)
            .map(|index| SHARED_KEYWORDS + SHARED_INTEGERS + SHARED_NUMBERS + SHARED_LENGTHS + index),
        _ => None,
    };
    if let Some(index) = index {
        StyleValueArc::clone(VALUES[index].get_or_init(|| StyleValueArc::immortal(value)))
    } else {
        StyleValueArc::new(value)
    }
}

impl RetainedStyleValueData {
    /// The value of `keyword` the shared values hold, which every value of it shares.
    pub(crate) fn shared_keyword(keyword: u16) -> &'static Self {
        assert!(
            usize::from(keyword) < SHARED_KEYWORDS,
            "the shared values hold every keyword"
        );
        let value =
            VALUES[usize::from(keyword)].get_or_init(|| StyleValueArc::immortal(StyleValueData::Keyword { keyword }));
        // SAFETY: A handle is one pointer at the value, as an arc is, and the shared values keep
        //         theirs for the life of the process.
        unsafe { &*std::ptr::from_ref(value).cast::<Self>() }
    }

    pub(crate) fn none() -> Self {
        Self {
            pointer: std::ptr::null(),
        }
    }

    pub(crate) fn from_owned(data: StyleValueData) -> Self {
        Self::from_arc(shared_style_value(data))
    }

    pub(crate) fn from_arc(value: StyleValueArc) -> Self {
        // SAFETY: `into_raw` transfers one reference to this handle.
        unsafe { Self::from_retained_pointer(StyleValueArc::into_raw(value)) }
    }

    pub(crate) fn from_optional_arc(value: Option<StyleValueArc>) -> Self {
        value.map_or_else(Self::none, Self::from_arc)
    }

    pub(crate) fn pointer(&self) -> *const StyleValueData {
        self.pointer.cast()
    }

    /// The handle's value, which an empty handle does not have.
    pub(crate) fn data(&self) -> &StyleValueData {
        self.optional_data().expect("an empty style value handle has no data")
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

    /// Releases the handle's reference to the caller as a raw pointer, null for an empty handle.
    pub(crate) fn into_pointer(self) -> *const StyleValueData {
        let pointer = self.pointer();
        std::mem::forget(self);
        pointer
    }

    /// The handles of `values`, borrowed: each is one pointer at the same value.
    pub(crate) fn slice_from_shared(values: &[StyleValueArc]) -> &[Self] {
        const _: () = assert!(size_of::<StyleValueArc>() == size_of::<RetainedStyleValueData>());
        const _: () = assert!(align_of::<StyleValueArc>() == align_of::<RetainedStyleValueData>());
        // SAFETY: Both are one pointer at the counted value, `StyleValueArc` transparently and the
        //         handle as `repr(C)`, and a borrowed handle neither adds nor drops a reference.
        unsafe { ak::slice_from_raw_parts(values.as_ptr().cast(), values.len()) }
    }

    /// Converts the retained reference into an Arc, transferring the strong reference.
    pub(crate) fn into_arc(self) -> StyleValueArc {
        let pointer = self.pointer();
        std::mem::forget(self);
        // SAFETY: The retained reference owns exactly one strong reference.
        unsafe { StyleValueArc::from_raw(pointer) }
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

/// A retained, Rust-owned array of shared style value data references, held by one pointer to its
/// first element, with the element count in the word before it (null when empty). A list fits in
/// the value holding it beside a few small fields.
#[repr(C)]
pub struct RetainedStyleValueDataList {
    pointer: *mut RetainedStyleValueData,
}

impl RetainedStyleValueDataList {
    const HEADER: usize = size_of::<usize>();

    fn layout(length: usize) -> std::alloc::Layout {
        const _: () = assert!(align_of::<RetainedStyleValueData>() <= size_of::<usize>());
        let bytes = length
            .checked_mul(size_of::<RetainedStyleValueData>())
            .and_then(|bytes| bytes.checked_add(Self::HEADER))
            .expect("style value list too long");
        std::alloc::Layout::from_size_align(bytes, align_of::<usize>()).expect("style value list too long")
    }

    pub(crate) fn len(&self) -> usize {
        if self.pointer.is_null() {
            return 0;
        }
        // SAFETY: A non-null pointer follows the element count of a live allocation.
        unsafe { *self.pointer.cast::<usize>().sub(1) }
    }

    pub(crate) fn as_slice(&self) -> &[RetainedStyleValueData] {
        if self.pointer.is_null() {
            return &[];
        }
        // SAFETY: A non-null pointer points at `len()` initialized elements.
        unsafe { ak::slice_from_raw_parts(self.pointer, self.len()) }
    }

    fn from_iterator(values: impl ExactSizeIterator<Item = RetainedStyleValueData>) -> Self {
        let length = values.len();
        if length == 0 {
            return Self {
                pointer: std::ptr::null_mut(),
            };
        }
        let layout = Self::layout(length);
        // SAFETY: The layout has a nonzero size.
        let base = unsafe { std::alloc::alloc(layout) };
        if base.is_null() {
            std::alloc::handle_alloc_error(layout);
        }
        // SAFETY: The allocation holds the count, then `length` elements.
        unsafe {
            base.cast::<usize>().write(length);
            let pointer = base.add(Self::HEADER).cast::<RetainedStyleValueData>();
            let mut written = 0;
            for value in values.take(length) {
                pointer.add(written).write(value);
                written += 1;
            }
            assert_eq!(written, length, "style value list iterator ended early");
            Self { pointer }
        }
    }

    pub(crate) fn from_retained_values(values: Vec<RetainedStyleValueData>) -> Self {
        Self::from_iterator(values.into_iter())
    }

    /// Takes ownership of one strong reference to each value.
    ///
    /// # Safety
    /// `values` must point to `length` strong references returned by `rust_style_value_retain`.
    unsafe fn from_retained_pointers(values: *const *const StyleValueData, length: usize) -> Self {
        Self::from_iterator(
            (0..length).map(|i| unsafe { RetainedStyleValueData::from_retained_pointer(*values.add(i)) }),
        )
    }
}

impl PartialEq for RetainedStyleValueDataList {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

impl Drop for RetainedStyleValueDataList {
    fn drop(&mut self) {
        if self.pointer.is_null() {
            return;
        }
        let length = self.len();
        // SAFETY: The list owns its `length` elements and the allocation they are in.
        unsafe {
            std::ptr::drop_in_place(std::ptr::slice_from_raw_parts_mut(self.pointer, length));
            std::alloc::dealloc(self.pointer.cast::<u8>().sub(Self::HEADER), Self::layout(length));
        }
    }
}

/// Preserve matrix-only literal angles only while their canonical argument is unchanged.
/// All other slots come from the newly mapped values, so lengths and rotation axes cannot become stale.
pub(crate) fn map_transform_matrix_values(
    previous: &[RetainedStyleValueData],
    values: &[RetainedStyleValueData],
    matrix_values: &[RetainedStyleValueData],
) -> RetainedStyleValueDataList {
    let mut mapped = Vec::new();
    for (index, matrix_value) in matrix_values.iter().enumerate() {
        let (Some(previous), Some(value)) = (previous.get(index), values.get(index)) else {
            continue;
        };
        if matches!(matrix_value.data(), StyleValueData::Angle { .. })
            && previous.data() == value.data()
            && matrix_value.data() != value.data()
        {
            if mapped.is_empty() {
                mapped = values.to_vec();
            }
            mapped[index] = matrix_value.clone_retained();
        }
    }
    RetainedStyleValueDataList::from_retained_values(mapped)
}

impl Clone for RetainedStyleValueDataList {
    fn clone(&self) -> Self {
        let mut cloned_by_pointer = Vec::new();
        let values = self
            .as_slice()
            .iter()
            .map(|value| {
                let Some(data) = value.optional_data() else {
                    return RetainedStyleValueData::none();
                };
                let pointer = data as *const StyleValueData as usize;
                if let Some((_, cloned)) = cloned_by_pointer.iter().find(|(original, _)| *original == pointer) {
                    return unsafe { RetainedStyleValueData::from_retained_pointer(retain_style_value(*cloned)) };
                }
                let cloned = StyleValueArc::into_raw(StyleValueArc::new(data.clone()));
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
        retained_list!($list, $element, native);
    };
    ($list:ident, $element:ty, native) => {
        retained_list_drop!($list);
        impl Clone for $list {
            fn clone(&self) -> Self {
                if self.pointer.is_null() {
                    return Self {
                        pointer: std::ptr::null_mut(),
                        length: 0,
                    };
                }
                let elements = unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
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
        unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
    }
}

/// Shared native URL text, independent of host strings and the atom table.
#[repr(C)]
pub struct RetainedString {
    /// Null for the empty string, which most images' unset base URL is.
    storage: *const c_void,
}

// SAFETY: This handle owns a reference to immutable ASCII or UTF-16 text, counted atomically.
unsafe impl Send for RetainedString {}
unsafe impl Sync for RetainedString {}

/// The header of a retained string's one allocation, which its text follows: ASCII bytes or UTF-16
/// code units.
#[repr(C)]
struct RetainedStringHeader {
    references: std::sync::atomic::AtomicUsize,
    length: u32,
    is_latin1: bool,
}

impl RetainedString {
    fn layout(length: usize, is_latin1: bool) -> std::alloc::Layout {
        let unit = if is_latin1 { 1 } else { size_of::<u16>() };
        let bytes = length
            .checked_mul(unit)
            .and_then(|bytes| bytes.checked_add(size_of::<RetainedStringHeader>()))
            .expect("retained string too long");
        std::alloc::Layout::from_size_align(bytes, align_of::<RetainedStringHeader>())
            .expect("retained string too long")
    }

    /// A string of `length` units, Latin-1 bytes or UTF-16 code units as `is_latin1` says, which
    /// `write` fills.
    fn with_units(length: usize, is_latin1: bool, write: impl FnOnce(*mut u8)) -> Self {
        if length == 0 {
            return Self {
                storage: std::ptr::null(),
            };
        }
        let layout = Self::layout(length, is_latin1);
        // SAFETY: The layout is nonzero; the header is written, then `write` fills the text.
        unsafe {
            let base = std::alloc::alloc(layout);
            if base.is_null() {
                std::alloc::handle_alloc_error(layout);
            }
            base.cast::<RetainedStringHeader>().write(RetainedStringHeader {
                references: std::sync::atomic::AtomicUsize::new(1),
                length: u32::try_from(length).expect("retained string too long"),
                is_latin1,
            });
            write(base.add(size_of::<RetainedStringHeader>()));
            Self { storage: base.cast() }
        }
    }

    fn header(&self) -> Option<&RetainedStringHeader> {
        // SAFETY: A non-null storage is a live allocation starting with its header.
        (!self.storage.is_null()).then(|| unsafe { &*self.storage.cast::<RetainedStringHeader>() })
    }

    /// Any text, which need not be well-formed UTF-16.
    pub(crate) fn from_text(units: TokenizerInput<'_>) -> Self {
        Self::from_units(units)
    }

    fn from_units(units: TokenizerInput<'_>) -> Self {
        match units {
            TokenizerInput::Latin1(bytes) => Self::with_units(bytes.len(), true, |text| unsafe {
                std::ptr::copy_nonoverlapping(bytes.as_ptr(), text, bytes.len());
            }),
            TokenizerInput::Utf16(units) if units.iter().all(|unit| *unit <= 0x7f) => {
                Self::with_units(units.len(), true, |text| {
                    for (index, unit) in units.iter().enumerate() {
                        unsafe { text.add(index).write(*unit as u8) };
                    }
                })
            }
            TokenizerInput::Utf16(units) => Self::with_units(units.len(), false, |text| unsafe {
                std::ptr::copy_nonoverlapping(units.as_ptr(), text.cast::<u16>(), units.len());
            }),
        }
    }

    pub(crate) fn from_ascii(string: String) -> Self {
        assert!(string.is_ascii());
        Self::from_units(TokenizerInput::Latin1(string.as_bytes()))
    }

    pub(crate) fn from_utf16(string: &[u16]) -> Option<Self> {
        if char::decode_utf16(string.iter().copied()).any(|character| character.is_err()) {
            return None;
        }
        Some(Self::from_units(TokenizerInput::Utf16(string)))
    }

    /// The text as UTF-16 code units.
    #[cfg(test)]
    pub(crate) fn to_utf16(&self) -> Vec<u16> {
        let units = self.units();
        let mut text = Vec::with_capacity(units.len());
        units.append_to(&mut text);
        text
    }

    pub(crate) fn units(&self) -> TokenizerInput<'_> {
        let Some(header) = self.header() else {
            return TokenizerInput::Latin1(&[]);
        };
        let length = header.length as usize;
        // SAFETY: The text follows the header, `length` units of the kind the header names.
        unsafe {
            let text = self.storage.cast::<u8>().add(size_of::<RetainedStringHeader>());
            if header.is_latin1 {
                TokenizerInput::Latin1(ak::slice_from_raw_parts(text, length))
            } else {
                TokenizerInput::Utf16(ak::slice_from_raw_parts(text.cast::<u16>(), length))
            }
        }
    }

    pub(crate) fn url_input(&self) -> liburl_rust::url::UrlInput<'_> {
        match self.units() {
            // SAFETY: ASCII is UTF-8.
            TokenizerInput::Latin1(bytes) if bytes.is_ascii() => {
                liburl_rust::url::UrlInput::Utf8(unsafe { std::str::from_utf8_unchecked(bytes) })
            }
            TokenizerInput::Latin1(bytes) => liburl_rust::url::UrlInput::Latin1(bytes),
            TokenizerInput::Utf16(units) => liburl_rust::url::UrlInput::Utf16(units),
        }
    }

    pub(crate) fn ascii_bytes(&self) -> &[u8] {
        let TokenizerInput::Latin1(bytes) = self.units() else {
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
        if let Some(header) = self.header() {
            header.references.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
        }
        Self { storage: self.storage }
    }
}

impl Drop for RetainedString {
    fn drop(&mut self) {
        let Some(header) = self.header() else {
            return;
        };
        if header.references.fetch_sub(1, std::sync::atomic::Ordering::Release) != 1 {
            return;
        }
        std::sync::atomic::fence(std::sync::atomic::Ordering::Acquire);
        let layout = Self::layout(header.length as usize, header.is_latin1);
        // SAFETY: The last reference frees the allocation it was made in.
        unsafe { std::alloc::dealloc(self.storage.cast_mut().cast::<u8>(), layout) };
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_css_url_text_view(text: &RetainedString) -> crate::css::ffi_support::FfiUtf16View<'_> {
    crate::css::ffi_support::FfiUtf16View::from_input(text.units())
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
        unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
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

retained_list!(RetainedColorStopList, RetainedColorStop, native);

impl RetainedCounterDefinition {
    pub(crate) fn value(&self) -> &RetainedStyleValueData {
        &self.value
    }

    /// The counter's integer, or none when the definition leaves it out. A computed value only
    /// ever holds an integer or a calculation that resolves to one.
    pub(crate) fn integer(&self) -> Option<i32> {
        match self.value.optional_data()? {
            StyleValueData::Integer { value } => Some(*value),
            calculated @ StyleValueData::Calculated { .. } => Some(
                crate::css::calc::resolve_calculated_integer_without_context(calculated)
                    .expect("a computed counter value resolves to an integer"),
            ),
            _ => panic!("a computed counter value is an integer"),
        }
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
                unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
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
        unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
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

retained_list!(RetainedLinearEasingStopList, RetainedLinearEasingStop, native);

/// The kind of one grid track list entry.
#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
// The C++ constructor supplies every variant through the FFI input.
pub enum GridTrackEntryKind {
    LineNames,
    Size,
    MinMax,
    Repeat,
}

/// A Rust-owned array of retained grid track list entries.
#[repr(C)]
pub struct RetainedGridTrackEntryList {
    // Opaque at the FFI boundary to avoid a circular C++ definition with RetainedGridTrackEntry.
    pointer: *mut c_void,
    length: usize,
}

/// A retained, Rust-owned grid track list entry.
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
        unsafe { ak::slice_from_raw_parts(self.pointer.cast(), self.length) }
    }

    pub(crate) fn from_retained_entries(entries: Vec<RetainedGridTrackEntry>) -> Self {
        let slice = entries.into_boxed_slice();
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
        Self::retained_size(RetainedStyleValueData::from_owned(value))
    }

    fn retained_size(size_value: RetainedStyleValueData) -> Self {
        Self {
            kind: GridTrackEntryKind::Size,
            names: CssStringList::from_strings(Vec::new()),
            size_value,
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

retained_list!(RetainedShapePointList, RetainedShapePoint, native);

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
    pub(crate) fn new(value_type: u8, min: f64, max: f64) -> Self {
        Self { value_type, min, max }
    }

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

    pub(crate) fn from_numeric_ranges(ranges: impl IntoIterator<Item = (u8, f64, f64)>) -> Self {
        let ranges = ranges
            .into_iter()
            .map(|(value_type, min, max)| RetainedNumericRangeByType { value_type, min, max })
            .collect::<Vec<_>>();
        if ranges.is_empty() {
            return Self::empty();
        }
        let ranges = ranges.into_boxed_slice();
        let length = ranges.len();
        let pointer = Box::into_raw(ranges) as *mut RetainedNumericRangeByType;
        Self { pointer, length }
    }

    pub(crate) fn as_slice(&self) -> &[RetainedNumericRangeByType] {
        if self.pointer.is_null() {
            return &[];
        }
        unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
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
                        unsafe { ak::slice_from_raw_parts(list.pointer.cast::<$element>(), list.length) }
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

// Basic shapes are uncommon and have enough fields to enlarge every scalar value.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct BasicShapeData {
    pub(crate) kind: u8,
    pub(crate) fill_rule: u8,
    pub(crate) v0: RetainedStyleValueData,
    pub(crate) v1: RetainedStyleValueData,
    pub(crate) v2: RetainedStyleValueData,
    pub(crate) v3: RetainedStyleValueData,
    pub(crate) v4: RetainedStyleValueData,
    pub(crate) points: RetainedShapePointList,
    pub(crate) path: crate::css::css_path::CssPath,
    /// The commands of a shape(), whose coordinates are `points` in order.
    pub(crate) commands: ShapeCommands,
}

/// https://drafts.csswg.org/css-shapes-1/#shape-function
/// One command of a shape(). Its points, in order, are its end point, then its control points, or its arc's
/// radii and rotation. Each point holds two length-percentages, the two edges of a position, or a keyword.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct ShapeCommand {
    pub(crate) kind: ShapeCommandKind,
    /// Whether the end point is given with `by`, relative to the command's starting point.
    pub(crate) relative: bool,
    /// What each control point is relative to.
    pub(crate) control_points: [ShapeControlPointAnchor; 2],
    pub(crate) control_point_count: u8,
    /// Whether an arc's single radius applies to both axes.
    pub(crate) arc_has_one_radius: bool,
    pub(crate) arc_clockwise: bool,
    pub(crate) arc_large: bool,
    /// Whether an arc's rotation was given. Arcs hold their rotation angle as a third point either way.
    pub(crate) arc_has_rotation: bool,
}

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShapeCommandKind {
    From,
    Move,
    Line,
    HorizontalLine,
    VerticalLine,
    Curve,
    Smooth,
    Arc,
    Close,
}

/// What a shape() control point is relative to: the command's start or end point, or the reference box.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ShapeControlPointAnchor {
    Start,
    End,
    Origin,
}

impl ShapeCommand {
    pub(crate) fn new(kind: ShapeCommandKind, relative: bool) -> Self {
        Self {
            kind,
            relative,
            control_points: [ShapeControlPointAnchor::Start; 2],
            control_point_count: 0,
            arc_has_one_radius: false,
            arc_clockwise: false,
            arc_large: false,
            arc_has_rotation: false,
        }
    }
}

/// The commands of a shape().
#[repr(C)]
pub struct ShapeCommands {
    pointer: *mut ShapeCommand,
    length: usize,
}

retained_list!(ShapeCommands, ShapeCommand, native);
retained_list_as_slice!(ShapeCommands, ShapeCommand);
retained_list_from_vec!(ShapeCommands, ShapeCommand);
retained_list_send_sync!(ShapeCommands, ShapeCommand);

impl ShapeCommands {
    pub(crate) fn new(commands: Vec<ShapeCommand>) -> Self {
        Self::from_retained_elements(commands)
    }
}

impl Default for ShapeCommands {
    fn default() -> Self {
        Self {
            pointer: std::ptr::null_mut(),
            length: 0,
        }
    }
}

impl PartialEq for ShapeCommands {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

/// The box a [`StyleValueData`] variant too large to sit beside the tag keeps its payload in, laid
/// out as one pointer C++ reads the payload through.
#[repr(C)]
pub struct StyleValueBox<T> {
    pointer: std::ptr::NonNull<T>,
}

// SAFETY: The box owns its payload as a `Box` would.
unsafe impl<T: Send> Send for StyleValueBox<T> {}
unsafe impl<T: Sync> Sync for StyleValueBox<T> {}

impl<T> StyleValueBox<T> {
    pub(crate) fn new(payload: T) -> Self {
        Self {
            pointer: std::ptr::NonNull::from(Box::leak(Box::new(payload))),
        }
    }

    pub(crate) fn into_inner(self) -> T {
        let pointer = self.pointer.as_ptr();
        std::mem::forget(self);
        // SAFETY: The box owned the payload it leaked in `new`, and is forgotten.
        *unsafe { Box::from_raw(pointer) }
    }
}

impl<T> std::ops::Deref for StyleValueBox<T> {
    type Target = T;

    fn deref(&self) -> &T {
        // SAFETY: The box owns a live payload.
        unsafe { self.pointer.as_ref() }
    }
}

impl<T> std::ops::DerefMut for StyleValueBox<T> {
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: The box owns a live payload, and `&mut self` borrows it exclusively.
        unsafe { self.pointer.as_mut() }
    }
}

impl<T: Clone> Clone for StyleValueBox<T> {
    fn clone(&self) -> Self {
        Self::new((**self).clone())
    }
}

impl<T: PartialEq> PartialEq for StyleValueBox<T> {
    fn eq(&self, other: &Self) -> bool {
        **self == **other
    }
}

impl<T> Drop for StyleValueBox<T> {
    fn drop(&mut self) {
        // SAFETY: The box owns the payload it leaked in `new`.
        drop(unsafe { Box::from_raw(self.pointer.as_ptr()) });
    }
}

/// A Rust-owned shape allocation with an explicit pointer layout for C++ bindings.
#[repr(C)]
pub struct OwnedBasicShapeData {
    pointer: *mut BasicShapeData,
}

// SAFETY: This handle owns its box and exposes only immutable access to its fields.
unsafe impl Send for OwnedBasicShapeData where BasicShapeData: Send {}
unsafe impl Sync for OwnedBasicShapeData where BasicShapeData: Sync {}

impl OwnedBasicShapeData {
    pub(crate) fn new(shape: BasicShapeData) -> Self {
        Self {
            pointer: Box::into_raw(Box::new(shape)),
        }
    }

    pub(crate) fn as_ref(&self) -> &BasicShapeData {
        // SAFETY: Every handle owns a live box for the duration of this borrow.
        unsafe { &*self.pointer }
    }
}

impl Clone for OwnedBasicShapeData {
    fn clone(&self) -> Self {
        Self::new(self.as_ref().clone())
    }
}

impl PartialEq for OwnedBasicShapeData {
    fn eq(&self, other: &Self) -> bool {
        self.as_ref() == other.as_ref()
    }
}

impl Drop for OwnedBasicShapeData {
    fn drop(&mut self) {
        // SAFETY: The handle owns exactly one box and transfers it back on drop.
        drop(unsafe { Box::from_raw(self.pointer) });
    }
}

/// The `function` of `StyleValueData::Counter`.
pub(crate) const COUNTER_FUNCTION_COUNTER: u8 = 0;
pub(crate) const COUNTER_FUNCTION_COUNTERS: u8 = 1;

/// The data of a single immutable CSS style value.
///
/// Variant payload fields are read directly by the corresponding C++ StyleValue subclass, so
/// changing a payload changes the C++ accessors reading it.
/// The byte discriminant shares each variant's padding instead of preceding a separate union.
#[repr(u8)]
// NB: Variant payload fields are only read by C++ through the exposed layout.
#[derive(Clone, PartialEq)]
pub enum StyleValueData {
    /// A CSS keyword. The value is the generated C++ `enum class Keyword : u16`, opaque to Rust.
    Keyword { keyword: u16 },
    /// A CSS `<number>`.
    Number { value: f64 },
    /// A CSS `<integer>`.
    Integer { value: i32 },
    /// A CSS `<angle>`. The unit is the C++ `enum class AngleUnit : u8`, opaque to Rust.
    Angle { unit: u8, value: f64 },
    /// A CSS `<flex>`. The unit is the C++ `enum class FlexUnit : u8`, opaque to Rust.
    Flex { unit: u8, value: f64 },
    /// A CSS `<frequency>`. The unit is the C++ `enum class FrequencyUnit : u8`, opaque to Rust.
    Frequency { unit: u8, value: f64 },
    /// A CSS `<length>`. The unit is the C++ `enum class LengthUnit : u8`, opaque to Rust.
    Length { unit: u8, value: f64 },
    /// A CSS `<percentage>`.
    Percentage { value: f64 },
    /// A CSS `<resolution>`. The unit is the C++ `enum class ResolutionUnit : u8`, opaque to Rust.
    Resolution { unit: u8, value: f64 },
    /// A CSS `<time>`. The unit is the C++ `enum class TimeUnit : u8`, opaque to Rust.
    Time { unit: u8, value: f64 },
    /// A basic shape. Kinds: inset (0), xywh (1) and rect (2) use five retained value slots;
    /// circle (3) and ellipse (4) use two; polygon (5) uses the fill rule and points; path (6)
    /// uses the fill rule and the retained serialized path data string.
    BasicShape { shape: OwnedBasicShapeData },
    /// A calc() or other math function: the retained calculation node tree root, its resolved
    /// numeric type, and the parse-time calculation context.
    Calculated(StyleValueBox<CalculatedPayload>),
    /// A CSS `<ratio>`, e.g. `16 / 9`. The numerator and denominator are style values.
    Ratio(StyleValueBox<RatioPayload>),
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
    /// A pending-substitution value retaining the shorthand it is part of and the value that
    /// shorthand was written with.
    PendingSubstitution {
        original_shorthand: u16,
        original_shorthand_value: RetainedStyleValueData,
    },
    /// scrollbar-color with retained thumb and track color values.
    ScrollbarColor(StyleValueBox<ScrollbarColorPayload>),
    /// rect() with four retained edge style values.
    Rect(StyleValueBox<RectPayload>),
    /// A CSS `<string>`, with its animation-name custom-ident classification captured when C++
    /// creates the immutable value so computation does not read the string across FFI.
    String {
        is_valid_animation_name_custom_ident: bool,
        serialize_without_quotes: bool,
        string: CssString,
    },
    /// An unrecognized CSS function, kept as its name and argument value.
    Function(StyleValueBox<FunctionPayload>),
    /// An OpenType tag with its value, from font-feature-settings or font-variation-settings.
    /// The mode is the C++ OpenTypeTaggedStyleValue::Mode, opaque to Rust.
    OpenTypeTagged(StyleValueBox<OpenTypeTaggedPayload>),
    /// font-style: a keyword (the C++ `enum class FontStyleKeyword : u8`, opaque to Rust) and
    /// an optional oblique angle style value (null when absent).
    FontStyle {
        font_style: u8,
        angle_value: RetainedStyleValueData,
    },
    /// text-indent: a length-percentage style value plus the hanging and each-line flags.
    TextIndent {
        hanging: bool,
        each_line: bool,
        length_percentage: RetainedStyleValueData,
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
    BackgroundSize(StyleValueBox<BackgroundSizePayload>),
    /// A background repeat-style. Both fields are the C++ `enum class Repetition : u8`, opaque
    /// to Rust.
    RepeatStyle { repeat_x: u8, repeat_y: u8 },
    /// border-image-slice: four retained offset data allocations and the fill keyword.
    BorderImageSlice(StyleValueBox<BorderImageSlicePayload>),
    /// anchor-size(): an optional anchor name, an optional size keyword (the C++ `enum class
    /// AnchorSize : u8`, opaque to Rust) and an optional retained fallback value.
    AnchorSize(StyleValueBox<AnchorSizePayload>),
    /// anchor(): an optional anchor name, the retained side style value and an optional
    /// retained fallback value.
    Anchor(StyleValueBox<AnchorPayload>),
    /// A CSS `<position>` with its two retained edge style values.
    Position(StyleValueBox<PositionPayload>),
    /// A shadow. The type and placement are C++ enums, opaque to Rust; the color, blur radius
    /// and spread distance are optional retained style values (null when absent).
    Shadow(StyleValueBox<ShadowPayload>),
    /// content with its retained content list and optional alt-text list (null when absent).
    Content(StyleValueBox<ContentPayload>),
    /// A @counter-style system descriptor: a plain system keyword (kind 0, the C++ `enum class
    /// CounterStyleSystem : u8`, opaque to Rust), fixed with an optional retained first symbol
    /// (kind 1), or extends with a retained counter style name (kind 2).
    CounterStyleSystem(StyleValueBox<CounterStyleSystemPayload>),
    /// A counter style reference: either a retained counter style name, or a symbols() function
    /// with its type (a `css_enums::symbols_type` value) and retained symbol strings.
    CounterStyle(StyleValueBox<CounterStylePayload>),
    /// A color function such as rgb() or oklch(): three retained channel values, an optional
    /// retained alpha, an optional name and an optional retained origin color for relative
    /// color syntax.
    ColorFunction(StyleValueBox<ColorFunctionPayload>),
    /// color-mix() with its optional retained interpolation method value and its mix items:
    /// one or more retained colors, each with an optional retained percentage. The two lists
    /// have the same length, and an omitted percentage is a null entry.
    ColorMix(StyleValueBox<ColorMixPayload>),
    /// The shared data of every color style value: an optional color type and the color syntax
    /// (both C++ enums on ColorStyleValue, opaque to Rust).
    /// linear-gradient(): a direction (either a retained angle value or a side-or-corner
    /// keyword), the retained color stops, the gradient type, the repeating flag, an optional
    /// retained interpolation method and the color syntax. Enums are C++ types, opaque to Rust.
    LinearGradient(StyleValueBox<LinearGradientPayload>),
    /// conic-gradient(): an optional retained from-angle, the retained position, the retained
    /// color stops, the repeating flag, an optional retained interpolation method and the color
    /// syntax (a C++ enum, opaque to Rust).
    ConicGradient(StyleValueBox<ConicGradientPayload>),
    /// radial-gradient(): the ending shape (a C++ enum, opaque to Rust), the retained size and
    /// position values, the retained color stops, the repeating flag, an optional retained
    /// interpolation method and the color syntax.
    RadialGradient(StyleValueBox<RadialGradientPayload>),
    /// A url() image. The resource context is snapshotted into computed images so a later C++
    /// wrapper can remain a lazy consumer adapter. Loading state stays on the C++ side.
    Image(StyleValueBox<ImagePayload>),
    /// image-set() with its retained options.
    ImageSet(StyleValueBox<ImageSetPayload>),
    /// An easing function: linear() with its retained stops (kind 0), cubic-bezier() with four
    /// retained control values (kind 1), or steps() with a retained interval count and a step
    /// position (kind 2, the C++ `enum class StepPosition : u8`, opaque to Rust).
    Easing(StyleValueBox<EasingPayload>),
    /// A cursor with its retained image value and optional retained hotspot coordinates (both
    /// null or both non-null).
    Cursor(StyleValueBox<CursorPayload>),
    /// A grid track size list: the subgrid and preserve-line-name-sets flags and the retained
    /// track entries.
    GridTrackSizeList(StyleValueBox<GridTrackSizeListPayload>),
    /// grid-template-areas with its retained named areas and the row and column counts.
    GridTemplateArea(StyleValueBox<GridTemplateAreaPayload>),
    /// counter-increment, counter-reset or counter-set with its retained counter definitions.
    CounterDefinitions(StyleValueBox<CounterDefinitionsPayload>),
    /// A grid-row/grid-column placement: auto (0), a span (1) or an area/line (2), with an
    /// optional retained line value and an optional retained name.
    GridTrackPlacement(StyleValueBox<GridTrackPlacementPayload>),
    /// counter() or counters(). The function is `COUNTER_FUNCTION_COUNTER` or
    /// `COUNTER_FUNCTION_COUNTERS`; the join string is empty for counter().
    Counter(StyleValueBox<CounterPayload>),
    /// light-dark() with its two retained color style values.
    LightDark(StyleValueBox<LightDarkPayload>),
    /// random-value-sharing: an optional retained fixed value (null when absent), the auto flag,
    /// an optional name and the element-shared flag.
    RandomValueSharing(StyleValueBox<RandomValueSharingPayload>),
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
        separator: u8,
        collapsible: bool,
        values: RetainedStyleValueDataList,
    },
    /// A tuple of optional style values (null entries represent absent optionals).
    Tuple { values: RetainedStyleValueDataList },
    /// A display value: the raw bytes of the C++ Display value type (a tag plus a union of
    /// packed u8 enums), opaque to Rust.
    Display { raw: u32 },
    /// color-scheme with its retained scheme names and the only keyword flag.
    ColorScheme(StyleValueBox<ColorSchemePayload>),
    /// An unresolved value with retained source text and components, flags for substitution functions,
    /// attr taint, and an optional parsed value cached for an attr()-tainted registered custom property.
    Unresolved(UnresolvedBox),
    /// A CSS url() or src() with its retained URL string, type (the C++ URL::Type, opaque to
    /// Rust) and request URL modifiers.
    Url(StyleValueBox<UrlPayload>),
    /// A @font-face source: either local() with a retained family name value, or a URL (encoded
    /// as in [`StyleValueData::Url`]) with an optional format string and a list of font
    /// technologies (C++ `enum class FontTech : u8`, opaque to Rust).
    FontSource(StyleValueBox<FontSourcePayload>),
    /// A radial gradient size: one or two components, each either a RadialExtent keyword (the
    /// C++ `enum class RadialExtent : u8`, opaque to Rust) or a retained style value.
    RadialSize(StyleValueBox<RadialSizePayload>),
    /// A transform function with its argument values. The property (PropertyID : u16) and
    /// function are C++ enums, opaque to Rust.
    Transformation(StyleValueBox<TransformationPayload>),
    /// A shorthand property value: the shorthand id, its longhand ids (both C++
    /// `enum class PropertyID : u16`, opaque to Rust) and their values.
    Shorthand(StyleValueBox<ShorthandPayload>),
    /// A CSS `<custom-ident>`.
    CustomIdent { custom_ident: CssString },
    /// A border-radius rect of four retained corner radius data allocations.
    BorderRadiusRect(StyleValueBox<BorderRadiusRectPayload>),
    /// A single corner radius: the horizontal and vertical radii and whether they differ.
    BorderRadius(StyleValueBox<BorderRadiusPayload>),
    /// A filter function. Kinds: blur (0, value = radius), drop-shadow (1, value = shadow),
    /// hue-rotate (2, value = angle), color (3, value = amount, with the color operation).
    /// The kind and the color operation are C++ enum values, opaque to Rust.
    Filter {
        kind: u8,
        color_operation: u8,
        value: RetainedStyleValueData,
    },
    /// https://drafts.csswg.org/motion-1/#ray-function
    /// A ray() offset path: its angle, its <ray-size> keyword, whether it has `contain`, and its
    /// optional `at <position>` (null when absent).
    Ray(StyleValueBox<RayPayload>),
}

/// What a [`StyleValueData::Calculated`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct CalculatedPayload {
    /// The resolve-against target, base-mapped at creation: whether one
    /// exists, whether it is the number type, and otherwise its base type
    /// index in the numeric type order.
    pub resolve_as_is_number: bool,
    pub resolve_as_base: u8,
    pub has_percentages_resolve_as: bool,
    pub percentages_resolve_as: u8,
    pub resolve_numbers_as_integers: bool,
    pub rust_calculation: crate::css::calc::CalcNodeHandle,
    pub resolved_type: crate::css::calc::FfiNumericType,
    pub accepted_ranges: RetainedNumericRangeList,
}

/// What a [`StyleValueData::Ratio`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RatioPayload {
    pub numerator: RetainedStyleValueData,
    pub denominator: RetainedStyleValueData,
}

/// What a [`StyleValueData::ScrollbarColor`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ScrollbarColorPayload {
    pub thumb_color: RetainedStyleValueData,
    pub track_color: RetainedStyleValueData,
}

/// What a [`StyleValueData::Rect`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RectPayload {
    pub top: RetainedStyleValueData,
    pub right: RetainedStyleValueData,
    pub bottom: RetainedStyleValueData,
    pub left: RetainedStyleValueData,
}

/// What a [`StyleValueData::Function`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct FunctionPayload {
    pub name: CssString,
    pub value: RetainedStyleValueData,
}

/// What a [`StyleValueData::OpenTypeTagged`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct OpenTypeTaggedPayload {
    pub mode: u8,
    pub tag_name: CssString,
    pub packed_tag: u32,
    pub value: RetainedStyleValueData,
}

/// What a [`StyleValueData::BackgroundSize`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct BackgroundSizePayload {
    pub size_x: RetainedStyleValueData,
    pub size_y: RetainedStyleValueData,
}

/// What a [`StyleValueData::BorderImageSlice`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct BorderImageSlicePayload {
    pub top: RetainedStyleValueData,
    pub right: RetainedStyleValueData,
    pub bottom: RetainedStyleValueData,
    pub left: RetainedStyleValueData,
    pub fill: bool,
}

/// What a [`StyleValueData::AnchorSize`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct AnchorSizePayload {
    pub has_anchor_name: bool,
    pub anchor_name: CssString,
    pub has_anchor_size: bool,
    pub anchor_size: u8,
    pub fallback_value: RetainedStyleValueData,
}

/// What a [`StyleValueData::Anchor`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct AnchorPayload {
    pub has_anchor_name: bool,
    pub anchor_name: CssString,
    pub anchor_side: RetainedStyleValueData,
    pub fallback_value: RetainedStyleValueData,
}

/// What a [`StyleValueData::Position`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct PositionPayload {
    pub edge_x: RetainedStyleValueData,
    pub edge_y: RetainedStyleValueData,
}

/// What a [`StyleValueData::Shadow`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ShadowPayload {
    pub shadow_type: u8,
    pub color: RetainedStyleValueData,
    pub offset_x: RetainedStyleValueData,
    pub offset_y: RetainedStyleValueData,
    pub blur_radius: RetainedStyleValueData,
    pub spread_distance: RetainedStyleValueData,
    pub placement: u8,
}

/// What a [`StyleValueData::Content`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ContentPayload {
    pub content: RetainedStyleValueData,
    pub alt_text: RetainedStyleValueData,
}

/// What a [`StyleValueData::CounterStyleSystem`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct CounterStyleSystemPayload {
    pub kind: u8,
    pub system: u8,
    pub first_symbol: RetainedStyleValueData,
    pub name: CssString,
}

/// What a [`StyleValueData::CounterStyle`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct CounterStylePayload {
    pub is_symbols: bool,
    pub name: CssString,
    pub symbols_type: u8,
    pub symbols: CssStringList,
}

/// What a [`StyleValueData::ColorFunction`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ColorFunctionPayload {
    pub color_base: ColorBase,
    pub channel_0: RetainedStyleValueData,
    pub channel_1: RetainedStyleValueData,
    pub channel_2: RetainedStyleValueData,
    pub alpha: RetainedStyleValueData,
    pub has_name: bool,
    pub name: CssString,
    pub origin_color: RetainedStyleValueData,
}

/// What a [`StyleValueData::ColorMix`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ColorMixPayload {
    pub color_base: ColorBase,
    pub color_interpolation_method: RetainedStyleValueData,
    pub colors: RetainedStyleValueDataList,
    pub percentages: RetainedStyleValueDataList,
}

/// What a [`StyleValueData::LinearGradient`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct LinearGradientPayload {
    pub has_direction_value: bool,
    pub direction_value: RetainedStyleValueData,
    pub side_or_corner: u8,
    pub color_stop_list: RetainedColorStopList,
    pub gradient_type: u8,
    pub repeating: bool,
    pub color_interpolation_method: RetainedStyleValueData,
    pub color_syntax: u8,
}

/// What a [`StyleValueData::ConicGradient`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ConicGradientPayload {
    pub from_angle: RetainedStyleValueData,
    pub position: RetainedStyleValueData,
    pub color_stop_list: RetainedColorStopList,
    pub repeating: bool,
    pub color_interpolation_method: RetainedStyleValueData,
    pub color_syntax: u8,
}

/// What a [`StyleValueData::RadialGradient`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RadialGradientPayload {
    pub ending_shape: u8,
    pub size: RetainedStyleValueData,
    pub position: RetainedStyleValueData,
    pub color_stop_list: RetainedColorStopList,
    pub repeating: bool,
    pub color_interpolation_method: RetainedStyleValueData,
    pub color_syntax: u8,
}

/// What a [`StyleValueData::Image`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ImagePayload {
    pub url: RetainedString,
    pub url_type: u8,
    pub url_modifiers: RetainedRequestUrlModifierList,
    pub resource_context: ImageResourceContext,
}

/// What a [`StyleValueData::ImageSet`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ImageSetPayload {
    pub options: RetainedImageSetOptionList,
}

/// What a [`StyleValueData::Easing`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct EasingPayload {
    pub kind: u8,
    pub step_position: u8,
    pub is_keyword: bool,
    pub linear_stops: RetainedLinearEasingStopList,
    pub x1: RetainedStyleValueData,
    pub y1: RetainedStyleValueData,
    pub x2: RetainedStyleValueData,
    pub y2: RetainedStyleValueData,
    pub number_of_intervals: RetainedStyleValueData,
}

/// What a [`StyleValueData::Cursor`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct CursorPayload {
    pub image: RetainedStyleValueData,
    pub x: RetainedStyleValueData,
    pub y: RetainedStyleValueData,
}

/// What a [`StyleValueData::GridTrackSizeList`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct GridTrackSizeListPayload {
    pub is_subgrid: bool,
    pub preserve_line_name_sets: bool,
    pub entries: RetainedGridTrackEntryList,
}

/// What a [`StyleValueData::GridTemplateArea`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct GridTemplateAreaPayload {
    pub grid_areas: RetainedGridAreaList,
    pub row_count: usize,
    pub column_count: usize,
}

/// What a [`StyleValueData::CounterDefinitions`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct CounterDefinitionsPayload {
    pub counter_definitions: RetainedCounterDefinitionList,
}

/// What a [`StyleValueData::GridTrackPlacement`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct GridTrackPlacementPayload {
    pub kind: u8,
    pub value: RetainedStyleValueData,
    pub has_name: bool,
    pub name: CssString,
    pub implicit_start_name: CssString,
    pub implicit_end_name: CssString,
}

/// What a [`StyleValueData::Counter`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct CounterPayload {
    pub function: u8,
    pub counter_name: CssString,
    pub counter_style: RetainedStyleValueData,
    pub join_string: CssString,
}

/// What a [`StyleValueData::LightDark`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct LightDarkPayload {
    pub color_base: ColorBase,
    pub light: RetainedStyleValueData,
    pub dark: RetainedStyleValueData,
}

/// What a [`StyleValueData::RandomValueSharing`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RandomValueSharingPayload {
    pub fixed_value: RetainedStyleValueData,
    pub is_auto: bool,
    pub has_name: bool,
    pub name: CssString,
    pub element_shared: bool,
}

/// What a [`StyleValueData::ColorScheme`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ColorSchemePayload {
    pub schemes: CssStringList,
    pub scheme_codes: RetainedByteList,
    pub only: bool,
}

/// What an unresolved value's text holds, found once when the value is made from the components it
/// tokenizes to: which substitution functions it uses (bits 0-5, in the order of
/// `SubstitutionFunctionsPresence`'s fields), whether it is a lone CSS-wide keyword (bit 6), whether
/// it calls a tree-counting function (bit 7), whether it references a custom property by name (bit
/// 8), whether a reference names its property by anything but a plain identifier (bit 9), and
/// whether the engine serialized the text from tokens, so that its comments only keep tokens apart
/// (bit 10).
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(transparent)]
pub struct UnresolvedFacts(u16);

const _: () = assert!(size_of::<UnresolvedFacts>() == 2);

impl UnresolvedFacts {
    const LONE_CSS_WIDE_KEYWORD: u16 = 1 << 6;
    const CALLS_TREE_COUNTING_FUNCTION: u16 = 1 << 7;
    const NAMES_CUSTOM_PROPERTIES: u16 = 1 << 8;
    const HIDES_A_REFERENCE: u16 = 1 << 9;
    const SERIALIZED_BY_THE_ENGINE: u16 = 1 << 10;

    /// The facts of a value whose text tokenizes to `values` and uses the substitution functions
    /// `presence` says, beside those of the custom properties it references.
    pub(crate) fn of(values: &[ComponentValue], presence: SubstitutionFunctionsPresence) -> Self {
        let presence_bits = [
            presence.attr,
            presence.dashed_function,
            presence.env,
            presence.if_,
            presence.inherit,
            presence.var,
        ]
        .iter()
        .enumerate()
        .fold(0_u16, |bits, (index, present)| bits | (u16::from(*present) << index));
        let mut bits = presence_bits;
        if crate::css::custom_properties::single_css_wide_keyword(values).is_some() {
            bits |= Self::LONE_CSS_WIDE_KEYWORD;
        }
        if crate::css::parser::value_parser::contains_tree_counting_function(values) {
            bits |= Self::CALLS_TREE_COUNTING_FUNCTION;
        }
        Self(bits)
    }

    fn with_references(self, references: &CustomPropertyReferences<'_>) -> Self {
        let mut bits = self.0 & !(Self::NAMES_CUSTOM_PROPERTIES | Self::HIDES_A_REFERENCE);
        if !references.names.is_empty() {
            bits |= Self::NAMES_CUSTOM_PROPERTIES;
        }
        if !references.all_visible {
            bits |= Self::HIDES_A_REFERENCE;
        }
        Self(bits)
    }

    /// Which substitution functions the text uses.
    pub(crate) fn presence(self) -> SubstitutionFunctionsPresence {
        let bit = |index: u16| self.0 & (1 << index) != 0;
        SubstitutionFunctionsPresence {
            attr: bit(0),
            dashed_function: bit(1),
            env: bit(2),
            if_: bit(3),
            inherit: bit(4),
            var: bit(5),
        }
    }

    /// Whether the text is one CSS-wide keyword, beside whitespace.
    pub(crate) fn is_lone_css_wide_keyword(self) -> bool {
        self.0 & Self::LONE_CSS_WIDE_KEYWORD != 0
    }

    /// Whether the text calls sibling-count() or sibling-index().
    pub(crate) fn calls_tree_counting_function(self) -> bool {
        self.0 & Self::CALLS_TREE_COUNTING_FUNCTION != 0
    }

    fn names_custom_properties(self) -> bool {
        self.0 & Self::NAMES_CUSTOM_PROPERTIES != 0
    }

    /// What a comment in the text tokenizes as.
    fn comments(self) -> crate::css::css_tokenizer::Comments {
        match self.0 & Self::SERIALIZED_BY_THE_ENGINE != 0 {
            true => crate::css::css_tokenizer::Comments::Separating,
            false => crate::css::css_tokenizer::Comments::AsWhitespace,
        }
    }
}

/// The custom properties a value's `var()` and `inherit()` functions name, as written, and whether
/// every one of them names its property with a plain identifier.
pub(crate) struct CustomPropertyReferences<'a> {
    pub(crate) names: smallvec::SmallVec<[&'a [u16]; 4]>,
    pub(crate) all_visible: bool,
}

impl<'a> CustomPropertyReferences<'a> {
    /// The references of a value whose text tokenizes to `values`.
    pub(crate) fn scan(values: &'a [ComponentValue]) -> Self {
        let mut names = smallvec::SmallVec::new();
        let all_visible = scan_custom_property_references(values, &mut names);
        Self { names, all_visible }
    }
}

/// What a [`StyleValueData::Unresolved`] holds: the start of one allocation its source text
/// follows, as one byte per unit where every unit is ASCII, else as UTF-16 code units. Where the
/// text references custom properties by name, the names follow the text, aligned to 4 bytes: their
/// count as a `u32`, then each name as its length in a `u32` and its UTF-16 code units, escapes
/// decoded, aligned to 4 bytes.
#[repr(C)]
pub struct UnresolvedPayload {
    pub parsed_value: RetainedStyleValueData,
    pub contains_attr_tainted_values: bool,
    units_are_latin1: bool,
    facts: UnresolvedFacts,
    length: u32,
}

const _: () = assert!(size_of::<UnresolvedPayload>() == 16);

impl UnresolvedPayload {
    /// The text the value was written as.
    pub(crate) fn source_text(&self) -> TokenizerInput<'_> {
        let length = self.length as usize;
        // SAFETY: The text follows the payload in its allocation, `length` units of the kind it
        //         says.
        unsafe {
            let text = std::ptr::from_ref(self).cast::<u8>().add(size_of::<Self>());
            if self.units_are_latin1 {
                TokenizerInput::Latin1(ak::slice_from_raw_parts(text, length))
            } else {
                TokenizerInput::Utf16(ak::slice_from_raw_parts(text.cast::<u16>(), length))
            }
        }
    }

    /// The components of the value, tokenized from its text for this read.
    pub(crate) fn components(&self) -> ComponentValueList {
        components_of_text(self.source_text(), self.facts.comments()).unwrap_or_default()
    }

    /// What the text holds.
    pub(crate) fn facts(&self) -> UnresolvedFacts {
        self.facts
    }

    /// Which substitution functions the text uses.
    pub(crate) fn presence(&self) -> SubstitutionFunctionsPresence {
        self.facts.presence()
    }

    /// The names of the custom properties the text references, as written, in the order it does.
    pub(crate) fn references(&self) -> impl Iterator<Item = &[u16]> {
        let (mut entry, mut remaining) = if self.facts.names_custom_properties() {
            // SAFETY: Where the text names custom properties, their count follows it, aligned, and
            //         the first name follows the count.
            unsafe {
                let count = self
                    .base()
                    .add(Self::table_offset(self.length as usize, self.units_are_latin1))
                    .cast::<u32>();
                (count.add(1), count.read())
            }
        } else {
            (std::ptr::null(), 0)
        };
        std::iter::from_fn(move || {
            if remaining == 0 {
                return None;
            }
            remaining -= 1;
            // SAFETY: Each name is its length followed by its units, aligned to 4 bytes; the next
            //         one follows it.
            unsafe {
                let length = entry.read() as usize;
                let units = entry.add(1).cast::<u16>();
                entry = units.cast::<u8>().add(Self::name_units_bytes(length)).cast::<u32>();
                Some(ak::slice_from_raw_parts(units, length))
            }
        })
    }

    /// Whether every custom property the text references is among [`Self::references()`]; one
    /// that is named by anything but a plain identifier is not.
    pub(crate) fn references_visible(&self) -> bool {
        self.facts.0 & UnresolvedFacts::HIDES_A_REFERENCE == 0
    }

    fn base(&self) -> *const u8 {
        std::ptr::from_ref(self).cast::<u8>()
    }

    fn text_end(length: usize, units_are_latin1: bool) -> usize {
        let unit = if units_are_latin1 { 1 } else { size_of::<u16>() };
        length
            .checked_mul(unit)
            .and_then(|bytes| bytes.checked_add(size_of::<Self>()))
            .expect("unresolved value text too long")
    }

    fn table_offset(length: usize, units_are_latin1: bool) -> usize {
        Self::text_end(length, units_are_latin1).next_multiple_of(align_of::<u32>())
    }

    fn name_units_bytes(length: usize) -> usize {
        (length * size_of::<u16>()).next_multiple_of(align_of::<u32>())
    }

    fn table_bytes<'a>(names: impl Iterator<Item = &'a [u16]>) -> usize {
        names.fold(size_of::<u32>(), |bytes, name| {
            bytes + size_of::<u32>() + Self::name_units_bytes(name.len())
        })
    }

    /// The bytes of the allocation the payload starts.
    fn allocation_size(&self) -> usize {
        let length = self.length as usize;
        if self.facts.names_custom_properties() {
            Self::table_offset(length, self.units_are_latin1) + Self::table_bytes(self.references())
        } else {
            Self::text_end(length, self.units_are_latin1)
        }
    }

    fn layout(bytes: usize) -> std::alloc::Layout {
        std::alloc::Layout::from_size_align(bytes, align_of::<Self>()).expect("unresolved value text too long")
    }
}

/// The components `text` tokenizes to, whose comments tokenize as `comments` says: as whitespace
/// in text an author wrote, as nothing in text the engine serialized from components with
/// [`ComponentSerializationMode::Retokenize`], which tokenizes back to them.
pub(crate) fn components_of_text(
    text: TokenizerInput<'_>,
    comments: crate::css::css_tokenizer::Comments,
) -> Result<ComponentValueList, ()> {
    crate::css::parser::component_value::consume_a_counted_list_of_component_values(
        crate::css::css_tokenizer::tokenize_for_parser_with_comments(text, comments),
    )
}

/// The box of an unresolved value's payload and text, laid out as one pointer C++ reads the
/// payload through.
#[repr(C)]
pub struct UnresolvedBox {
    pointer: std::ptr::NonNull<UnresolvedPayload>,
}

// SAFETY: The box owns its payload, whose fields are `Send + Sync`, and its immutable text.
unsafe impl Send for UnresolvedBox {}
unsafe impl Sync for UnresolvedBox {}

/// What an unresolved value is made of, which it does not keep: the components its text, as an
/// author wrote it, tokenizes to, or the components the engine serialized its text from with
/// [`ComponentSerializationMode::Retokenize`], which the text tokenizes back to with its comments
/// keeping tokens apart.
pub(crate) enum UnresolvedValueComponents<'a> {
    OfText(&'a [ComponentValue]),
    OfSerialization(&'a [ComponentValue]),
}

/// The fields of a payload its text and names, written first, say.
struct UnresolvedTextShape {
    units_are_latin1: bool,
    facts: UnresolvedFacts,
    length: u32,
}

impl UnresolvedBox {
    /// The payload of a value written as `source_text`, which need not be well-formed UTF-16, and
    /// made of `components`, which uses the substitution functions `presence` says.
    pub(crate) fn new(
        source_text: TokenizerInput<'_>,
        components: UnresolvedValueComponents<'_>,
        presence: SubstitutionFunctionsPresence,
        contains_attr_tainted_values: bool,
        parsed_value: RetainedStyleValueData,
    ) -> Self {
        let (values, serialized) = match components {
            UnresolvedValueComponents::OfText(values) => (values, false),
            UnresolvedValueComponents::OfSerialization(values) => (values, true),
        };
        let mut facts = UnresolvedFacts::of(values, presence);
        if serialized {
            facts.0 |= UnresolvedFacts::SERIALIZED_BY_THE_ENGINE;
        }
        let (base, shape) = Self::allocate(source_text, facts, &CustomPropertyReferences::scan(values));
        Self::finish(base, shape, contains_attr_tainted_values, parsed_value)
    }

    /// An allocation holding `source_text` and the names of `references`, whose payload is still
    /// to be written.
    fn allocate(
        source_text: TokenizerInput<'_>,
        facts: UnresolvedFacts,
        references: &CustomPropertyReferences<'_>,
    ) -> (std::ptr::NonNull<u8>, UnresolvedTextShape) {
        let units_are_latin1 = match source_text {
            TokenizerInput::Latin1(_) => true,
            TokenizerInput::Utf16(units) => units.iter().all(|unit| *unit <= 0x7f),
        };
        let length = source_text.len();
        let facts = facts.with_references(references);
        let table_offset = UnresolvedPayload::table_offset(length, units_are_latin1);
        let bytes = if facts.names_custom_properties() {
            table_offset + UnresolvedPayload::table_bytes(references.names.iter().copied())
        } else {
            UnresolvedPayload::text_end(length, units_are_latin1)
        };
        let layout = UnresolvedPayload::layout(bytes);
        // SAFETY: The layout is nonzero and covers the payload, its text and its names, each unit
        //         of which is written once.
        unsafe {
            let base = std::alloc::alloc(layout);
            if base.is_null() {
                std::alloc::handle_alloc_error(layout);
            }
            let text = base.add(size_of::<UnresolvedPayload>());
            match source_text {
                TokenizerInput::Latin1(bytes) => std::ptr::copy_nonoverlapping(bytes.as_ptr(), text, length),
                TokenizerInput::Utf16(units) if units_are_latin1 => {
                    for (index, unit) in units.iter().enumerate() {
                        text.add(index).write(*unit as u8);
                    }
                }
                TokenizerInput::Utf16(units) => {
                    std::ptr::copy_nonoverlapping(units.as_ptr(), text.cast::<u16>(), length);
                }
            }
            if facts.names_custom_properties() {
                // NB: Every name is part of the text, so neither the count nor a length can exceed
                //     the text's, which fits in a u32.
                let count = base.add(table_offset).cast::<u32>();
                count.write(references.names.len() as u32);
                let mut entry = count.add(1);
                for name in &references.names {
                    entry.write(name.len() as u32);
                    let units = entry.add(1).cast::<u16>();
                    std::ptr::copy_nonoverlapping(name.as_ptr(), units, name.len());
                    entry = units
                        .cast::<u8>()
                        .add(UnresolvedPayload::name_units_bytes(name.len()))
                        .cast::<u32>();
                }
            }
            let shape = UnresolvedTextShape {
                units_are_latin1,
                facts,
                length: u32::try_from(length).expect("unresolved value text too long"),
            };
            (std::ptr::NonNull::new_unchecked(base), shape)
        }
    }

    /// The box of the allocation `allocate` made, its payload written now.
    fn finish(
        base: std::ptr::NonNull<u8>,
        shape: UnresolvedTextShape,
        contains_attr_tainted_values: bool,
        parsed_value: RetainedStyleValueData,
    ) -> Self {
        let pointer = base.cast::<UnresolvedPayload>();
        // SAFETY: The allocation starts with room for the payload, written once here.
        unsafe {
            pointer.write(UnresolvedPayload {
                parsed_value,
                contains_attr_tainted_values,
                units_are_latin1: shape.units_are_latin1,
                facts: shape.facts,
                length: shape.length,
            });
        }
        Self { pointer }
    }

    /// This payload, written as `source_text`.
    fn copy_with_text(&self, source_text: TokenizerInput<'_>) -> Self {
        let references = CustomPropertyReferences {
            names: self.references().collect(),
            all_visible: self.references_visible(),
        };
        let (base, shape) = Self::allocate(source_text, self.facts, &references);
        Self::finish(
            base,
            shape,
            self.contains_attr_tainted_values,
            self.parsed_value.clone(),
        )
    }

    /// The payload, to change what it holds beside its text.
    pub(crate) fn payload_mut(&mut self) -> &mut UnresolvedPayload {
        // SAFETY: The box owns a live payload, which this borrow alone reaches.
        unsafe { self.pointer.as_mut() }
    }

    /// This payload, written as `source_text` instead.
    pub(crate) fn with_source_text(&self, source_text: TokenizerInput<'_>) -> Self {
        self.copy_with_text(source_text)
    }
}

impl std::ops::Deref for UnresolvedBox {
    type Target = UnresolvedPayload;

    fn deref(&self) -> &UnresolvedPayload {
        // SAFETY: The box owns a live payload.
        unsafe { self.pointer.as_ref() }
    }
}

impl Clone for UnresolvedBox {
    fn clone(&self) -> Self {
        self.copy_with_text(self.source_text())
    }
}

impl PartialEq for UnresolvedBox {
    fn eq(&self, other: &Self) -> bool {
        let same_text = match (self.source_text(), other.source_text()) {
            (TokenizerInput::Latin1(first), TokenizerInput::Latin1(second)) => first == second,
            (TokenizerInput::Utf16(first), TokenizerInput::Utf16(second)) => first == second,
            // NB: Text up to U+00FF may be kept as bytes or as UTF-16, whichever it arrived as.
            (TokenizerInput::Latin1(bytes), TokenizerInput::Utf16(units))
            | (TokenizerInput::Utf16(units), TokenizerInput::Latin1(bytes)) => {
                bytes.iter().copied().map(u16::from).eq(units.iter().copied())
            }
        };
        same_text
            && self.facts == other.facts
            && self.contains_attr_tainted_values == other.contains_attr_tainted_values
            && self.parsed_value == other.parsed_value
    }
}

impl Drop for UnresolvedBox {
    fn drop(&mut self) {
        let layout = UnresolvedPayload::layout(self.allocation_size());
        // SAFETY: The box owns the payload and the allocation `new` made.
        unsafe {
            std::ptr::drop_in_place(self.pointer.as_ptr());
            std::alloc::dealloc(self.pointer.as_ptr().cast(), layout);
        }
    }
}

/// What a [`StyleValueData::Url`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct UrlPayload {
    pub url: RetainedString,
    pub url_type: u8,
    pub modifiers: RetainedRequestUrlModifierList,
}

/// What a [`StyleValueData::FontSource`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct FontSourcePayload {
    pub is_local: bool,
    pub url_type: u8,
    pub has_format: bool,
    pub local_name: RetainedStyleValueData,
    pub url: RetainedString,
    pub url_modifiers: RetainedRequestUrlModifierList,
    pub format: CssString,
    pub tech: RetainedByteList,
}

/// What a [`StyleValueData::RadialSize`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RadialSizePayload {
    pub component_count: u8,
    pub is_extent_0: bool,
    pub extent_0: u8,
    pub value_0: RetainedStyleValueData,
    pub is_extent_1: bool,
    pub extent_1: u8,
    pub value_1: RetainedStyleValueData,
}

/// What a [`StyleValueData::Transformation`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct TransformationPayload {
    pub property: u16,
    pub transform_function: u8,
    pub values: RetainedStyleValueDataList,
    /// Matrix-only arguments preserving literal angle precision across canonicalization.
    /// Empty when the canonical arguments lower to the same matrix inputs.
    pub matrix_values: RetainedStyleValueDataList,
}

/// What a [`StyleValueData::Shorthand`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct ShorthandPayload {
    pub shorthand_property: u16,
    pub sub_properties: RetainedPropertyIdList,
    pub values: RetainedStyleValueDataList,
}

/// What a [`StyleValueData::BorderRadiusRect`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct BorderRadiusRectPayload {
    pub top_left: RetainedStyleValueData,
    pub top_right: RetainedStyleValueData,
    pub bottom_right: RetainedStyleValueData,
    pub bottom_left: RetainedStyleValueData,
}

/// What a [`StyleValueData::BorderRadius`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct BorderRadiusPayload {
    pub is_elliptical: bool,
    pub horizontal_radius: RetainedStyleValueData,
    pub vertical_radius: RetainedStyleValueData,
}

/// What a [`StyleValueData::Ray`] holds, boxed so that a style value stays small.
#[repr(C)]
#[derive(Clone, PartialEq)]
pub struct RayPayload {
    pub size: u16,
    pub contain: bool,
    pub angle: RetainedStyleValueData,
    pub position: RetainedStyleValueData,
}

// The kinds of a filter style value, which are C++ enum values.
pub(crate) const FILTER_KIND_BLUR: u8 = 0;
pub(crate) const FILTER_KIND_DROP_SHADOW: u8 = 1;
pub(crate) const FILTER_KIND_HUE_ROTATE: u8 = 2;
pub(crate) const FILTER_KIND_COLOR: u8 = 3;

impl StyleValueData {
    /// The shared prefix of every color variant.
    pub(crate) fn color_base(&self) -> Option<&ColorBase> {
        match self {
            Self::ContrastColor { color_base, .. } => Some(color_base),
            Self::ColorFunction(color_function) => Some(&color_function.color_base),
            Self::ColorMix(color_mix) => Some(&color_mix.color_base),
            Self::LightDark(light_dark) => Some(&light_dark.color_base),
            _ => None,
        }
    }

    /// The bytes of the box a variant too large to hold inline keeps its payload in.
    pub(crate) fn boxed_payload_bytes(&self) -> usize {
        match self {
            Self::Calculated(payload) => size_of_val::<CalculatedPayload>(payload),
            Self::Ratio(payload) => size_of_val::<RatioPayload>(payload),
            Self::ScrollbarColor(payload) => size_of_val::<ScrollbarColorPayload>(payload),
            Self::Rect(payload) => size_of_val::<RectPayload>(payload),
            Self::Function(payload) => size_of_val::<FunctionPayload>(payload),
            Self::OpenTypeTagged(payload) => size_of_val::<OpenTypeTaggedPayload>(payload),
            Self::BorderImageSlice(payload) => size_of_val::<BorderImageSlicePayload>(payload),
            Self::AnchorSize(payload) => size_of_val::<AnchorSizePayload>(payload),
            Self::Anchor(payload) => size_of_val::<AnchorPayload>(payload),
            Self::Position(payload) => size_of_val::<PositionPayload>(payload),
            Self::Shadow(payload) => size_of_val::<ShadowPayload>(payload),
            Self::Content(payload) => size_of_val::<ContentPayload>(payload),
            Self::CounterStyleSystem(payload) => size_of_val::<CounterStyleSystemPayload>(payload),
            Self::CounterStyle(payload) => size_of_val::<CounterStylePayload>(payload),
            Self::ColorFunction(payload) => size_of_val::<ColorFunctionPayload>(payload),
            Self::ColorMix(payload) => size_of_val::<ColorMixPayload>(payload),
            Self::LinearGradient(payload) => size_of_val::<LinearGradientPayload>(payload),
            Self::ConicGradient(payload) => size_of_val::<ConicGradientPayload>(payload),
            Self::RadialGradient(payload) => size_of_val::<RadialGradientPayload>(payload),
            Self::Image(payload) => size_of_val::<ImagePayload>(payload),
            Self::ImageSet(payload) => size_of_val::<ImageSetPayload>(payload),
            Self::Easing(payload) => size_of_val::<EasingPayload>(payload),
            Self::Cursor(payload) => size_of_val::<CursorPayload>(payload),
            Self::GridTrackSizeList(payload) => size_of_val::<GridTrackSizeListPayload>(payload),
            Self::GridTemplateArea(payload) => size_of_val::<GridTemplateAreaPayload>(payload),
            Self::CounterDefinitions(payload) => size_of_val::<CounterDefinitionsPayload>(payload),
            Self::GridTrackPlacement(payload) => size_of_val::<GridTrackPlacementPayload>(payload),
            Self::Counter(payload) => size_of_val::<CounterPayload>(payload),
            Self::LightDark(payload) => size_of_val::<LightDarkPayload>(payload),
            Self::RandomValueSharing(payload) => size_of_val::<RandomValueSharingPayload>(payload),
            Self::ColorScheme(payload) => size_of_val::<ColorSchemePayload>(payload),
            Self::Unresolved(payload) => size_of_val::<UnresolvedPayload>(payload),
            Self::Url(payload) => size_of_val::<UrlPayload>(payload),
            Self::FontSource(payload) => size_of_val::<FontSourcePayload>(payload),
            Self::RadialSize(payload) => size_of_val::<RadialSizePayload>(payload),
            Self::Transformation(payload) => size_of_val::<TransformationPayload>(payload),
            Self::Shorthand(payload) => size_of_val::<ShorthandPayload>(payload),
            Self::BorderRadiusRect(payload) => size_of_val::<BorderRadiusRectPayload>(payload),
            Self::BorderRadius(payload) => size_of_val::<BorderRadiusPayload>(payload),
            Self::Ray(payload) => size_of_val::<RayPayload>(payload),
            Self::BackgroundSize(payload) => size_of_val::<BackgroundSizePayload>(payload),
            _ => 0,
        }
    }

    /// The values a list, tuple, shorthand or transform function holds.
    pub(crate) fn child_values(&self) -> Option<&RetainedStyleValueDataList> {
        match self {
            Self::ValueList { values, .. } | Self::Tuple { values } => Some(values),
            Self::Shorthand(shorthand) => Some(&shorthand.values),
            Self::Transformation(transformation) => Some(&transformation.values),
            _ => None,
        }
    }

    pub(crate) fn as_counter_style_system(&self) -> Option<&CounterStyleSystemPayload> {
        match self {
            Self::CounterStyleSystem(system) => Some(system),
            _ => None,
        }
    }

    pub(crate) fn as_counter_style(&self) -> Option<&CounterStylePayload> {
        match self {
            Self::CounterStyle(counter_style) => Some(counter_style),
            _ => None,
        }
    }

    /// Whether an anchor() or anchor-size() names its anchor, the name, and its fallback value.
    pub(crate) fn anchor_name_and_fallback(&self) -> Option<(bool, &CssString, &RetainedStyleValueData)> {
        match self {
            Self::Anchor(anchor) => Some((anchor.has_anchor_name, &anchor.anchor_name, &anchor.fallback_value)),
            Self::AnchorSize(anchor_size) => Some((
                anchor_size.has_anchor_name,
                &anchor_size.anchor_name,
                &anchor_size.fallback_value,
            )),
            _ => None,
        }
    }
}

pub(crate) fn is_none_keyword(value: &StyleValueData) -> bool {
    matches!(value, StyleValueData::Keyword { keyword: code } if *code == crate::css::css_enums::keyword::NONE)
}

// Every value pays for the largest variant, so a variant that does not fit beside the tag in 16
// bytes holds its payload in a box, and a length takes 16 bytes.
const _: () = assert!(size_of::<StyleValueData>() <= 16);

/// One entry in a computed font-family list. A generic family carries its Keyword code;
/// a named family borrows the Rust string retained by the style value.
#[repr(C)]
pub struct FfiComputedFontFamilyEntry {
    pub kind: u8,
    pub keyword: u16,
    pub string: *const CssString,
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
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_computed_length_value(value: &StyleValueData) -> f64 {
    match value {
        StyleValueData::Length { value, .. } => *value,
        _ => unreachable!("computed value must be a length"),
    }
}

/// Reads the primitive payload of a computed number.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_computed_number(value: &StyleValueData) -> f64 {
    match value {
        StyleValueData::Number { value } => *value,
        _ => unreachable!("computed value must be a number"),
    }
}

/// Reads the primitive payload of a computed percentage.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_computed_percentage(value: &StyleValueData) -> f64 {
    match value {
        StyleValueData::Percentage { value } => *value,
        _ => unreachable!("computed value must be a percentage"),
    }
}

/// Reads a computed `font-size-adjust` that scales the size fonts are selected at: true, with the
/// metric (a `font_size_adjust_metric` value) and the number, for one naming a number, and false
/// for `none` and `from-font`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_computed_font_size_adjust_number(
    value: &StyleValueData,
    metric: &mut u8,
    number: &mut f64,
) -> bool {
    let (kind, value_metric, value_number) = crate::css::style::engine_sample::font_size_adjust_of(Some(value));
    if kind != crate::css::style::bridge::FontSizeAdjustKind::Number {
        return false;
    }
    *metric = value_metric;
    *number = value_number;
    true
}

impl StyleValueData {
    pub(crate) fn basic_shape(&self) -> Option<&BasicShapeData> {
        match self {
            Self::BasicShape { shape } => Some(shape.as_ref()),
            _ => None,
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
    /// Opaque payloads (calc trees and path geometry) contribute nothing beyond the variant
    /// discriminant; deep equality settles whatever shares a bucket. Floats hash by bit
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
        fn write_color_stops(hasher: &mut crate::css::style::fast_hash::FastHasher, stops: &RetainedColorStopList) {
            hasher.write_usize(stops.as_slice().len());
            for stop in stops.as_slice() {
                for value in stop.values() {
                    write_value(hasher, value);
                }
            }
        }
        fn write_grid_track_entries(
            hasher: &mut crate::css::style::fast_hash::FastHasher,
            entries: &[RetainedGridTrackEntry],
        ) {
            hasher.write_usize(entries.len());
            for entry in entries {
                hasher.write_u8(entry.kind as u8);
                hasher.write_usize(entry.names.as_slice().len());
                for name in entry.names.as_slice() {
                    write_fly(hasher, name);
                }
                write_value(hasher, &entry.size_value);
                write_value(hasher, &entry.min_value);
                write_value(hasher, &entry.max_value);
                hasher.write_u8(entry.repeat_type);
                write_value(hasher, &entry.repeat_count);
                write_bool(hasher, entry.repeat_is_subgrid);
                write_bool(hasher, entry.repeat_preserve_line_name_sets);
                write_grid_track_entries(hasher, entry.repeat_entries.as_slice());
            }
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
            Self::BasicShape { shape } => {
                let BasicShapeData {
                    kind,
                    v0,
                    v1,
                    v2,
                    v3,
                    v4,
                    fill_rule,
                    points: _,
                    path,
                    commands,
                } = shape.as_ref();
                hasher.write_u8(*kind);
                hasher.write_usize(commands.as_slice().len());
                write_value(hasher, v0);
                write_value(hasher, v1);
                write_value(hasher, v2);
                write_value(hasher, v3);
                write_value(hasher, v4);
                hasher.write_u8(*fill_rule);
                path.write_content_hash(hasher);
            }
            StyleValueData::Calculated(calculated_payload) => {
                let CalculatedPayload {
                    rust_calculation: _,
                    resolve_as_is_number,
                    resolve_as_base,
                    resolved_type: _,
                    has_percentages_resolve_as,
                    percentages_resolve_as,
                    resolve_numbers_as_integers,
                    accepted_ranges: _,
                } = &**calculated_payload;
                write_bool(hasher, *resolve_as_is_number);
                hasher.write_u8(*resolve_as_base);
                write_bool(hasher, *has_percentages_resolve_as);
                hasher.write_u8(*percentages_resolve_as);
                write_bool(hasher, *resolve_numbers_as_integers);
            }
            StyleValueData::Ratio(ratio_payload) => {
                let RatioPayload { numerator, denominator } = &**ratio_payload;
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
                ..
            } => write_value(hasher, original_shorthand_value),
            StyleValueData::ScrollbarColor(scrollbar_color_payload) => {
                let ScrollbarColorPayload {
                    thumb_color,
                    track_color,
                } = &**scrollbar_color_payload;
                write_value(hasher, thumb_color);
                write_value(hasher, track_color);
            }
            StyleValueData::Rect(rect_payload) => {
                let RectPayload {
                    top,
                    right,
                    bottom,
                    left,
                } = &**rect_payload;
                write_value(hasher, top);
                write_value(hasher, right);
                write_value(hasher, bottom);
                write_value(hasher, left);
            }
            Self::String { string, .. } => write_fly(hasher, string),
            StyleValueData::Function(function_payload) => {
                let FunctionPayload { name, value } = &**function_payload;
                write_fly(hasher, name);
                write_value(hasher, value);
            }
            StyleValueData::OpenTypeTagged(open_type_tagged_payload) => {
                let OpenTypeTaggedPayload {
                    mode,
                    tag_name: tag,
                    packed_tag,
                    value,
                } = &**open_type_tagged_payload;
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
            StyleValueData::BackgroundSize(background_size_payload) => {
                let BackgroundSizePayload { size_x, size_y } = &**background_size_payload;
                write_value(hasher, size_x);
                write_value(hasher, size_y);
            }
            Self::RepeatStyle { repeat_x, repeat_y } => {
                hasher.write_u8(*repeat_x);
                hasher.write_u8(*repeat_y);
            }
            StyleValueData::BorderImageSlice(border_image_slice_payload) => {
                let BorderImageSlicePayload {
                    top,
                    right,
                    bottom,
                    left,
                    fill,
                } = &**border_image_slice_payload;
                write_value(hasher, top);
                write_value(hasher, right);
                write_value(hasher, bottom);
                write_value(hasher, left);
                write_bool(hasher, *fill);
            }
            StyleValueData::AnchorSize(anchor_size_payload) => {
                let AnchorSizePayload {
                    has_anchor_name,
                    anchor_name,
                    has_anchor_size,
                    anchor_size,
                    fallback_value,
                } = &**anchor_size_payload;
                write_bool(hasher, *has_anchor_name);
                write_fly(hasher, anchor_name);
                write_bool(hasher, *has_anchor_size);
                hasher.write_u8(*anchor_size);
                write_value(hasher, fallback_value);
            }
            StyleValueData::Anchor(anchor_payload) => {
                let AnchorPayload {
                    has_anchor_name,
                    anchor_name,
                    anchor_side,
                    fallback_value,
                } = &**anchor_payload;
                write_bool(hasher, *has_anchor_name);
                write_fly(hasher, anchor_name);
                write_value(hasher, anchor_side);
                write_value(hasher, fallback_value);
            }
            StyleValueData::Position(position_payload) => {
                let PositionPayload { edge_x, edge_y } = &**position_payload;
                write_value(hasher, edge_x);
                write_value(hasher, edge_y);
            }
            StyleValueData::Shadow(shadow_payload) => {
                let ShadowPayload {
                    shadow_type,
                    color,
                    offset_x,
                    offset_y,
                    blur_radius,
                    spread_distance,
                    placement,
                } = &**shadow_payload;
                hasher.write_u8(*shadow_type);
                write_value(hasher, color);
                write_value(hasher, offset_x);
                write_value(hasher, offset_y);
                write_value(hasher, blur_radius);
                write_value(hasher, spread_distance);
                hasher.write_u8(*placement);
            }
            StyleValueData::Content(content_payload) => {
                let ContentPayload { content, alt_text } = &**content_payload;
                write_value(hasher, content);
                write_value(hasher, alt_text);
            }
            StyleValueData::CounterStyleSystem(counter_style_system_payload) => {
                let CounterStyleSystemPayload {
                    kind,
                    system,
                    first_symbol,
                    name,
                } = &**counter_style_system_payload;
                hasher.write_u8(*kind);
                hasher.write_u8(*system);
                write_value(hasher, first_symbol);
                write_fly(hasher, name);
            }
            StyleValueData::CounterStyle(counter_style_payload) => {
                let CounterStylePayload {
                    is_symbols,
                    name,
                    symbols_type,
                    symbols,
                } = &**counter_style_payload;
                write_bool(hasher, *is_symbols);
                write_fly(hasher, name);
                hasher.write_u8(*symbols_type);
                for string in symbols.as_slice() {
                    write_fly(hasher, string);
                }
            }
            StyleValueData::ColorFunction(color_function_payload) => {
                let ColorFunctionPayload {
                    color_base,
                    channel_0,
                    channel_1,
                    channel_2,
                    alpha,
                    has_name,
                    name,
                    origin_color,
                } = &**color_function_payload;
                // NB: Equality ignores the color syntax, and the color type and name when absent.
                write_bool(hasher, color_base.has_color_type);
                if color_base.has_color_type {
                    hasher.write_u8(color_base.color_type);
                }
                write_value(hasher, channel_0);
                write_value(hasher, channel_1);
                write_value(hasher, channel_2);
                write_value(hasher, alpha);
                write_bool(hasher, *has_name);
                if *has_name {
                    write_fly(hasher, name);
                }
                write_value(hasher, origin_color);
            }
            StyleValueData::ColorMix(color_mix_payload) => {
                let ColorMixPayload {
                    color_base,
                    color_interpolation_method,
                    colors,
                    percentages,
                } = &**color_mix_payload;
                write_color_base(hasher, color_base);
                write_value(hasher, color_interpolation_method);
                write_values(hasher, colors);
                write_values(hasher, percentages);
            }
            StyleValueData::LinearGradient(linear_gradient_payload) => {
                let LinearGradientPayload {
                    has_direction_value,
                    direction_value,
                    side_or_corner,
                    color_stop_list,
                    gradient_type,
                    repeating,
                    color_interpolation_method,
                    color_syntax,
                } = &**linear_gradient_payload;
                write_bool(hasher, *has_direction_value);
                write_value(hasher, direction_value);
                hasher.write_u8(*side_or_corner);
                write_color_stops(hasher, color_stop_list);
                hasher.write_u8(*gradient_type);
                write_bool(hasher, *repeating);
                write_value(hasher, color_interpolation_method);
                hasher.write_u8(*color_syntax);
            }
            StyleValueData::ConicGradient(conic_gradient_payload) => {
                let ConicGradientPayload {
                    from_angle,
                    position,
                    color_stop_list,
                    repeating,
                    color_interpolation_method,
                    color_syntax,
                } = &**conic_gradient_payload;
                write_value(hasher, from_angle);
                write_value(hasher, position);
                write_color_stops(hasher, color_stop_list);
                write_bool(hasher, *repeating);
                write_value(hasher, color_interpolation_method);
                hasher.write_u8(*color_syntax);
            }
            StyleValueData::RadialGradient(radial_gradient_payload) => {
                let RadialGradientPayload {
                    ending_shape,
                    size,
                    position,
                    color_stop_list,
                    repeating,
                    color_interpolation_method,
                    color_syntax,
                } = &**radial_gradient_payload;
                hasher.write_u8(*ending_shape);
                write_value(hasher, size);
                write_value(hasher, position);
                write_color_stops(hasher, color_stop_list);
                write_bool(hasher, *repeating);
                write_value(hasher, color_interpolation_method);
                hasher.write_u8(*color_syntax);
            }
            StyleValueData::Image(image_payload) => {
                let ImagePayload { url, url_type, .. } = &**image_payload;
                url.hash(hasher);
                hasher.write_u8(*url_type);
            }
            StyleValueData::ImageSet(image_set_payload) => {
                let ImageSetPayload { options } = &**image_set_payload;
                hasher.write_usize(options.as_slice().len());
                for option in options.as_slice() {
                    write_value(hasher, &option.image);
                    write_value(hasher, &option.resolution);
                    write_bool(hasher, option.has_type);
                    write_fly(hasher, &option.type_string);
                }
            }
            StyleValueData::Easing(easing_payload) => {
                let EasingPayload {
                    kind,
                    linear_stops,
                    x1,
                    y1,
                    x2,
                    y2,
                    number_of_intervals,
                    step_position,
                    is_keyword,
                } = &**easing_payload;
                hasher.write_u8(*kind);
                write_bool(hasher, *is_keyword);
                hasher.write_usize(linear_stops.as_slice().len());
                for stop in linear_stops.as_slice() {
                    write_value(hasher, &stop.output);
                    write_value(hasher, &stop.input);
                }
                write_value(hasher, x1);
                write_value(hasher, y1);
                write_value(hasher, x2);
                write_value(hasher, y2);
                write_value(hasher, number_of_intervals);
                hasher.write_u8(*step_position);
            }
            StyleValueData::Cursor(cursor_payload) => {
                let CursorPayload { image, x, y } = &**cursor_payload;
                write_value(hasher, image);
                write_value(hasher, x);
                write_value(hasher, y);
            }
            StyleValueData::GridTrackSizeList(grid_track_size_list_payload) => {
                let GridTrackSizeListPayload {
                    is_subgrid,
                    preserve_line_name_sets,
                    entries,
                } = &**grid_track_size_list_payload;
                write_bool(hasher, *is_subgrid);
                write_bool(hasher, *preserve_line_name_sets);
                write_grid_track_entries(hasher, entries.as_slice());
            }
            StyleValueData::GridTemplateArea(grid_template_area_payload) => {
                let GridTemplateAreaPayload {
                    grid_areas,
                    row_count,
                    column_count,
                } = &**grid_template_area_payload;
                hasher.write_usize(grid_areas.as_slice().len());
                for area in grid_areas.as_slice() {
                    write_fly(hasher, &area.name);
                    write_fly(hasher, &area.implicit_start_name);
                    write_fly(hasher, &area.implicit_end_name);
                    for line in area.grid_lines() {
                        hasher.write_usize(line);
                    }
                }
                hasher.write_usize(*row_count);
                hasher.write_usize(*column_count);
            }
            StyleValueData::CounterDefinitions(counter_definitions_payload) => {
                let CounterDefinitionsPayload { counter_definitions } = &**counter_definitions_payload;
                hasher.write_usize(counter_definitions.as_slice().len());
                for definition in counter_definitions.as_slice() {
                    write_fly(hasher, &definition.name);
                    write_bool(hasher, definition.is_reversed);
                    write_value(hasher, &definition.value);
                }
            }
            StyleValueData::GridTrackPlacement(grid_track_placement_payload) => {
                let GridTrackPlacementPayload {
                    kind,
                    value,
                    has_name,
                    name,
                    implicit_start_name: _,
                    implicit_end_name: _,
                } = &**grid_track_placement_payload;
                hasher.write_u8(*kind);
                write_value(hasher, value);
                write_bool(hasher, *has_name);
                write_fly(hasher, name);
            }
            StyleValueData::Counter(counter_payload) => {
                let CounterPayload {
                    function,
                    counter_name,
                    counter_style,
                    join_string,
                } = &**counter_payload;
                hasher.write_u8(*function);
                write_fly(hasher, counter_name);
                write_value(hasher, counter_style);
                write_fly(hasher, join_string);
            }
            StyleValueData::LightDark(light_dark_payload) => {
                let LightDarkPayload {
                    color_base,
                    light,
                    dark,
                } = &**light_dark_payload;
                write_color_base(hasher, color_base);
                write_value(hasher, light);
                write_value(hasher, dark);
            }
            StyleValueData::RandomValueSharing(random_value_sharing_payload) => {
                let RandomValueSharingPayload {
                    fixed_value,
                    is_auto,
                    has_name,
                    name,
                    element_shared,
                } = &**random_value_sharing_payload;
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
            StyleValueData::ColorScheme(color_scheme_payload) => {
                let ColorSchemePayload {
                    schemes,
                    scheme_codes,
                    only,
                } = &**color_scheme_payload;
                for string in schemes.as_slice() {
                    write_fly(hasher, string);
                }
                hasher.write(scheme_codes.as_slice());
                write_bool(hasher, *only);
            }
            StyleValueData::Unresolved(unresolved_payload) => {
                let unresolved_payload: &UnresolvedPayload = unresolved_payload;
                let UnresolvedPayload {
                    contains_attr_tainted_values,
                    ..
                } = unresolved_payload;
                let source_text = unresolved_payload.source_text();
                let presence_attr = &unresolved_payload.presence().attr;
                let presence_dashed_function = &unresolved_payload.presence().dashed_function;
                let presence_env = &unresolved_payload.presence().env;
                let presence_if = &unresolved_payload.presence().if_;
                let presence_inherit = &unresolved_payload.presence().inherit;
                let presence_var = &unresolved_payload.presence().var;
                hash_string_units(hasher, source_text);
                write_bool(hasher, *presence_attr);
                write_bool(hasher, *presence_dashed_function);
                write_bool(hasher, *presence_env);
                write_bool(hasher, *presence_if);
                write_bool(hasher, *presence_inherit);
                write_bool(hasher, *presence_var);
                write_bool(hasher, *contains_attr_tainted_values);
            }
            StyleValueData::Url(url_payload) => {
                let UrlPayload {
                    url,
                    url_type,
                    modifiers: _,
                } = &**url_payload;
                url.hash(hasher);
                hasher.write_u8(*url_type);
            }
            StyleValueData::FontSource(font_source_payload) => {
                let FontSourcePayload {
                    is_local,
                    local_name,
                    url,
                    url_type,
                    url_modifiers: _,
                    has_format,
                    format,
                    tech,
                } = &**font_source_payload;
                write_bool(hasher, *is_local);
                write_value(hasher, local_name);
                url.hash(hasher);
                hasher.write_u8(*url_type);
                write_bool(hasher, *has_format);
                write_fly(hasher, format);
                hasher.write(tech.as_slice());
            }
            StyleValueData::RadialSize(radial_size_payload) => {
                let RadialSizePayload {
                    component_count,
                    is_extent_0,
                    extent_0,
                    value_0,
                    is_extent_1,
                    extent_1,
                    value_1,
                } = &**radial_size_payload;
                hasher.write_u8(*component_count);
                write_bool(hasher, *is_extent_0);
                hasher.write_u8(*extent_0);
                write_value(hasher, value_0);
                write_bool(hasher, *is_extent_1);
                hasher.write_u8(*extent_1);
                write_value(hasher, value_1);
            }
            StyleValueData::Transformation(transformation_payload) => {
                let TransformationPayload {
                    property,
                    transform_function,
                    values,
                    matrix_values,
                } = &**transformation_payload;
                hasher.write_u16(*property);
                hasher.write_u8(*transform_function);
                write_values(hasher, values);
                write_values(hasher, matrix_values);
            }
            StyleValueData::Shorthand(shorthand_payload) => {
                let ShorthandPayload {
                    shorthand_property,
                    sub_properties,
                    values,
                } = &**shorthand_payload;
                hasher.write_u16(*shorthand_property);
                for id in sub_properties.as_slice() {
                    hasher.write_u16(*id);
                }
                write_values(hasher, values);
            }
            Self::CustomIdent { custom_ident } => write_fly(hasher, custom_ident),
            StyleValueData::BorderRadiusRect(border_radius_rect_payload) => {
                let BorderRadiusRectPayload {
                    top_left,
                    top_right,
                    bottom_right,
                    bottom_left,
                } = &**border_radius_rect_payload;
                write_value(hasher, top_left);
                write_value(hasher, top_right);
                write_value(hasher, bottom_right);
                write_value(hasher, bottom_left);
            }
            StyleValueData::BorderRadius(border_radius_payload) => {
                let BorderRadiusPayload {
                    is_elliptical,
                    horizontal_radius,
                    vertical_radius,
                } = &**border_radius_payload;
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
            StyleValueData::Ray(ray_payload) => {
                let RayPayload {
                    size,
                    contain,
                    angle,
                    position,
                } = &**ray_payload;
                hasher.write_u16(*size);
                write_bool(hasher, *contain);
                write_value(hasher, angle);
                write_value(hasher, position);
            }
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_keyword(keyword: u16) -> *const StyleValueData {
    StyleValueArc::into_raw(shared_style_value(StyleValueData::Keyword { keyword }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_number(value: f64) -> *const StyleValueData {
    StyleValueArc::into_raw(shared_style_value(StyleValueData::Number { value }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_integer(value: i32) -> *const StyleValueData {
    StyleValueArc::into_raw(shared_style_value(StyleValueData::Integer { value }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_angle(value: f64, unit: u8) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Angle { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_length(value: f64, unit: u8) -> *const StyleValueData {
    StyleValueArc::into_raw(shared_style_value(StyleValueData::Length { value, unit }))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_percentage(value: f64) -> *const StyleValueData {
    StyleValueArc::into_raw(shared_style_value(StyleValueData::Percentage { value }))
}

/// Takes ownership of one strong reference to each of the numerator and denominator.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_ratio(
    numerator: OwnedStyleValue,
    denominator: OwnedStyleValue,
) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Ratio(StyleValueBox::new(
        RatioPayload {
            numerator: RetainedStyleValueData::from_arc(numerator.into_arc()),
            denominator: RetainedStyleValueData::from_arc(denominator.into_arc()),
        },
    ))))
}

/// Takes ownership of one strong reference to each track size.
pub(crate) unsafe fn style_value_create_grid_track_size_list(
    track_sizes: *const *const StyleValueData,
    length: usize,
) -> *const StyleValueData {
    let entries = unsafe { ak::slice_from_raw_parts(track_sizes, length) }
        .iter()
        .map(|&track_size| {
            RetainedGridTrackEntry::retained_size(unsafe { RetainedStyleValueData::from_retained_pointer(track_size) })
        })
        .collect();
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::GridTrackSizeList(
        StyleValueBox::new(GridTrackSizeListPayload {
            is_subgrid: false,
            preserve_line_name_sets: false,
            entries: RetainedGridTrackEntryList::from_retained_entries(entries),
        }),
    )))
}

/// The track size of a grid track size list that holds just one track size, without line names, or null. The
/// returned value is borrowed from `data`.
pub(crate) fn style_value_grid_track_size_list_single_size(data: &StyleValueData) -> *const StyleValueData {
    let StyleValueData::GridTrackSizeList(grid_track_size_list) = data else {
        return std::ptr::null();
    };
    if grid_track_size_list.is_subgrid {
        return std::ptr::null();
    }
    match grid_track_size_list.entries.as_slice() {
        [entry] if entry.kind == GridTrackEntryKind::Size => entry.size_value.pointer(),
        _ => std::ptr::null(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_guaranteed_invalid() -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::GuaranteedInvalid))
}

/// Takes ownership of one leaked reference to the name and one strong reference to the value data.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_function(
    name: usize,
    value: OwnedStyleValue,
) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Function(StyleValueBox::new(
        FunctionPayload {
            name: unsafe { CssString::from_leaked_raw(name) },
            value: RetainedStyleValueData::from_arc(value.into_arc()),
        },
    ))))
}

/// Takes ownership of one strong reference to the angle value data if it is non-null.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_font_style(
    font_style: u8,
    angle_value: OwnedStyleValue,
) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::FontStyle {
        font_style,
        angle_value: RetainedStyleValueData::from_optional_arc(angle_value.into_option()),
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
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::ValueList {
        values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, length) },
        separator,
        collapsible,
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
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Transformation(StyleValueBox::new(
        TransformationPayload {
            property,
            transform_function,
            values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, length) },
            matrix_values: crate::css::style_value::RetainedStyleValueDataList::from_retained_values(Vec::new()),
        },
    ))))
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
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Shorthand(StyleValueBox::new(
        ShorthandPayload {
            shorthand_property,
            sub_properties: unsafe { RetainedPropertyIdList::from_raw(sub_properties, sub_property_count) },
            values: unsafe { RetainedStyleValueDataList::from_retained_pointers(values, value_count) },
        },
    ))))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_display(raw: u32) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Display { raw }))
}

/// Creates an unresolved value from borrowed token source, deriving its source text according to
/// the requested SourceTextMode and retaining the component tree.
///
/// Takes ownership of one strong reference to `parsed_value` when it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_unresolved_from_source(
    token_source_ascii_units: *const u8,
    token_source_code_units: *const u16,
    token_source_length: usize,
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
    let normalized = serialize_component_values_to_utf16(&components, ComponentSerializationMode::Normalized);
    let source_text = match source_text_mode {
        0 | 1 => serialize_component_values_to_utf16(&components, ComponentSerializationMode::PreserveNumericSource),
        2 => original_component_values_source(&components).unwrap_or(normalized),
        _ => unreachable!("unknown unresolved source text mode"),
    };
    let source_text = if source_text_mode == 0 {
        trim_component_value_boundary_whitespace(&source_text)
    } else if source_text_mode == 1 {
        trim_ascii_whitespace_start(&source_text)
    } else {
        &source_text
    };
    let presence = SubstitutionFunctionsPresence {
        attr: presence_attr,
        dashed_function: presence_dashed_function,
        env: presence_env,
        if_: presence_if,
        inherit: presence_inherit,
        var: presence_var,
    };
    let payload = UnresolvedBox::new(
        TokenizerInput::Utf16(source_text),
        UnresolvedValueComponents::OfText(&components),
        presence,
        contains_attr_tainted_values,
        unsafe { RetainedStyleValueData::from_retained_optional_pointer(parsed_value) },
    );
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Unresolved(payload)))
}

/// Takes ownership of the definitions' retained strings and values.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_definitions(
    definitions: *const FfiCounterDefinition,
    length: usize,
) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::CounterDefinitions(
        StyleValueBox::new(CounterDefinitionsPayload {
            counter_definitions: unsafe { RetainedCounterDefinitionList::from_raw(definitions, length) },
        }),
    )))
}

/// A counter style reference by name. Takes ownership of one leaked reference to the name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_counter_style(name: usize) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::CounterStyle(StyleValueBox::new(
        CounterStylePayload {
            is_symbols: false,
            name: unsafe { CssString::from_leaked_raw(name) },
            symbols_type: 0,
            symbols: CssStringList::from_strings(Vec::new()),
        },
    ))))
}

/// Takes ownership of one strong reference to each non-null value and one leaked reference to
/// the name when present.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_color_function(
    has_color_type: bool,
    color_type: u8,
    color_syntax: u8,
    channel_0: OwnedStyleValue,
    channel_1: OwnedStyleValue,
    channel_2: OwnedStyleValue,
    alpha: OwnedStyleValue,
    has_name: bool,
    name: usize,
    origin_color: OwnedStyleValue,
) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::ColorFunction(StyleValueBox::new(
        ColorFunctionPayload {
            color_base: ColorBase {
                has_color_type,
                color_type,
                color_syntax,
            },
            channel_0: RetainedStyleValueData::from_arc(channel_0.into_arc()),
            channel_1: RetainedStyleValueData::from_arc(channel_1.into_arc()),
            channel_2: RetainedStyleValueData::from_arc(channel_2.into_arc()),
            alpha: RetainedStyleValueData::from_optional_arc(alpha.into_option()),
            has_name,
            name: unsafe { CssString::from_leaked_raw(name) },
            origin_color: RetainedStyleValueData::from_optional_arc(origin_color.into_option()),
        },
    ))))
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
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Calculated(StyleValueBox::new(
        CalculatedPayload {
            rust_calculation: unsafe { crate::css::calc::CalcNodeHandle::from_raw(rust_calculation) },
            resolve_as_is_number,
            resolve_as_base,
            resolved_type,
            has_percentages_resolve_as,
            percentages_resolve_as,
            resolve_numbers_as_integers,
            accepted_ranges: unsafe { RetainedNumericRangeList::from_raw(accepted_ranges, accepted_range_count) },
        },
    ))))
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
) -> *const StyleValueData {
    StyleValueArc::into_raw(StyleValueArc::new(StyleValueData::Image(StyleValueBox::new(
        ImagePayload {
            url: RetainedString::from_units(url.units().expect("invalid URL text")),
            url_type,
            url_modifiers: unsafe { RetainedRequestUrlModifierList::from_raw(url_modifiers, url_modifier_count) },
            resource_context: ImageResourceContext {
                base_url: RetainedString::from_units(resource_base_url.units().expect("invalid base URL text")),
                has_base_url: has_resource_base_url,
                has_parent_style_sheet_origin_clean: false,
                parent_style_sheet_origin_clean: false,
                should_absolutize_url_for_computed_value: false,
            },
        },
    ))))
}

pub(crate) unsafe fn release_style_value(value: *const StyleValueData) {
    if value.is_null() {
        return;
    }
    unsafe { StyleValueArc::decrement_strong_count(value) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_release(value: *const StyleValueData) {
    unsafe { release_style_value(value) };
}

/// Whether two shared style values hold the same value.
///
/// The addresses are compared first because a shared allocation is the common case, and the values
/// otherwise: two allocations built from the same declaration are equal, and a caller holding a
/// freshly computed one has no way to know that from the pointer alone. Null stands for no value.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_equals(first: Option<&StyleValueData>, second: Option<&StyleValueData>) -> bool {
    optional_style_values_equal(first, second)
}

/// `rust_style_value_equals` for two handles.
pub(crate) fn retained_style_values_equal(first: &RetainedStyleValueData, second: &RetainedStyleValueData) -> bool {
    optional_style_values_equal(first.optional_data(), second.optional_data())
}

/// Two absent values are equal, and an absent value equals no present one.
pub(crate) fn optional_style_values_equal(first: Option<&StyleValueData>, second: Option<&StyleValueData>) -> bool {
    match (first, second) {
        (None, None) => true,
        (Some(first), Some(second)) => style_values_equal(first, second),
        _ => false,
    }
}

fn style_values_equal(first: &StyleValueData, second: &StyleValueData) -> bool {
    if std::ptr::eq(first, second) {
        return true;
    }
    match (first, second) {
        (
            StyleValueData::ColorFunction(color_function_payload_1),
            StyleValueData::ColorFunction(color_function_payload_2),
        ) => {
            let ColorFunctionPayload {
                color_base: first_base,
                channel_0: first_channel_0,
                channel_1: first_channel_1,
                channel_2: first_channel_2,
                alpha: first_alpha,
                has_name: first_has_name,
                name: first_name,
                origin_color: first_origin_color,
            } = &**color_function_payload_1;
            let ColorFunctionPayload {
                color_base: second_base,
                channel_0: second_channel_0,
                channel_1: second_channel_1,
                channel_2: second_channel_2,
                alpha: second_alpha,
                has_name: second_has_name,
                name: second_name,
                origin_color: second_origin_color,
            } = &**color_function_payload_2;
            first_base.has_color_type == second_base.has_color_type
                && (!first_base.has_color_type || first_base.color_type == second_base.color_type)
                && first_channel_0 == second_channel_0
                && first_channel_1 == second_channel_1
                && first_channel_2 == second_channel_2
                && first_alpha == second_alpha
                && first_has_name == second_has_name
                && (!first_has_name || first_name == second_name)
                && first_origin_color == second_origin_color
        }
        _ => first == second,
    }
}

/// Retains one reference to a shared style value allocation.
///
/// # Safety
/// `value` must be null or point at a live `StyleValueData` allocated by this module.
pub(crate) unsafe fn retain_style_value(value: *const StyleValueData) -> *const StyleValueData {
    if !value.is_null() {
        unsafe { StyleValueArc::increment_strong_count(value) };
    }
    value
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_retain(value: *const StyleValueData) -> *const StyleValueData {
    unsafe { retain_style_value(value) }
}

/// https://drafts.csswg.org/css-viewport/#zoom-om
/// Computed style APIs (i.e., all values returned by getComputedStyle()) that are non-auto and non-percentage lengths
/// must be unscaled.
/// The computed value `value` of an element whose effective zoom is `zoom`, with its lengths unscaled, retained for the
/// caller; null where unscaling changes nothing.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_unscaled_by_zoom(value: &StyleValueData, zoom: f64) -> *const StyleValueData {
    if zoom == 1.0 || !zoom.is_normal() || zoom < 0.0 {
        return std::ptr::null();
    }
    let length = crate::css::style_compute::FfiLengthResolutionContext {
        zoom: 1.0 / zoom,
        ..Default::default()
    };
    let context = crate::css::absolutize::AbsolutizationContext {
        length: &length,
        scheme: None,
        resolved_viewport_relative_length: std::cell::Cell::new(false),
        tree_counting: None,
        random_base_values: &[],
        document_base_url: &[],
        style_sheet_resource_context: None,
        current_color: None,
    };
    match crate::css::absolutize::absolutize(value, &context) {
        Some(crate::css::absolutize::Absolutized::Changed(unscaled)) => {
            let pointer = unscaled.pointer();
            std::mem::forget(unscaled);
            pointer
        }
        _ => std::ptr::null(),
    }
}

#[cfg(test)]
mod substitution_clone_tests {
    use super::*;

    #[test]
    fn pending_substitution_retains_its_shorthand_value() {
        let written = StyleValueArc::new(StyleValueData::Number { value: 1.0 });
        let pending = StyleValueArc::new(StyleValueData::PendingSubstitution {
            original_shorthand: crate::css::property_metadata::property_id::BORDER,
            original_shorthand_value: RetainedStyleValueData::from_arc(written.clone()),
        });
        assert_eq!(StyleValueArc::strong_count(&written), 2);
        drop(pending);
        assert_eq!(StyleValueArc::strong_count(&written), 1);
    }

    #[test]
    fn shorthand_clone_preserves_repeated_child_aliases() {
        let child = StyleValueArc::new(StyleValueData::Keyword { keyword: 1 });
        let properties = [1, 2];
        let children = [StyleValueArc::into_raw(child.clone()), StyleValueArc::into_raw(child)];
        let shorthand = unsafe {
            rust_style_value_create_shorthand(
                3,
                properties.as_ptr(),
                properties.len(),
                children.as_ptr(),
                children.len(),
            )
        };
        let cloned_shorthand = StyleValueArc::into_raw(StyleValueArc::new(unsafe { (*shorthand).clone() }));

        let StyleValueData::Shorthand(shorthand_payload) = (unsafe { &*shorthand }) else {
            unreachable!()
        };
        let ShorthandPayload { values: original, .. } = &**shorthand_payload;
        let StyleValueData::Shorthand(shorthand_payload) = (unsafe { &*cloned_shorthand }) else {
            unreachable!()
        };
        let ShorthandPayload {
            values: cloned_values, ..
        } = &**shorthand_payload;
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

// Both spellings of one text hash alike: each is fed as the same chunks of UTF-16 code units.
fn hash_string_units(hasher: &mut crate::css::style::fast_hash::FastHasher, units: TokenizerInput<'_>) {
    use std::hash::Hasher;
    const CHUNK: usize = 64;
    fn write_chunk(hasher: &mut crate::css::style::fast_hash::FastHasher, chunk: &[u16]) {
        let mut bytes = [0u8; CHUNK * 2];
        for (unit, destination) in chunk.iter().zip(bytes.as_chunks_mut::<2>().0) {
            *destination = unit.to_ne_bytes();
        }
        hasher.write(&bytes[..chunk.len() * 2]);
    }
    match units {
        TokenizerInput::Latin1(units) => {
            let mut widened = [0u16; CHUNK];
            for chunk in units.chunks(CHUNK) {
                for (unit, destination) in chunk.iter().zip(widened.iter_mut()) {
                    *destination = u16::from(*unit);
                }
                write_chunk(hasher, &widened[..chunk.len()]);
            }
        }
        TokenizerInput::Utf16(units) => {
            for chunk in units.chunks(CHUNK) {
                write_chunk(hasher, chunk);
            }
        }
    }
}

#[cfg(test)]
mod equality_tests {
    use super::*;
    use crate::css::css_tokenizer::tokenize_for_parser;
    use crate::css::parser::component_value::consume_a_list_of_component_values;
    use crate::css::parser::value_parser::unresolved_value;

    fn unresolved(source: &str) -> StyleValueData {
        let values = consume_a_list_of_component_values(tokenize_for_parser(source.as_bytes())).unwrap();
        let serialized = serialize_component_values_to_utf16(&values, ComponentSerializationMode::Retokenize);
        unresolved_value(&serialized, values, Default::default())
    }

    #[test]
    fn unresolved_equality_preserves_numeric_token_types() {
        for (first, second) in [("1", "1.0"), ("1", "1e0"), ("f(1)", "f(1.0)"), ("[1]", "[1.0]")] {
            let first = unresolved(first);
            let second = unresolved(second);
            assert!(!style_values_equal(&first, &second));
        }
    }

    #[test]
    fn text_hashes_alike_in_either_spelling() {
        use std::hash::Hasher;
        let text = "a text longer than one chunk of the hasher, so that it takes more than one write";
        let utf16: Vec<u16> = text.encode_utf16().collect();
        let mut ascii_hasher = crate::css::style::fast_hash::fast_hasher();
        hash_string_units(&mut ascii_hasher, TokenizerInput::Latin1(text.as_bytes()));
        let mut utf16_hasher = crate::css::style::fast_hash::fast_hasher();
        hash_string_units(&mut utf16_hasher, TokenizerInput::Utf16(&utf16));
        assert_eq!(ascii_hasher.finish(), utf16_hasher.finish());
    }

    fn retained_number(value: f64) -> RetainedStyleValueData {
        RetainedStyleValueData::from_arc(StyleValueArc::new(StyleValueData::Number { value }))
    }

    fn retained(value: StyleValueData) -> RetainedStyleValueData {
        RetainedStyleValueData::from_arc(StyleValueArc::new(value))
    }

    fn color_function(color_syntax: u8) -> StyleValueData {
        StyleValueData::ColorFunction(StyleValueBox::new(ColorFunctionPayload {
            color_base: ColorBase {
                has_color_type: true,
                color_type: 0,
                color_syntax,
            },
            channel_0: retained_number(1.0),
            channel_1: retained_number(2.0),
            channel_2: retained_number(3.0),
            alpha: RetainedStyleValueData::none(),
            has_name: false,
            name: CssString::none(),
            origin_color: RetainedStyleValueData::none(),
        }))
    }

    #[test]
    fn color_function_equality_ignores_syntax() {
        let legacy = color_function(0);
        let modern = color_function(1);
        assert!(legacy != modern);
        assert!(style_values_equal(&legacy, &modern));
    }

    #[test]
    fn color_equality_rejects_different_variants() {
        let color = color_function(0);
        let light_dark = StyleValueData::LightDark(StyleValueBox::new(LightDarkPayload {
            color_base: ColorBase {
                has_color_type: false,
                color_type: 0,
                color_syntax: 1,
            },
            light: retained(color_function(0)),
            dark: retained(color_function(0)),
        }));
        assert!(!style_values_equal(&color, &light_dark));
        assert!(!style_values_equal(&light_dark, &color));
    }
}

/// Whether a value's computed color depends on the element's used currentcolor: the
/// currentcolor keyword itself, a color function whose nested colors do, an Effects list
/// whose shadow or filter colors do, or a scrollbar color whose thumb or track color does.
pub(crate) fn value_depends_on_current_color(value: &StyleValueData) -> bool {
    let retained_data_depends =
        |retained: &RetainedStyleValueData| retained.optional_data().is_some_and(value_depends_on_current_color);
    let list_depends = |values: &RetainedStyleValueDataList| values.as_slice().iter().any(retained_data_depends);
    match value {
        StyleValueData::Keyword { keyword } => *keyword == crate::css::style_compute::keyword::CURRENTCOLOR,
        StyleValueData::ColorFunction(color_function_payload) => {
            let ColorFunctionPayload { origin_color, .. } = &**color_function_payload;
            retained_data_depends(origin_color)
        }
        StyleValueData::ColorMix(color_mix_payload) => {
            let ColorMixPayload { colors, .. } = &**color_mix_payload;
            colors.as_slice().iter().any(retained_data_depends)
        }
        StyleValueData::ContrastColor { color, .. } => retained_data_depends(color),
        StyleValueData::LightDark(light_dark_payload) => {
            let LightDarkPayload { light, dark, .. } = &**light_dark_payload;
            retained_data_depends(light) || retained_data_depends(dark)
        }
        StyleValueData::Shadow(shadow_payload) => {
            // A shadow without a color is `currentcolor`.
            let ShadowPayload { color, .. } = &**shadow_payload;
            color.optional_data().is_none_or(value_depends_on_current_color)
        }
        StyleValueData::Filter { value, .. } => retained_data_depends(value),
        StyleValueData::ScrollbarColor(scrollbar_color_payload) => {
            let ScrollbarColorPayload {
                thumb_color,
                track_color,
            } = &**scrollbar_color_payload;
            retained_data_depends(thumb_color) || retained_data_depends(track_color)
        }
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
        StyleValueData::ColorFunction(color_function_payload) => {
            let ColorFunctionPayload { origin_color, .. } = &**color_function_payload;
            origin_color.optional_data().is_some_and(value_depends_on_color_scheme)
        }
        StyleValueData::ColorMix(color_mix_payload) => {
            let ColorMixPayload { colors, .. } = &**color_mix_payload;
            colors.as_slice().iter().any(retained_data_depends)
        }
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

pub(crate) fn style_value_dependency_flags(value: &StyleValueData) -> u8 {
    u8::from(value_depends_on_current_color(value))
        | (u8::from(value_depends_on_color_scheme(value)) << 1)
        | (u8::from(value_may_depend_on_font_metrics(value)) << 2)
}

pub(crate) fn retained_value_depends_on_current_color(value: &RetainedStyleValueData) -> bool {
    style_value_dependency_flags(value.data()) & 1 != 0
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_depends_on_current_color(data: &StyleValueData) -> bool {
    value_depends_on_current_color(data)
}
