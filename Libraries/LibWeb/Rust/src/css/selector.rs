/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub use crate::css::ffi_support::FfiStringView;
use crate::css::ffi_support::OwnedBox;
use crate::css::ffi_support::ascii_lowercase;
use crate::css::style::index::StyleAtomID;

pub type SelectorString = crate::css::css_string::CssString;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
pub enum Combinator {
    None,
    ImmediateChild,
    Descendant,
    NextSibling,
    SubsequentSibling,
    Column,
    PseudoElement,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: The numeric values are part of the C++ FFI.
pub enum NamespaceType {
    Default,
    None,
    Any,
    Named,
}

/// A name with its namespace. The names are process style atoms, which the selector holding them
/// leases; the atom table gives each its text and its ASCII-lowercase form.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct QualifiedName {
    pub namespace_type: NamespaceType,
    /// The namespace prefix, for a `Named` namespace, and none otherwise.
    pub namespace: StyleAtomID,
    pub name: StyleAtomID,
    /// Whether `name` is its own ASCII-lowercase form, which a match that folds the name compares as written.
    pub name_is_ascii_lowercase: bool,
}

impl QualifiedName {
    #[must_use]
    pub fn new(namespace_type: NamespaceType, namespace: StyleAtomID, name: StyleAtomID) -> Self {
        Self {
            namespace_type,
            namespace,
            name,
            name_is_ascii_lowercase: is_ascii_lowercase_atom(name),
        }
    }
}

/// The name of an id or class selector, as a process style atom the selector holding it leases.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct NameSelector {
    pub name: StyleAtomID,
    /// Whether `name` is its own ASCII-lowercase form, which a match that folds the name compares as written.
    pub is_ascii_lowercase: bool,
}

impl NameSelector {
    #[must_use]
    pub fn new(name: StyleAtomID) -> Self {
        Self {
            name,
            is_ascii_lowercase: is_ascii_lowercase_atom(name),
        }
    }
}

