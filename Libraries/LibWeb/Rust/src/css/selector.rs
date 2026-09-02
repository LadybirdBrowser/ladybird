/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::rc::Rc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::css::ffi_support::ascii_lowercase;
use crate::css::retained_fly_string::RetainedUtf16FlyString;

static NEXT_SELECTOR_ID: AtomicU64 = AtomicU64::new(1);

pub type SelectorString = Box<[u16]>;
/// A selector list owns references to selectors that have already been compiled. `Rc` allows
/// functional pseudo-classes to share those selectors with the C++ selector tree that owns them.
pub type SelectorList = Box<[Rc<CompiledSelector>]>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
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
#[allow(dead_code)]
pub enum NamespaceType {
    Default,
    None,
    Any,
    Named,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct QualifiedName {
    pub namespace_type: NamespaceType,
    pub namespace: SelectorString,
    pub name: SelectorString,
    pub lowercase_name: SelectorString,
    pub(crate) interned_name: Option<RetainedUtf16FlyString>,
    pub(crate) interned_lowercase_name: Option<RetainedUtf16FlyString>,
    pub(crate) interned_namespace: Option<RetainedUtf16FlyString>,
}

impl QualifiedName {
    #[must_use]
    pub fn interned_name_identity(&self) -> Option<usize> {
        self.interned_name.as_ref().map(RetainedUtf16FlyString::raw)
    }

    /// HTML elements in HTML documents match tag names case-insensitively, so the lowercase form is
    /// the identity a tag atom is keyed by.
    #[must_use]
    pub fn interned_lowercase_name_identity(&self) -> Option<usize> {
        self.interned_lowercase_name
            .as_ref()
            .or(self.interned_name.as_ref())
            .map(RetainedUtf16FlyString::raw)
    }

