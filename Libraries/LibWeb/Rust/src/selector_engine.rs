/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::sync::Arc;
use std::sync::atomic::{AtomicU64, Ordering};

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