/// Whether the name of `atom` is its own ASCII-lowercase form.
fn is_ascii_lowercase_atom(atom: StyleAtomID) -> bool {
    atom.is_none() || crate::css::style::process_atoms().ascii_lowercase(atom) == atom
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AttributeMatchType {
    HasAttribute,
    ExactValue,
    ContainsWord,
    ContainsString,
    StartsWithSegment,
    StartsWithString,
    EndsWithString,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum AttributeCaseType {
    Default,
    Sensitive,
    Insensitive,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AnPlusBPattern {
    pub step_size: i32,
    pub offset: i32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    LeftToRight,
    RightToLeft,
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PseudoClassParameterType {
    None,
    AnPlusB,
    AnPlusBOf,
    CompoundSelector,
    ForgivingSelectorList,
    Ident,
    LanguageRanges,
    LevelList,
    RelativeSelectorList,
    SelectorList,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PseudoClassMetadata {
    pub parameter_type: PseudoClassParameterType,
    pub is_valid_as_function: bool,
    pub is_valid_as_identifier: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PseudoElementParameterType {
    None,
    CompoundSelector,
    HighlightNameSelector,
    IdentList,
    PTNameAndClassSelector,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PseudoElementMetadata {
    pub parameter_type: PseudoElementParameterType,
    pub is_valid_as_function: bool,
    pub is_valid_as_identifier: bool,
}

fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&unit, &byte)| ascii_lowercase(unit) == u16::from(byte.to_ascii_lowercase()))
}

include!(concat!(env!("OUT_DIR"), "/selector_pseudo_generated.rs"));

/// Selector specificity, compared component by component.
#[derive(Clone, Copy, Debug, Default, Hash, PartialEq, Eq, PartialOrd, Ord)]
pub struct Specificity {
    pub ids: u16,
    pub classes: u16,
    pub types: u16,
}

impl Specificity {
    #[must_use]
    pub fn saturating_add(self, other: Self) -> Self {
        Self {
            ids: self.ids.saturating_add(other.ids),
            classes: self.classes.saturating_add(other.classes),
            types: self.types.saturating_add(other.types),
        }
    }

    #[must_use]
    pub(in crate::css) fn packed(self) -> u32 {
        (u32::from(self.ids.min(u16::from(u8::MAX))) << 16)
            | (u32::from(self.classes.min(u16::from(u8::MAX))) << 8)
            | u32::from(self.types.min(u16::from(u8::MAX)))
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum SearchTextMatchFilter {
    Current,
    NotCurrent,
}

pub use crate::css::selector_arena::{
    AttributeSelector, CompoundRef, PseudoClassSelector, PseudoElementSelector, PseudoElementValue, Selector,
    SelectorListRef, SelectorListView, SelectorRef, SimpleSelector, TransitionNameSelector,
    search_text_match_filter_of_pseudo_class,
};

/// A parsed selector, as C++ holds one.
pub struct RustSelector {
    pub(crate) selector: SelectorRef,
}

impl RustSelector {
    /// The selector, for consumers that translate it rather than match with it.
    #[must_use]
    pub fn compiled(&self) -> Selector<'_> {
        self.selector.view()
    }
}

struct SelectorSubtags<'a, T> {
    value: &'a [T],
    position: usize,
    finished: bool,
}

impl<'a, T> SelectorSubtags<'a, T> {
    fn new(value: &'a [T]) -> Self {
        Self {
            value,
            position: 0,
            finished: false,
        }
    }
}

impl<T: Copy + Into<u16>> Iterator for SelectorSubtags<'_, T> {
    type Item = std::ops::Range<usize>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.finished {
            return None;
        }
        let start = self.position;
        while self.position < self.value.len() && self.value[self.position].into() != u16::from(b'-') {
            self.position += 1;
        }
        let end = self.position;
        if self.position < self.value.len() {
            self.position += 1;
        } else {
            self.finished = true;
        }
        Some(start..end)
    }
}

fn language_subtags_match<T: Copy + Into<u16>>(
    language_range: &[u16],
    range_subtag: &std::ops::Range<usize>,
    language_tag: &[T],
    tag_subtag: &std::ops::Range<usize>,
) -> bool {
    if range_subtag.len() == 1 && language_range[range_subtag.start] == u16::from(b'*') {
        return true;
    }
    range_subtag.len() == tag_subtag.len()
        && (0..range_subtag.len()).all(|offset| {
            ascii_lowercase(language_range[range_subtag.start + offset])
                == ascii_lowercase(language_tag[tag_subtag.start + offset].into())
        })
}

fn is_ascii_alphanumeric(code_unit: u16) -> bool {
    (u16::from(b'0')..=u16::from(b'9')).contains(&code_unit)
        || (u16::from(b'A')..=u16::from(b'Z')).contains(&code_unit)
        || (u16::from(b'a')..=u16::from(b'z')).contains(&code_unit)
}

/// Whether an extended language range matches a language tag.
///
/// Both sides are plain code units so that every consumer of the algorithm shares one
/// implementation, whatever it holds the tag as. The tag's units are ASCII or UTF-16.
///
/// https://www.rfc-editor.org/rfc/rfc4647#section-3.3.2
pub fn language_range_matches_tag<T: Copy + Into<u16>>(language_range: &[u16], language_tag: &[T]) -> bool {
    // 1. Split both the extended language range and the language tag being compared into a list
    //    of subtags by dividing on the hyphen (%x2D) character.
    let mut range_subtags = SelectorSubtags::new(language_range);
    let mut tag_subtags = SelectorSubtags::new(language_tag);

    //    Two subtags match if either they are the same when compared case-insensitively or the
    //    language range's subtag is the wildcard '*'.

    // 2. Begin with the first subtag in each list. If the first subtag in the range does not match
    //    the first subtag in the tag, the overall match fails. Otherwise, move to the next subtag
    //    in both the range and the tag.
    let first_range_subtag = range_subtags.next().unwrap();
    let first_tag_subtag = tag_subtags.next().unwrap();
    if !language_subtags_match(language_range, &first_range_subtag, language_tag, &first_tag_subtag) {
        return false;
    }

    let mut tag_subtag = tag_subtags.next();

    // 3. While there are more subtags left in the language range's list:
    for range_subtag in range_subtags {
        // A. If the subtag currently being examined in the range is the wildcard ('*'), move to
        //    the next subtag in the range and continue with the loop.
        if range_subtag.len() == 1 && language_range[range_subtag.start] == u16::from(b'*') {
            continue;
        }

        // B. Else, if there are no more subtags in the language tag's list, the match fails.
        loop {
            let Some(current_tag_subtag) = tag_subtag else {
                return false;
            };

            // C. Else, if the current subtag in the range's list matches the current subtag in the
            //    language tag's list, move to the next subtag in both lists and continue with the
            //    loop.
            if language_subtags_match(language_range, &range_subtag, language_tag, &current_tag_subtag) {
                tag_subtag = tag_subtags.next();
                break;
            }

            // D. Else, if the language tag's subtag is a "singleton" (a single letter or digit,
            //    which includes the private-use subtag 'x') the match fails.
            if current_tag_subtag.len() == 1 && is_ascii_alphanumeric(language_tag[current_tag_subtag.start].into()) {
                return false;
            }

            // E. Else, move to the next subtag in the language tag's list and continue with the
            //    loop.
            tag_subtag = tag_subtags.next();
        }
    }

    // 4. When the language range's list has no more subtags, the match succeeds.
    true
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
// Attribute selectors on an HTML element in an HTML document must treat the values of attributes
// with the following names as ASCII case-insensitive:
pub fn is_ascii_case_insensitive_html_attribute(name: &[u16]) -> bool {
    const NAMES: &[&[u8]] = &[
        b"accept",
        b"accept-charset",
        b"align",
        b"alink",
        b"axis",
        b"bgcolor",
        b"charset",
        b"checked",
        b"clear",
        b"codetype",
        b"color",
        b"compact",
        b"declare",
        b"defer",
        b"dir",
        b"direction",
        b"disabled",
        b"enctype",
        b"face",
        b"frame",
        b"hreflang",
        b"http-equiv",
        b"lang",
        b"language",
        b"link",
        b"media",
        b"method",
        b"multiple",
        b"nohref",
        b"noresize",
        b"noshade",
        b"nowrap",
        b"readonly",
        b"rel",
        b"rev",
        b"rules",
        b"scope",
        b"scrolling",
        b"selected",
        b"shape",
        b"target",
        b"text",
        b"type",
        b"valign",
        b"valuetype",
        b"vlink",
    ];
    NAMES.iter().any(|candidate| {
        name.len() == candidate.len() && candidate.iter().zip(name).all(|(&byte, &unit)| unit == u16::from(byte))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_selector_destroy(selector: OwnedBox<RustSelector>) {
    drop(selector);
}

/// Returns the selector's specificity in the packed representation used by the DevTools protocol.
///
/// # Safety
/// `selector` must point to a live selector handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_specificity(selector: *const RustSelector) -> u32 {
    assert!(!selector.is_null());
    // SAFETY: The caller guarantees that the selector handle remains valid for this call.
    unsafe { &(*selector).selector }.view().specificity().packed()
}
