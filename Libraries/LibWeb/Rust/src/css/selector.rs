/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Arc;

pub use crate::css::ffi_support::FfiStringView;
use crate::css::ffi_support::ascii_lowercase;
use crate::css::retained_fly_string::RetainedUtf16FlyString;

pub type SelectorString = crate::css::css_string::CssString;
pub type SelectorList = Box<[Arc<CompiledSelector>]>;

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

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QualifiedName {
    pub namespace_type: NamespaceType,
    pub namespace: RetainedUtf16FlyString,
    pub name: RetainedUtf16FlyString,
    pub lowercase_name: RetainedUtf16FlyString,
}

impl QualifiedName {
    #[must_use]
    pub fn interned_name_identity(&self) -> Option<usize> {
        self.name.optional_raw()
    }

    /// HTML elements in HTML documents match tag names case-insensitively, so the lowercase form is
    /// the identity a tag atom is keyed by.
    #[must_use]
    pub fn interned_lowercase_name_identity(&self) -> Option<usize> {
        self.lowercase_name.optional_raw()
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NameSelector {
    pub name: RetainedUtf16FlyString,
    /// The ASCII-lowercase name used for quirks-mode id and class matching.
    pub lowercase_name: RetainedUtf16FlyString,
}

impl NameSelector {
    /// The interned identity of this name, for keying an atom table by the same word the DOM side
    /// compares against.
    #[must_use]
    pub fn interned_name_identity(&self) -> Option<usize> {
        self.name.optional_raw()
    }

    /// The interned identity of this name's ASCII-lowercase folding.
    #[must_use]
    pub fn interned_lowercase_name_identity(&self) -> Option<usize> {
        self.lowercase_name.optional_raw()
    }
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

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttributeSelector {
    pub match_type: AttributeMatchType,
    pub qualified_name: QualifiedName,
    pub value: SelectorString,
    pub value_identity: RetainedUtf16FlyString,
    pub case_type: AttributeCaseType,
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
    IdentList,
    PTNameSelector,
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

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct LanguageRange {
    pub value: SelectorString,
    pub is_string: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PseudoClassSelector {
    pub pseudo_class: PseudoClassType,
    pub an_plus_b_pattern: AnPlusBPattern,
    pub argument_selector_list: SelectorList,
    pub languages: Box<[LanguageRange]>,
    pub direction: Option<Direction>,
    pub identifier: Option<SelectorString>,
    pub identifier_identity: RetainedUtf16FlyString,
    pub identifier_lowercase_identity: RetainedUtf16FlyString,
    pub levels: Box<[i64]>,
    pub is_forgiving: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PseudoElementValue {
    None,
    CompoundSelector(Arc<CompiledSelector>),
    Identifiers(Box<[SelectorString]>),
    TransitionName { is_universal: bool, value: SelectorString },
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PseudoElementSelector {
    pub pseudo_element: PseudoElementType,
    pub serialized_name: Option<SelectorString>,
    pub value: PseudoElementValue,
    /// The interned identity of each name in `value`, when it holds identifiers, keyed by the same
    /// word the DOM side compares against.
    pub identifier_identities: Box<[RetainedUtf16FlyString]>,
}

// Keep the common ID and class selectors inline without reserving space for the larger payloads.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SimpleSelector {
    Universal(Box<QualifiedName>),
    TagName(Box<QualifiedName>),
    Id(NameSelector),
    Class(NameSelector),
    Attribute(Box<AttributeSelector>),
    PseudoClass(Box<PseudoClassSelector>),
    PseudoElement(Box<PseudoElementSelector>),
    Nesting,
    Invalid(SelectorString),
}

// Keep names to three words, plus the selector tag.
const _: () = assert!(size_of::<SimpleSelector>() <= 4 * size_of::<usize>());

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompoundSelector {
    /// The combinator relating this compound to the compound immediately to its left. The
    /// leftmost compound therefore has `Combinator::None`.
    pub combinator: Combinator,
    pub is_implicit_universal_anchor: bool,
    pub simple_selectors: Box<[SimpleSelector]>,
}

/// The immutable representation used by every matching path.
///
/// Compounds retain their parsed left-to-right order. Matching starts at the final compound and
/// follows each compound's combinator toward the beginning of this slice.
#[derive(Debug, PartialEq, Eq)]
pub struct CompiledSelector {
    pub compound_selectors: Box<[CompoundSelector]>,
    /// The pseudo-element required on the initial match target, if this selector ends in one.
    pub target_pseudo_element: Option<PseudoElementType>,
}

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
    fn packed(self) -> u32 {
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

pub fn search_text_match_filter_of_pseudo_class(pseudo_class: &PseudoClassSelector) -> Option<SearchTextMatchFilter> {
    match pseudo_class.pseudo_class {
        PseudoClassType::Current => Some(SearchTextMatchFilter::Current),
        PseudoClassType::Not => {
            let [argument] = &*pseudo_class.argument_selector_list else {
                return None;
            };
            let [compound] = &*argument.compound_selectors else {
                return None;
            };
            matches!(
                &*compound.simple_selectors,
                [SimpleSelector::PseudoClass(inner)] if inner.pseudo_class == PseudoClassType::Current
            )
            .then_some(SearchTextMatchFilter::NotCurrent)
        }
        _ => None,
    }
}

pub fn search_text_match_filter(simple: &SimpleSelector) -> Option<SearchTextMatchFilter> {
    match simple {
        SimpleSelector::PseudoClass(pseudo_class) => search_text_match_filter_of_pseudo_class(pseudo_class),
        _ => None,
    }
}

impl CompiledSelector {
    #[allow(clippy::arc_with_non_send_sync)] // Bound selectors retain document-thread atoms.
    pub(crate) fn new(compound_selectors: Box<[CompoundSelector]>) -> Arc<Self> {
        let target_pseudo_element = compound_selectors
            .last()
            .and_then(|compound| compound.simple_selectors.first())
            .and_then(|simple_selector| match simple_selector {
                SimpleSelector::PseudoElement(selector)
                    if !matches!(
                        selector.pseudo_element,
                        PseudoElementType::Part | PseudoElementType::Slotted
                    ) =>
                {
                    Some(selector.pseudo_element)
                }
                _ => None,
            })
            .map(|pseudo_element| {
                let names_current_match = pseudo_element == PseudoElementType::SearchText
                    && compound_selectors.last().is_some_and(|compound| {
                        compound
                            .simple_selectors
                            .iter()
                            .any(|simple| search_text_match_filter(simple) == Some(SearchTextMatchFilter::Current))
                    });
                if names_current_match {
                    PseudoElementType::SearchTextCurrent
                } else {
                    pseudo_element
                }
            });

        Arc::new(Self {
            compound_selectors,
            target_pseudo_element,
        })
    }

    pub fn styles_every_search_text_match(&self) -> bool {
        self.target_pseudo_element == Some(PseudoElementType::SearchText)
            && self.compound_selectors.last().is_some_and(|compound| {
                compound
                    .simple_selectors
                    .iter()
                    .all(|simple| search_text_match_filter(simple).is_none())
            })
    }

    /// https://www.w3.org/TR/selectors-4/#specificity-rules
    #[must_use]
    pub fn specificity(&self) -> Specificity {
        fn greatest_specificity(selectors: &[Arc<CompiledSelector>]) -> Specificity {
            selectors
                .iter()
                .map(|selector| selector.specificity())
                .max()
                .unwrap_or_default()
        }

        let mut specificity = Specificity::default();
        for compound in &self.compound_selectors {
            for simple_selector in &compound.simple_selectors {
                let contribution = match simple_selector {
                    SimpleSelector::Id(_) => Specificity {
                        ids: 1,
                        ..Specificity::default()
                    },
                    SimpleSelector::Class(_) | SimpleSelector::Attribute(_) => Specificity {
                        classes: 1,
                        ..Specificity::default()
                    },
                    SimpleSelector::TagName(_) => Specificity {
                        types: 1,
                        ..Specificity::default()
                    },
                    SimpleSelector::PseudoClass(pseudo_class) => match pseudo_class.pseudo_class {
                        PseudoClassType::Has | PseudoClassType::Is | PseudoClassType::Not => {
                            greatest_specificity(&pseudo_class.argument_selector_list)
                        }
                        PseudoClassType::NthChild | PseudoClassType::NthLastChild => Specificity {
                            classes: 1,
                            ..Specificity::default()
                        }
                        .saturating_add(greatest_specificity(&pseudo_class.argument_selector_list)),
                        PseudoClassType::Where => Specificity::default(),
                        PseudoClassType::Host => Specificity {
                            classes: 1,
                            ..Specificity::default()
                        }
                        .saturating_add(greatest_specificity(&pseudo_class.argument_selector_list)),
                        _ => Specificity {
                            classes: 1,
                            ..Specificity::default()
                        },
                    },
                    SimpleSelector::PseudoElement(pseudo_element) => {
                        if matches!(
                            pseudo_element.pseudo_element,
                            PseudoElementType::ViewTransitionGroup
                                | PseudoElementType::ViewTransitionImagePair
                                | PseudoElementType::ViewTransitionNew
                                | PseudoElementType::ViewTransitionOld
                        ) && matches!(
                            pseudo_element.value,
                            PseudoElementValue::TransitionName { is_universal: true, .. }
                        ) {
                            Specificity::default()
                        } else {
                            let pseudo_specificity = Specificity {
                                types: 1,
                                ..Specificity::default()
                            };
                            match &pseudo_element.value {
                                PseudoElementValue::CompoundSelector(argument)
                                    if pseudo_element.pseudo_element == PseudoElementType::Slotted =>
                                {
                                    pseudo_specificity.saturating_add(argument.specificity())
                                }
                                _ => pseudo_specificity,
                            }
                        }
                    }
                    SimpleSelector::Universal(_) | SimpleSelector::Nesting | SimpleSelector::Invalid(_) => {
                        Specificity::default()
                    }
                };
                specificity = specificity.saturating_add(contribution);
            }
        }
        specificity
    }
}

pub struct RustSelector {
    pub(crate) selector: Arc<CompiledSelector>,
}

impl RustSelector {
    /// The compiled selector, for consumers that translate it rather than match with it.
    #[must_use]
    pub fn compiled(&self) -> &CompiledSelector {
        &self.selector
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
pub fn is_ascii_case_insensitive_html_attribute(name: &RetainedUtf16FlyString) -> bool {
    let name = super::css_tokenizer::TokenizerInput::from(name);
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
        name.len() == candidate.len()
            && candidate
                .iter()
                .enumerate()
                .all(|(index, &byte)| name.code_unit_at(index) == u16::from(byte))
    })
}

/// # Safety
/// `selector` must be an owned selector handle that has not already been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_destroy(selector: *mut RustSelector) {
    if !selector.is_null() {
        // SAFETY: The caller guarantees that this is an owned handle returned by
        // a selector-producing FFI function and that it has not already been destroyed.
        drop(unsafe { Box::from_raw(selector) });
    }
}

/// Returns the selector's specificity in the packed representation used by the DevTools protocol.
///
/// # Safety
/// `selector` must point to a live selector handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_specificity(selector: *const RustSelector) -> u32 {
    assert!(!selector.is_null());
    // SAFETY: The caller guarantees that the selector handle remains valid for this call.
    unsafe { &(*selector).selector }.specificity().packed()
}