    #[must_use]
    pub fn interned_namespace_identity(&self) -> Option<usize> {
        self.interned_namespace.as_ref().map(RetainedUtf16FlyString::raw)
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct NameSelector {
    pub name: SelectorString,
    /// The one-word identity of the C++ `Utf16FlyString` backing `name`. This is present for
    /// parsed selectors and allows the live DOM wrapper to compare interned names
    /// without crossing the FFI.
    pub(crate) interned_name: Option<RetainedUtf16FlyString>,
    /// The identity of that name's ASCII-lowercase folding. A quirks-mode document matches id and
    /// class selectors case-insensitively, so it is the identity such a document keys them by.
    pub(crate) interned_lowercase_name: Option<RetainedUtf16FlyString>,
}

impl NameSelector {
    /// The interned identity of this name, for keying an atom table by the same word the DOM side
    /// compares against.
    #[must_use]
    pub fn interned_name_identity(&self) -> Option<usize> {
        self.interned_name.as_ref().map(RetainedUtf16FlyString::raw)
    }

    /// The interned identity of this name's ASCII-lowercase folding.
    #[must_use]
    pub fn interned_lowercase_name_identity(&self) -> Option<usize> {
        self.interned_lowercase_name
            .as_ref()
            .or(self.interned_name.as_ref())
            .map(RetainedUtf16FlyString::raw)
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[allow(dead_code)]
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
#[allow(dead_code)]
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
    pub value_identity: Option<RetainedUtf16FlyString>,
    pub case_type: AttributeCaseType,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct AnPlusBPattern {
    pub step_size: i32,
    pub offset: i32,
}

impl AnPlusBPattern {
    pub fn matches(self, index: i32) -> bool {
        if self.step_size == 0 {
            return index == self.offset;
        }
        let delta = i64::from(index) - i64::from(self.offset);
        let step_size = i64::from(self.step_size);
        delta % step_size == 0 && delta / step_size >= 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    LeftToRight,
    RightToLeft,
    Other,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PseudoClassParameterType {
    None,
    AnPlusB,
    AnPlusBOf,
    CompoundSelector,
    ForgivingSelectorList,
    ForgivingRelativeSelectorList,
    Ident,
    LanguageRanges,
    LevelList,
    RelativeSelectorList,
    SelectorList,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PseudoClassMetadata {
    pub parameter_type: PseudoClassParameterType,
    pub is_valid_as_function: bool,
    pub is_valid_as_identifier: bool,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum PseudoElementParameterType {
    None,
    CompoundSelector,
    IdentList,
    PTNameSelector,
}

#[allow(dead_code)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PseudoElementMetadata {
    pub parameter_type: PseudoElementParameterType,
    pub is_valid_as_function: bool,
    pub is_valid_as_identifier: bool,
}

#[allow(dead_code)]
fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&unit, &byte)| ascii_lowercase(unit) == u16::from(byte.to_ascii_lowercase()))
}

include!(concat!(env!("OUT_DIR"), "/selector_pseudo_generated.rs"));

/// Crate-visible access to the generated pseudo-element code mapping, for the
/// style computation core's pseudo-element decisions.
pub(crate) fn pseudo_element_type_from_code(value: u8) -> PseudoElementType {
    pseudo_element_from_ffi(value)
}

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
    pub identifier_identity: Option<RetainedUtf16FlyString>,
    pub identifier_lowercase_identity: Option<RetainedUtf16FlyString>,
    pub levels: Box<[i64]>,
    pub is_forgiving: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PseudoElementValue {
    None,
    CompoundSelector(Rc<CompiledSelector>),
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

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SimpleSelector {
    Universal(QualifiedName),
    TagName(QualifiedName),
    Id(NameSelector),
    Class(NameSelector),
    Attribute(AttributeSelector),
    PseudoClass(PseudoClassSelector),
    PseudoElement(PseudoElementSelector),
    Nesting,
    Invalid(SelectorString),
}

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
#[derive(Debug)]
pub struct CompiledSelector {
    /// Process-local identity used for `:has()` cache keys. Equality deliberately ignores it.
    id: u64,
    pub compound_selectors: Box<[CompoundSelector]>,
    /// The pseudo-element required on the initial match target, if this selector ends in one.
    pub target_pseudo_element: Option<PseudoElementType>,
    /// Whether matching needs only light-tree parent traversal and the simple selectors accepted
    /// by `fast_matches()`.
    pub can_use_fast_matches: bool,
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

impl PartialEq for CompiledSelector {
    fn eq(&self, other: &Self) -> bool {
        self.compound_selectors == other.compound_selectors
    }
}

impl Eq for CompiledSelector {}

impl CompiledSelector {
    pub(crate) fn new(compound_selectors: Box<[CompoundSelector]>) -> Rc<Self> {
        let id = NEXT_SELECTOR_ID.fetch_add(1, Ordering::Relaxed);
        assert_ne!(id, 0, "selector IDs must not wrap");

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
            });

        let can_use_fast_matches = compound_selectors.iter().all(|compound| {
            matches!(
                compound.combinator,
                Combinator::None | Combinator::Descendant | Combinator::ImmediateChild
            ) && compound
                .simple_selectors
                .iter()
                .all(can_simple_selector_use_fast_matches)
        });

        Rc::new(Self {
            id,
            compound_selectors,
            target_pseudo_element,
            can_use_fast_matches,
        })
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    /// https://www.w3.org/TR/selectors-4/#specificity-rules
    #[must_use]
    pub fn specificity(&self) -> Specificity {
        fn greatest_specificity(selectors: &[Rc<CompiledSelector>]) -> Specificity {
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

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStringView {
    pub data: *const u16,
    pub length: usize,
}

pub struct RustSelector {
    pub(crate) selector: Rc<CompiledSelector>,
}

impl RustSelector {
    /// The compiled selector, for consumers that translate it rather than match with it.
    #[must_use]
    pub fn compiled(&self) -> &CompiledSelector {
        &self.selector
    }
}

struct SelectorSubtags<'a> {
    value: &'a [u16],
    position: usize,
    finished: bool,
}

impl<'a> SelectorSubtags<'a> {
    fn new(value: &'a [u16]) -> Self {
        Self {
            value,
            position: 0,
            finished: false,
        }
    }
}

impl Iterator for SelectorSubtags<'_> {
    type Item = std::ops::Range<usize>;

    fn next(&mut self) -> Option<Self::Item> {
        if self.finished {
            return None;
        }
        let start = self.position;
        while self.position < self.value.len() && self.value[self.position] != u16::from(b'-') {
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

fn language_subtags_match(
    language_range: &[u16],
    range_subtag: &std::ops::Range<usize>,
    language_tag: &[u16],
    tag_subtag: &std::ops::Range<usize>,
) -> bool {
    if range_subtag.len() == 1 && language_range[range_subtag.start] == u16::from(b'*') {
        return true;
    }
    range_subtag.len() == tag_subtag.len()
        && (0..range_subtag.len()).all(|offset| {
            ascii_lowercase(language_range[range_subtag.start + offset])
                == ascii_lowercase(language_tag[tag_subtag.start + offset])
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
/// implementation, whatever it holds the tag as.
///
/// https://www.rfc-editor.org/rfc/rfc4647#section-3.3.2
pub fn language_range_matches_tag(language_range: &[u16], language_tag: &[u16]) -> bool {
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
            if current_tag_subtag.len() == 1 && is_ascii_alphanumeric(language_tag[current_tag_subtag.start]) {
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

fn utf16_equals_ascii(value: &[u16], ascii: &[u8]) -> bool {
    value.len() == ascii.len()
        && value
            .iter()
            .zip(ascii)
            .all(|(&code_unit, &byte)| code_unit == u16::from(byte))
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
    NAMES.iter().any(|candidate| utf16_equals_ascii(name, candidate))
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

/// # Safety
/// `selector` must point to a live selector handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_target_pseudo_element(selector: *const RustSelector) -> u8 {
    assert!(!selector.is_null());
    // SAFETY: The caller guarantees that `selector` points to a live selector handle.
    unsafe { &(*selector).selector }
        .target_pseudo_element
        .map_or(u8::MAX, |pseudo_element| pseudo_element as u8)
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

fn can_simple_selector_use_fast_matches(simple_selector: &SimpleSelector) -> bool {
    match simple_selector {
        SimpleSelector::Universal(_)
        | SimpleSelector::TagName(_)
        | SimpleSelector::Id(_)
        | SimpleSelector::Class(_)
        | SimpleSelector::Attribute(_) => true,
        SimpleSelector::PseudoClass(selector) => matches!(
            selector.pseudo_class,
            PseudoClassType::Active
                | PseudoClassType::AnyLink
                | PseudoClassType::Autofill
                | PseudoClassType::Checked
                | PseudoClassType::Disabled
                | PseudoClassType::Empty
                | PseudoClassType::Enabled
                | PseudoClassType::FirstChild
                | PseudoClassType::Focus
                | PseudoClassType::FocusVisible
                | PseudoClassType::FocusWithin
                | PseudoClassType::Hover
                | PseudoClassType::LastChild
                | PseudoClassType::Link
                | PseudoClassType::LocalLink
                | PseudoClassType::OnlyChild
                | PseudoClassType::Root
                | PseudoClassType::State
                | PseudoClassType::Unchecked
                | PseudoClassType::Visited
        ),
        SimpleSelector::PseudoElement(_) | SimpleSelector::Nesting | SimpleSelector::Invalid(_) => false,
    }
}
