/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::abort_on_panic;

static NEXT_SELECTOR_ID: AtomicU64 = AtomicU64::new(1);

pub type SelectorString = Box<[u16]>;
pub type SelectorList = Box<[Arc<CompiledSelector>]>;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
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
        let delta = index - self.offset;
        delta.checked_rem(self.step_size) == Some(0) && delta / self.step_size >= 0
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    LeftToRight,
    RightToLeft,
    Other,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PseudoClassType {
    Active,
    AnyLink,
    Autofill,
    Buffering,
    Checked,
    Default,
    Defined,
    Dir,
    Disabled,
    Empty,
    Enabled,
    EvenLessGoodValue,
    FirstChild,
    FirstOfType,
    Focus,
    FocusVisible,
    FocusWithin,
    Fullscreen,
    Has,
    Heading,
    HighValue,
    Host,
    Hover,
    Indeterminate,
    Invalid,
    Is,
    Lang,
    LastChild,
    LastOfType,
    Link,
    LocalLink,
    LowValue,
    Modal,
    Muted,
    Not,
    NthChild,
    NthLastChild,
    NthLastOfType,
    NthOfType,
    OnlyChild,
    OnlyOfType,
    Open,
    OptimalValue,
    Optional,
    PopoverOpen,
    Paused,
    PlaceholderShown,
    Playing,
    ReadOnly,
    ReadWrite,
    Required,
    Root,
    Scope,
    Seeking,
    Stalled,
    State,
    SuboptimalValue,
    Target,
    Unchecked,
    UserInvalid,
    UserValid,
    Valid,
    Visited,
    VolumeLocked,
    Where,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PseudoClassSelector {
    pub pseudo_class: PseudoClassType,
    pub an_plus_b_pattern: AnPlusBPattern,
    pub argument_selector_list: SelectorList,
    pub languages: Box<[SelectorString]>,
    pub direction: Option<Direction>,
    pub identifier: Option<SelectorString>,
    pub levels: Box<[i64]>,
}

impl PseudoClassSelector {
    #[cfg(test)]
    fn without_arguments(pseudo_class: PseudoClassType) -> Self {
        Self {
            pseudo_class,
            an_plus_b_pattern: AnPlusBPattern::default(),
            argument_selector_list: Box::new([]),
            languages: Box::new([]),
            direction: None,
            identifier: None,
            levels: Box::new([]),
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum PseudoElementType {
    After,
    Backdrop,
    Before,
    FirstLetter,
    FirstLine,
    Marker,
    Selection,
    ViewTransition,
    DetailsContent,
    FileSelectorButton,
    Placeholder,
    SliderFill,
    SliderThumb,
    SliderTrack,
    Part,
    Slotted,
    ViewTransitionGroup,
    ViewTransitionImagePair,
    ViewTransitionNew,
    ViewTransitionOld,
    UnknownWebKit,
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
    pub value: PseudoElementValue,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum SimpleSelector {
    Universal(QualifiedName),
    TagName(QualifiedName),
    Id(SelectorString),
    Class(SelectorString),
    Attribute(AttributeSelector),
    PseudoClass(PseudoClassSelector),
    PseudoElement(PseudoElementSelector),
    Nesting,
    Invalid,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompoundSelector {
    pub combinator: Combinator,
    pub is_implicit_universal_anchor: bool,
    pub simple_selectors: Box<[SimpleSelector]>,
}

#[derive(Debug)]
pub struct CompiledSelector {
    id: u64,
    pub compound_selectors: Box<[CompoundSelector]>,
    pub target_pseudo_element: Option<PseudoElementType>,
    pub can_use_fast_matches: bool,
    pub sibling_invalidation_distance: usize,
}

impl PartialEq for CompiledSelector {
    fn eq(&self, other: &Self) -> bool {
        self.compound_selectors == other.compound_selectors
    }
}

impl Eq for CompiledSelector {}

impl CompiledSelector {
    pub fn new(compound_selectors: Box<[CompoundSelector]>) -> Arc<Self> {
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

        let sibling_invalidation_distance = sibling_invalidation_distance(&compound_selectors);

        Arc::new(Self {
            id,
            compound_selectors,
            target_pseudo_element,
            can_use_fast_matches,
            sibling_invalidation_distance,
        })
    }

    pub fn id(&self) -> u64 {
        self.id
    }
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiSimpleSelectorType {
    Universal,
    TagName,
    Id,
    Class,
    Attribute,
    PseudoClass,
    PseudoElement,
    Nesting,
    Invalid,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiCombinator {
    None,
    ImmediateChild,
    Descendant,
    NextSibling,
    SubsequentSibling,
    Column,
    PseudoElement,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiNamespaceType {
    Default,
    None,
    Any,
    Named,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiAttributeMatchType {
    HasAttribute,
    ExactValue,
    ContainsWord,
    ContainsString,
    StartsWithSegment,
    StartsWithString,
    EndsWithString,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiAttributeCaseType {
    Default,
    Sensitive,
    Insensitive,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiDirection {
    None,
    LeftToRight,
    RightToLeft,
    Other,
}

#[derive(Clone, Copy)]
#[repr(u8)]
pub enum FfiPseudoElementValueType {
    None,
    CompoundSelector,
    Identifiers,
    TransitionName,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiStringView {
    pub data: *const u16,
    pub length: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSimpleSelector {
    pub selector_type: FfiSimpleSelectorType,
    pub namespace_type: FfiNamespaceType,
    pub namespace: FfiStringView,
    pub name: FfiStringView,
    pub lowercase_name: FfiStringView,
    pub attribute_match_type: FfiAttributeMatchType,
    pub attribute_case_type: FfiAttributeCaseType,
    pub attribute_value: FfiStringView,
    pub pseudo_class: u8,
    pub an_plus_b_step_size: i32,
    pub an_plus_b_offset: i32,
    pub argument_selectors: *const *const RustSelector,
    pub argument_selector_count: usize,
    pub languages: *const FfiStringView,
    pub language_count: usize,
    pub direction: FfiDirection,
    pub identifier: FfiStringView,
    pub levels: *const i64,
    pub level_count: usize,
    pub pseudo_element: u8,
    pub pseudo_element_value_type: FfiPseudoElementValueType,
    pub pseudo_element_selector: *const RustSelector,
    pub pseudo_element_identifiers: *const FfiStringView,
    pub pseudo_element_identifier_count: usize,
    pub transition_name_is_universal: bool,
    pub transition_name: FfiStringView,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiCompoundSelector {
    pub combinator: FfiCombinator,
    pub is_implicit_universal_anchor: bool,
    pub simple_selectors: *const FfiSimpleSelector,
    pub simple_selector_count: usize,
}

#[repr(C)]
pub struct FfiSelector {
    pub compound_selectors: *const FfiCompoundSelector,
    pub compound_selector_count: usize,
}

pub struct RustSelector {
    selector: Arc<CompiledSelector>,
}

impl From<FfiCombinator> for Combinator {
    fn from(value: FfiCombinator) -> Self {
        match value {
            FfiCombinator::None => Self::None,
            FfiCombinator::ImmediateChild => Self::ImmediateChild,
            FfiCombinator::Descendant => Self::Descendant,
            FfiCombinator::NextSibling => Self::NextSibling,
            FfiCombinator::SubsequentSibling => Self::SubsequentSibling,
            FfiCombinator::Column => Self::Column,
            FfiCombinator::PseudoElement => Self::PseudoElement,
        }
    }
}

impl From<FfiNamespaceType> for NamespaceType {
    fn from(value: FfiNamespaceType) -> Self {
        match value {
            FfiNamespaceType::Default => Self::Default,
            FfiNamespaceType::None => Self::None,
            FfiNamespaceType::Any => Self::Any,
            FfiNamespaceType::Named => Self::Named,
        }
    }
}

impl From<FfiAttributeMatchType> for AttributeMatchType {
    fn from(value: FfiAttributeMatchType) -> Self {
        match value {
            FfiAttributeMatchType::HasAttribute => Self::HasAttribute,
            FfiAttributeMatchType::ExactValue => Self::ExactValue,
            FfiAttributeMatchType::ContainsWord => Self::ContainsWord,
            FfiAttributeMatchType::ContainsString => Self::ContainsString,
            FfiAttributeMatchType::StartsWithSegment => Self::StartsWithSegment,
            FfiAttributeMatchType::StartsWithString => Self::StartsWithString,
            FfiAttributeMatchType::EndsWithString => Self::EndsWithString,
        }
    }
}

impl From<FfiAttributeCaseType> for AttributeCaseType {
    fn from(value: FfiAttributeCaseType) -> Self {
        match value {
            FfiAttributeCaseType::Default => Self::Default,
            FfiAttributeCaseType::Sensitive => Self::Sensitive,
            FfiAttributeCaseType::Insensitive => Self::Insensitive,
        }
    }
}

fn pseudo_class_from_ffi(value: u8) -> PseudoClassType {
    use PseudoClassType::*;
    match value {
        0 => Active,
        1 => AnyLink,
        2 => Autofill,
        3 => Buffering,
        4 => Checked,
        5 => Default,
        6 => Defined,
        7 => Dir,
        8 => Disabled,
        9 => Empty,
        10 => Enabled,
        11 => EvenLessGoodValue,
        12 => FirstChild,
        13 => FirstOfType,
        14 => Focus,
        15 => FocusVisible,
        16 => FocusWithin,
        17 => Fullscreen,
        18 => Has,
        19 => Heading,
        20 => HighValue,
        21 => Host,
        22 => Hover,
        23 => Indeterminate,
        24 => Invalid,
        25 => Is,
        26 => Lang,
        27 => LastChild,
        28 => LastOfType,
        29 => Link,
        30 => LocalLink,
        31 => LowValue,
        32 => Modal,
        33 => Muted,
        34 => Not,
        35 => NthChild,
        36 => NthLastChild,
        37 => NthLastOfType,
        38 => NthOfType,
        39 => OnlyChild,
        40 => OnlyOfType,
        41 => Open,
        42 => OptimalValue,
        43 => Optional,
        44 => PopoverOpen,
        45 => Paused,
        46 => PlaceholderShown,
        47 => Playing,
        48 => ReadOnly,
        49 => ReadWrite,
        50 => Required,
        51 => Root,
        52 => Scope,
        53 => Seeking,
        54 => Stalled,
        55 => State,
        56 => SuboptimalValue,
        57 => Target,
        58 => Unchecked,
        59 => UserInvalid,
        60 => UserValid,
        61 => Valid,
        62 => Visited,
        63 => VolumeLocked,
        64 => Where,
        _ => panic!("invalid pseudo-class {value}"),
    }
}

fn pseudo_element_from_ffi(value: u8) -> PseudoElementType {
    use PseudoElementType::*;
    match value {
        0 => After,
        1 => Backdrop,
        2 => Before,
        3 => FirstLetter,
        4 => FirstLine,
        5 => Marker,
        6 => Selection,
        7 => ViewTransition,
        8 => DetailsContent,
        9 => FileSelectorButton,
        10 => Placeholder,
        11 => SliderFill,
        12 => SliderThumb,
        13 => SliderTrack,
        14 => Part,
        15 => Slotted,
        16 => ViewTransitionGroup,
        17 => ViewTransitionImagePair,
        18 => ViewTransitionNew,
        19 => ViewTransitionOld,
        20 => UnknownWebKit,
        _ => panic!("invalid pseudo-element {value}"),
    }
}

unsafe fn ffi_slice<'a, T>(data: *const T, length: usize) -> &'a [T] {
    if length == 0 {
        return &[];
    }
    assert!(!data.is_null());
    unsafe { std::slice::from_raw_parts(data, length) }
}

unsafe fn string_from_ffi(value: FfiStringView) -> SelectorString {
    unsafe { ffi_slice(value.data, value.length) }.into()
}

fn qualified_name_from_ffi(selector: &FfiSimpleSelector) -> QualifiedName {
    QualifiedName {
        namespace_type: selector.namespace_type.into(),
        namespace: unsafe { string_from_ffi(selector.namespace) },
        name: unsafe { string_from_ffi(selector.name) },
        lowercase_name: unsafe { string_from_ffi(selector.lowercase_name) },
    }
}

unsafe fn selector_from_handle(handle: *const RustSelector) -> Arc<CompiledSelector> {
    assert!(!handle.is_null());
    unsafe { (*handle).selector.clone() }
}

unsafe fn simple_selector_from_ffi(selector: &FfiSimpleSelector) -> SimpleSelector {
    match selector.selector_type {
        FfiSimpleSelectorType::Universal => SimpleSelector::Universal(qualified_name_from_ffi(selector)),
        FfiSimpleSelectorType::TagName => SimpleSelector::TagName(qualified_name_from_ffi(selector)),
        FfiSimpleSelectorType::Id => SimpleSelector::Id(unsafe { string_from_ffi(selector.name) }),
        FfiSimpleSelectorType::Class => SimpleSelector::Class(unsafe { string_from_ffi(selector.name) }),
        FfiSimpleSelectorType::Attribute => SimpleSelector::Attribute(AttributeSelector {
            match_type: selector.attribute_match_type.into(),
            qualified_name: qualified_name_from_ffi(selector),
            value: unsafe { string_from_ffi(selector.attribute_value) },
            case_type: selector.attribute_case_type.into(),
        }),
        FfiSimpleSelectorType::PseudoClass => {
            let argument_selector_list = unsafe {
                ffi_slice(selector.argument_selectors, selector.argument_selector_count)
                    .iter()
                    .map(|handle| selector_from_handle(*handle))
                    .collect::<Vec<_>>()
                    .into_boxed_slice()
            };
            let languages = unsafe {
                ffi_slice(selector.languages, selector.language_count)
                    .iter()
                    .map(|language| string_from_ffi(*language))
                    .collect::<Vec<_>>()
                    .into_boxed_slice()
            };
            let direction = match selector.direction {
                FfiDirection::None => None,
                FfiDirection::LeftToRight => Some(Direction::LeftToRight),
                FfiDirection::RightToLeft => Some(Direction::RightToLeft),
                FfiDirection::Other => Some(Direction::Other),
            };
            let identifier = (selector.identifier.length != 0).then(|| unsafe { string_from_ffi(selector.identifier) });
            let levels = unsafe { ffi_slice(selector.levels, selector.level_count) }.into();

            SimpleSelector::PseudoClass(PseudoClassSelector {
                pseudo_class: pseudo_class_from_ffi(selector.pseudo_class),
                an_plus_b_pattern: AnPlusBPattern {
                    step_size: selector.an_plus_b_step_size,
                    offset: selector.an_plus_b_offset,
                },
                argument_selector_list,
                languages,
                direction,
                identifier,
                levels,
            })
        }
        FfiSimpleSelectorType::PseudoElement => {
            let value = match selector.pseudo_element_value_type {
                FfiPseudoElementValueType::None => PseudoElementValue::None,
                FfiPseudoElementValueType::CompoundSelector => PseudoElementValue::CompoundSelector(unsafe {
                    selector_from_handle(selector.pseudo_element_selector)
                }),
                FfiPseudoElementValueType::Identifiers => PseudoElementValue::Identifiers(unsafe {
                    ffi_slice(
                        selector.pseudo_element_identifiers,
                        selector.pseudo_element_identifier_count,
                    )
                    .iter()
                    .map(|identifier| string_from_ffi(*identifier))
                    .collect::<Vec<_>>()
                    .into_boxed_slice()
                }),
                FfiPseudoElementValueType::TransitionName => PseudoElementValue::TransitionName {
                    is_universal: selector.transition_name_is_universal,
                    value: unsafe { string_from_ffi(selector.transition_name) },
                },
            };
            SimpleSelector::PseudoElement(PseudoElementSelector {
                pseudo_element: pseudo_element_from_ffi(selector.pseudo_element),
                value,
            })
        }
        FfiSimpleSelectorType::Nesting => SimpleSelector::Nesting,
        FfiSimpleSelectorType::Invalid => SimpleSelector::Invalid,
    }
}

unsafe fn compiled_selector_from_ffi(selector: &FfiSelector) -> Arc<CompiledSelector> {
    let compound_selectors = unsafe { ffi_slice(selector.compound_selectors, selector.compound_selector_count) }
        .iter()
        .map(|compound| CompoundSelector {
            combinator: compound.combinator.into(),
            is_implicit_universal_anchor: compound.is_implicit_universal_anchor,
            simple_selectors: unsafe { ffi_slice(compound.simple_selectors, compound.simple_selector_count) }
                .iter()
                .map(|simple| unsafe { simple_selector_from_ffi(simple) })
                .collect::<Vec<_>>()
                .into_boxed_slice(),
        })
        .collect::<Vec<_>>()
        .into_boxed_slice();
    CompiledSelector::new(compound_selectors)
}

/// # Safety
/// `selector` and all transitively referenced arrays and strings must remain valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_create(selector: *const FfiSelector) -> *mut RustSelector {
    abort_on_panic(|| {
        assert!(!selector.is_null());
        let selector = unsafe { compiled_selector_from_ffi(&*selector) };
        Box::into_raw(Box::new(RustSelector { selector }))
    })
}

/// # Safety
/// `selector` must be a pointer returned by `rust_selector_create` that has not already been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_destroy(selector: *mut RustSelector) {
    abort_on_panic(|| {
        if !selector.is_null() {
            drop(unsafe { Box::from_raw(selector) });
        }
    });
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
        SimpleSelector::PseudoElement(_) | SimpleSelector::Nesting | SimpleSelector::Invalid => false,
    }
}

fn sibling_invalidation_distance(compound_selectors: &[CompoundSelector]) -> usize {
    let mut maximum_distance = 0usize;
    let mut current_distance = 0usize;

    for compound_selector in compound_selectors {
        match compound_selector.combinator {
            Combinator::None | Combinator::PseudoElement => {}
            Combinator::SubsequentSibling => return usize::MAX,
            Combinator::NextSibling => current_distance = current_distance.saturating_add(1),
            Combinator::ImmediateChild | Combinator::Descendant | Combinator::Column => {
                maximum_distance = maximum_distance.max(current_distance);
                current_distance = 0;
            }
        }
    }

    maximum_distance.max(current_distance)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn selector_with_combinators(combinators: &[Combinator]) -> Arc<CompiledSelector> {
        let compounds = combinators
            .iter()
            .map(|combinator| CompoundSelector {
                combinator: *combinator,
                is_implicit_universal_anchor: false,
                simple_selectors: vec![SimpleSelector::Id(Box::from([b'x' as u16]))].into_boxed_slice(),
            })
            .collect::<Vec<_>>()
            .into_boxed_slice();
        CompiledSelector::new(compounds)
    }

    #[test]
    fn an_plus_b_matching() {
        assert!(
            AnPlusBPattern {
                step_size: 2,
                offset: 1
            }
            .matches(5)
        );
        assert!(
            !AnPlusBPattern {
                step_size: 2,
                offset: 1
            }
            .matches(4)
        );
        assert!(
            AnPlusBPattern {
                step_size: -1,
                offset: 3
            }
            .matches(1)
        );
        assert!(
            !AnPlusBPattern {
                step_size: -1,
                offset: 3
            }
            .matches(4)
        );
        assert!(
            AnPlusBPattern {
                step_size: 0,
                offset: 2
            }
            .matches(2)
        );
    }

    #[test]
    fn computes_fast_match_eligibility() {
        let fast = selector_with_combinators(&[Combinator::None, Combinator::Descendant]);
        assert!(fast.can_use_fast_matches);

        let pseudo = CompiledSelector::new(Box::new([CompoundSelector {
            combinator: Combinator::None,
            is_implicit_universal_anchor: false,
            simple_selectors: Box::new([SimpleSelector::PseudoClass(PseudoClassSelector::without_arguments(
                PseudoClassType::NthChild,
            ))]),
        }]));
        assert!(!pseudo.can_use_fast_matches);
    }

    #[test]
    fn computes_sibling_invalidation_distance() {
        let adjacent = selector_with_combinators(&[
            Combinator::None,
            Combinator::NextSibling,
            Combinator::NextSibling,
            Combinator::ImmediateChild,
        ]);
        assert_eq!(adjacent.sibling_invalidation_distance, 2);

        let subsequent = selector_with_combinators(&[Combinator::None, Combinator::SubsequentSibling]);
        assert_eq!(subsequent.sibling_invalidation_distance, usize::MAX);
    }

    #[test]
    fn assigns_unique_nonzero_ids() {
        let first = selector_with_combinators(&[Combinator::None]);
        let second = selector_with_combinators(&[Combinator::None]);
        assert_ne!(first.id(), 0);
        assert_ne!(first.id(), second.id());
        assert_eq!(first, second);
    }
}
