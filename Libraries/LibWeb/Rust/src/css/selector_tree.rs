/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tree a selector parses into, which the parser builds and packs into a selector arena as it
//! finishes a list. Nothing keeps it after the parse.

use smallvec::SmallVec;
use std::sync::Arc;

use crate::css::selector::{
    AnPlusBPattern, AttributeCaseType, AttributeMatchType, Combinator, Direction, NameSelector, PseudoClassType,
    PseudoElementType, QualifiedName, SearchTextMatchFilter, SelectorString,
};
use crate::css::style::index::StyleAtomID;

pub(crate) type SelectorList = Box<[Arc<CompiledSelector>]>;

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct AttributeSelector {
    pub match_type: AttributeMatchType,
    pub qualified_name: QualifiedName,
    pub value: SelectorString,
    /// The value as a process style atom, or none for a presence test.
    pub value_identity: StyleAtomID,
    pub case_type: AttributeCaseType,
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
    /// The identifier as a process style atom, where the pseudo-class names a state by it.
    pub identifier_identity: StyleAtomID,
    pub levels: Box<[i64]>,
    pub is_forgiving: bool,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum PseudoElementValue {
    None,
    CompoundSelector(Arc<CompiledSelector>),
    Identifiers(Box<[SelectorString]>),
    TransitionName(Box<TransitionNameAndClassSelector>),
    Universal,
}

// https://drafts.csswg.org/css-view-transitions-1/#typedef-pt-name-selector
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum TransitionNameSelector {
    Universal,
    Name(SelectorString),
}

// https://drafts.csswg.org/css-view-transitions-2/#typedef-pt-name-and-class-selector
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct TransitionNameAndClassSelector {
    pub name: Option<TransitionNameSelector>,
    pub classes: Box<[SelectorString]>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct PseudoElementSelector {
    pub pseudo_element: PseudoElementType,
    pub serialized_name: Option<SelectorString>,
    pub value: PseudoElementValue,
    /// The process style atom of each name in `value`, when it holds identifiers.
    pub identifier_identities: Box<[StyleAtomID]>,
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
    /// The start of a compound in a complex selector's flat components, with the combinator
    /// relating it to the compound left of it. It never appears among a compound's simple
    /// selectors.
    Compound {
        combinator: Combinator,
        is_implicit_universal_anchor: bool,
    },
}

// Keep every simple selector to one word, plus the selector tag.
const _: () = assert!(size_of::<SimpleSelector>() == 2 * size_of::<usize>());

/// A compound selector to build a complex selector from.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct CompoundSelector {
    /// The combinator relating this compound to the compound immediately to its left. The
    /// leftmost compound therefore has `Combinator::None`.
    pub combinator: Combinator,
    pub is_implicit_universal_anchor: bool,
    pub simple_selectors: Box<[SimpleSelector]>,
}

/// One compound of a complex selector, borrowed from its flat components.
#[derive(Clone, Copy, Debug)]
pub struct CompoundRef<'a> {
    /// The combinator relating this compound to the compound immediately to its left. The
    /// leftmost compound therefore has `Combinator::None`.
    pub combinator: Combinator,
    pub is_implicit_universal_anchor: bool,
    pub simple_selectors: &'a [SimpleSelector],
}

impl CompoundRef<'_> {
    #[must_use]
    pub fn to_compound(self) -> CompoundSelector {
        CompoundSelector {
            combinator: self.combinator,
            is_implicit_universal_anchor: self.is_implicit_universal_anchor,
            simple_selectors: self.simple_selectors.into(),
        }
    }
}

/// One complex selector of the tree, its compounds flat: each compound is a
/// [`SimpleSelector::Compound`] naming its combinator, followed by its simple selectors.
#[derive(Debug, PartialEq, Eq)]
pub struct CompiledSelector {
    components: Box<[SimpleSelector]>,
}

impl CompiledSelector {
    pub(crate) fn new(compound_selectors: Box<[CompoundSelector]>) -> Arc<Self> {
        let count = compound_selectors
            .iter()
            .map(|compound| compound.simple_selectors.len() + 1)
            .sum();
        let mut components = Vec::with_capacity(count);
        for compound in compound_selectors {
            components.push(SimpleSelector::Compound {
                combinator: compound.combinator,
                is_implicit_universal_anchor: compound.is_implicit_universal_anchor,
            });
            components.extend(compound.simple_selectors);
        }
        Arc::new(Self {
            components: components.into_boxed_slice(),
        })
    }

    /// The compounds of the selector, left to right.
    #[must_use]
    pub fn compound_selectors(&self) -> SmallVec<[CompoundRef<'_>; 4]> {
        let mut compounds = SmallVec::new();
        let mut index = 0;
        while index < self.components.len() {
            let SimpleSelector::Compound {
                combinator,
                is_implicit_universal_anchor,
            } = self.components[index]
            else {
                unreachable!("a compound starts with its combinator");
            };
            let start = index + 1;
            index = start;
            while index < self.components.len() && !matches!(self.components[index], SimpleSelector::Compound { .. }) {
                index += 1;
            }
            compounds.push(CompoundRef {
                combinator,
                is_implicit_universal_anchor,
                simple_selectors: &self.components[start..index],
            });
        }
        compounds
    }

    /// The compounds of the selector, to build another from.
    #[must_use]
    pub fn to_compounds(&self) -> Box<[CompoundSelector]> {
        self.compound_selectors()
            .iter()
            .map(|compound| compound.to_compound())
            .collect()
    }
}

fn search_text_match_filter_of_pseudo_class(pseudo_class: &PseudoClassSelector) -> Option<SearchTextMatchFilter> {
    match pseudo_class.pseudo_class {
        PseudoClassType::Current => Some(SearchTextMatchFilter::Current),
        PseudoClassType::Not => {
            let [argument] = &*pseudo_class.argument_selector_list else {
                return None;
            };
            let compounds = argument.compound_selectors();
            let [compound] = &*compounds else {
                return None;
            };
            matches!(
                compound.simple_selectors,
                [SimpleSelector::PseudoClass(inner)] if inner.pseudo_class == PseudoClassType::Current
            )
            .then_some(SearchTextMatchFilter::NotCurrent)
        }
        _ => None,
    }
}

pub(crate) fn search_text_match_filter(simple: &SimpleSelector) -> Option<SearchTextMatchFilter> {
    match simple {
        SimpleSelector::PseudoClass(pseudo_class) => search_text_match_filter_of_pseudo_class(pseudo_class),
        _ => None,
    }
}

/// Whether the selector's subject is a pseudo-element, which the parser checks where one is not allowed.
pub(crate) fn target_pseudo_element(selector: &CompiledSelector) -> Option<PseudoElementType> {
    let compounds = selector.compound_selectors();
    let last = compounds.last()?;
    match last.simple_selectors.first()? {
        SimpleSelector::PseudoElement(selector)
            if !matches!(
                selector.pseudo_element,
                PseudoElementType::Part | PseudoElementType::Slotted
            ) =>
        {
            Some(selector.pseudo_element)
        }
        _ => None,
    }
}
