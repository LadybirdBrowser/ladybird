/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS selector parsing.

use crate::css::ffi_support::OwnedBox;
use smallvec::SmallVec;
use std::sync::Arc;

use super::css_tokenizer::{TokenizerInput, filter_code_points};
use super::ffi_support::FfiUtf16View;
use super::ffi_support::ascii_lowercase;
use super::parser::component_value::ComponentValue;
use super::parser::rows::{Cv, Name, Rows, Values};
#[cfg(test)]
use super::selector::FfiStringView;
use super::selector::{
    AnPlusBPattern, AttributeCaseType, AttributeMatchType, Combinator, Direction, NamespaceType,
    PseudoClassParameterType, PseudoClassType, PseudoElementParameterType, PseudoElementType, SelectorString,
};
use super::selector::{NameSelector, QualifiedName, RustSelector, SelectorListRef, SelectorRef};
use super::selector_tree::{
    AttributeSelector, CompiledSelector, CompoundSelector, LanguageRange, PseudoClassSelector, PseudoElementSelector,
    PseudoElementValue, SelectorList, SimpleSelector, SimpleSelectorVec, TransitionNameAndClassSelector,
    TransitionNameSelector, search_text_match_filter,
};
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::style::AtomLease;
use crate::css::style::index::StyleAtomID;

const MAXIMUM_SELECTOR_NESTING_DEPTH: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelectorType {
    Standalone,
    Relative,
}

pub use super::ffi_support::StyleNestingParent;
use super::selector_arena::{NestingTarget, RuleOrdinal};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelectorParsingMode {
    Standard,
    Forgiving,
}

fn ascii_eq(value: &[u16], expected: &str) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected.bytes())
            .all(|(&unit, byte)| ascii_lowercase(unit) == u16::from(byte.to_ascii_lowercase()))
}

fn ascii_starts_with(value: &[u16], expected: &str) -> bool {
    value.len() >= expected.len() && ascii_eq(&value[..expected.len()], expected)
}

fn lowercase(value: &[u16]) -> SelectorString {
    value
        .iter()
        .map(|&unit| ascii_lowercase(unit))
        .collect::<Vec<_>>()
        .into()
}

fn pseudo_class_selector(pseudo_class: PseudoClassType) -> PseudoClassSelector {
    PseudoClassSelector {
        pseudo_class,
        an_plus_b_pattern: AnPlusBPattern::default(),
        argument_selector_list: Box::new([]),
        languages: Box::new([]),
        direction: None,
        identifier: None,
        identifier_identity: StyleAtomID::NONE,
        levels: Box::new([]),
        is_forgiving: false,
    }
}

fn split_on_commas(mut values: Values<'_>) -> Vec<Values<'_>> {
    let mut result = Vec::new();
    let mut is_first = true;
    while let Some(part) = values.split_at_comma(is_first) {
        is_first = false;
        result.push(part);
    }
    result
}

fn is_css_wide_keyword(value: &[u16]) -> bool {
    ["initial", "inherit", "unset", "revert", "revert-layer"]
        .iter()
        .any(|keyword| ascii_eq(value, keyword))
}

fn clamp_integer(value: f64) -> i32 {
    value.clamp(f64::from(i32::MIN), f64::from(i32::MAX)) as i32
}

/// Takes the next value of `stream`, and answers whether it is the identifier `expected`.
#[inline(never)]
fn next_is_ident(stream: &mut Values<'_>, expected: &str) -> bool {
    stream
        .next()
        .and_then(|value| value.ident())
        .is_some_and(|ident| ascii_eq(&ident, expected))
}

fn integer(value: Cv<'_>) -> Option<(i32, bool)> {
    let (value, is_integer, is_signed) = value.number()?;
    is_integer.then(|| (clamp_integer(value), is_signed))
}

fn dimension(value: Cv<'_>) -> Option<(i32, Name)> {
    let (value, is_integer, unit) = value.dimension()?;
    is_integer.then(|| (clamp_integer(value), unit))
}

fn ascii_i32_saturating(value: &[u16]) -> Option<i32> {
    let (negative, digits) = match value {
        [sign, digits @ ..] if *sign == b'-' as u16 => (true, digits),
        [sign, digits @ ..] if *sign == b'+' as u16 => (false, digits),
        _ => (false, value),
    };
    if digits.is_empty() {
        return None;
    }
    digits.iter().try_fold(0i32, |result, &unit| {
        let digit = i32::from(u8::try_from(unit).ok()?.checked_sub(b'0')?);
        if digit > 9 {
            return None;
        }
        Some(if negative {
            result.saturating_mul(10).saturating_sub(digit)
        } else {
            result.saturating_mul(10).saturating_add(digit)
        })
    })
}

fn digits(value: &[u16]) -> bool {
    !value.is_empty() && value.iter().all(|unit| (b'0' as u16..=b'9' as u16).contains(unit))
}

fn parse_an_plus_b_tail(stream: &mut Values<'_>, step_size: i32) -> AnPlusBPattern {
    let after_step = *stream;
    stream.discard_whitespace();
    if let Some((offset, true)) = stream.peek().and_then(integer) {
        stream.skip();
        return AnPlusBPattern { step_size, offset };
    }
    let sign = if stream.peek().is_some_and(|value| value.is_delim(b'+')) {
        1
    } else if stream.peek().is_some_and(|value| value.is_delim(b'-')) {
        -1
    } else {
        *stream = after_step;
        return AnPlusBPattern { step_size, offset: 0 };
    };
    stream.skip();
    stream.discard_whitespace();
    if let Some((offset, false)) = stream.peek().and_then(integer) {
        stream.skip();
        return AnPlusBPattern {
            step_size,
            offset: offset.saturating_mul(sign),
        };
    }
    *stream = after_step;
    AnPlusBPattern { step_size, offset: 0 }
}

// https://drafts.csswg.org/css-syntax-3/#anb-microsyntax
fn parse_an_plus_b(stream: &mut Values<'_>) -> Option<AnPlusBPattern> {
    let original = *stream;
    stream.discard_whitespace();
    if stream
        .peek()
        .and_then(|value| value.ident())
        .is_some_and(|ident| ascii_eq(&ident, "odd"))
    {
        stream.skip();
        return Some(AnPlusBPattern {
            step_size: 2,
            offset: 1,
        });
    }
    if stream
        .peek()
        .and_then(|value| value.ident())
        .is_some_and(|ident| ascii_eq(&ident, "even"))
    {
        stream.skip();
        return Some(AnPlusBPattern {
            step_size: 2,
            offset: 0,
        });
    }
    if let Some((offset, _)) = stream.peek().and_then(integer) {
        stream.skip();
        return Some(AnPlusBPattern { step_size: 0, offset });
    }
    if let Some((step_size, unit)) = stream.peek().and_then(dimension) {
        if ascii_eq(&unit, "n") {
            stream.skip();
            return Some(parse_an_plus_b_tail(stream, step_size));
        }
        if ascii_eq(&unit, "n-") {
            stream.skip();
            if let Some((offset, false)) = stream.peek().and_then(integer) {
                stream.skip();
                return Some(AnPlusBPattern {
                    step_size,
                    offset: offset.saturating_neg(),
                });
            }
            *stream = original;
            return None;
        }
        if unit.len() > 2 && ascii_starts_with(&unit, "n-") && digits(&unit[2..]) {
            let offset = ascii_i32_saturating(&unit[1..])?;
            stream.skip();
            return Some(AnPlusBPattern { step_size, offset });
        }
    }
    if let Some(ident) = stream.peek().and_then(|value| value.ident()) {
        if ident.len() > 3 && ascii_starts_with(&ident, "-n-") && digits(&ident[3..]) {
            let offset = ascii_i32_saturating(&ident[2..])?;
            stream.skip();
            return Some(AnPlusBPattern { step_size: -1, offset });
        }
        if ascii_eq(&ident, "-n") {
            stream.skip();
            return Some(parse_an_plus_b_tail(stream, -1));
        }
        if ascii_eq(&ident, "-n-") {
            stream.skip();
            stream.discard_whitespace();
            if let Some((offset, false)) = stream.peek().and_then(integer) {
                stream.skip();
                return Some(AnPlusBPattern {
                    step_size: -1,
                    offset: offset.saturating_neg(),
                });
            }
            *stream = original;
            return None;
        }
    }

    if stream.peek().is_some_and(|value| value.is_delim(b'+')) {
        stream.skip();
    }
    let ident = stream.next().and_then(|value| value.ident());
    let ident = ident.as_deref();
    if ident.is_some_and(|ident| ascii_eq(ident, "n")) {
        return Some(parse_an_plus_b_tail(stream, 1));
    }
    if ident.is_some_and(|ident| ascii_eq(ident, "n-")) {
        stream.discard_whitespace();
        if let Some((offset, false)) = stream.peek().and_then(integer) {
            stream.skip();
            return Some(AnPlusBPattern {
                step_size: 1,
                offset: offset.saturating_neg(),
            });
        }
    }
    if let Some(ident) = ident
        && ident.len() > 2
        && ascii_starts_with(ident, "n-")
        && digits(&ident[2..])
    {
        let offset = ascii_i32_saturating(&ident[1..])?;
        return Some(AnPlusBPattern { step_size: 1, offset });
    }
    *stream = original;
    None
}

/// The process style atom of each name that the selectors of one parse name, with one reference to each. A sheet says
/// the same names many times, and the process atom table then sees each of them once.
#[derive(Default)]
pub(crate) struct NameCache {
    /// Where each name is in `units`, with its hash and its atom.
    atoms: hashbrown::HashTable<(u64, u32, u32, StyleAtomID)>,
    /// The units of all the names, one after the other.
    units: Vec<u16>,
    /// Keyed at random, so that a page cannot make many names fall into one bucket.
    hasher: foldhash::fast::RandomState,
    /// A reference to each atom, which keeps it live until the arena of the selectors leases it. Whoever holds the
    /// cache drops it after that.
    leases: Vec<AtomLease>,
}

impl NameCache {
    fn name(&mut self, value: &[u16]) -> StyleAtomID {
        use std::hash::BuildHasher;
        let hash = self.hasher.hash_one(value);
        let units = &self.units;
        if let Some(&(.., atom)) = self.atoms.find(hash, |&(entry_hash, start, length, _)| {
            entry_hash == hash && units[start as usize..(start + length) as usize] == *value
        }) {
            return atom;
        }
        let lease = crate::css::style::process_atoms().intern(value);
        let atom = lease.atom();
        self.leases.push(lease);
        let start = u32::try_from(self.units.len()).expect("the names of a sheet fit in 32 bits");
        self.units.extend_from_slice(value);
        self.atoms
            .insert_unique(hash, (hash, start, value.len() as u32, atom), |entry| entry.0);
        atom
    }
}

struct SelectorParser<'a> {
    declared_namespaces: Option<&'a [TokenizerInput<'a>]>,
    name_cache: Option<&'a std::cell::RefCell<NameCache>>,
    pseudo_class_context: Vec<PseudoClassType>,
    nesting_limit_exceeded: bool,
    compound_pseudo_element: Option<PseudoElementType>,
    bare_current_allowed: bool,
    /// A reference to each name the parse interned, which keeps its atom live until the selector
    /// naming it leases it.
    names: std::cell::RefCell<Vec<AtomLease>>,
}

impl<'a> SelectorParser<'a> {
    fn new(declared_namespaces: Option<&'a [TokenizerInput<'a>]>) -> Self {
        Self {
            declared_namespaces,
            name_cache: None,
            pseudo_class_context: Vec::new(),
            nesting_limit_exceeded: false,
            compound_pseudo_element: None,
            bare_current_allowed: false,
            names: std::cell::RefCell::default(),
        }
    }

    /// The process style atom of the name `value` spells.
    fn name(&self, value: &[u16]) -> StyleAtomID {
        if let Some(cache) = self.name_cache {
            return cache.borrow_mut().name(value);
        }
        let lease = crate::css::style::process_atoms().intern(value);
        let atom = lease.atom();
        self.names.borrow_mut().push(lease);
        atom
    }

    /// The process style atom of a name a qualified name holds, none for `*`, which names any.
    fn qualified_name_part(&self, value: &[u16]) -> StyleAtomID {
        match value == [b'*' as u16] {
            true => StyleAtomID::NONE,
            false => self.name(value),
        }
    }

    fn parse_selector_list(
        &mut self,
        values: Values<'_>,
        selector_type: SelectorType,
        parsing_mode: SelectorParsingMode,
    ) -> Result<SelectorList, ()> {
        // NB: This is the list of a pseudo-class, which a selector can nest to the depth limit. Its lists are small
        //     here, so that the stack of the parser does not hold the inline memory of a list at each depth.
        self.parse_selectors_in::<1, 1>(values, selector_type, parsing_mode)
            .map(|selectors| selectors.into_iter().collect())
    }

    /// Parses a selector list into inline memory, for a list that goes into an arena at once.
    fn parse_selectors(
        &mut self,
        values: Values<'_>,
        selector_type: SelectorType,
        parsing_mode: SelectorParsingMode,
    ) -> Result<ParsedSelectors, ()> {
        self.parse_selectors_in::<3, 4>(values, selector_type, parsing_mode)
    }

    /// Parses a selector list. `SELECTORS` selectors, and `COMPOUNDS` compound selectors of each, are in place.
    fn parse_selectors_in<const SELECTORS: usize, const COMPOUNDS: usize>(
        &mut self,
        values: Values<'_>,
        selector_type: SelectorType,
        parsing_mode: SelectorParsingMode,
    ) -> Result<SmallVec<[CompiledSelector; SELECTORS]>, ()> {
        if self.pseudo_class_context.len() > MAXIMUM_SELECTOR_NESTING_DEPTH {
            self.nesting_limit_exceeded = true;
            return Err(());
        }
        let mut selectors = SmallVec::new();
        let mut rest = values;
        let mut is_first = true;
        while let Some(selector_values) = rest.split_at_comma(is_first) {
            is_first = false;
            let selector = self.parse_complex_selector::<COMPOUNDS>(selector_values, selector_type);
            if self.nesting_limit_exceeded {
                return Err(());
            }
            match selector {
                Ok(selector) => selectors.push(selector),
                Err(()) if parsing_mode == SelectorParsingMode::Forgiving => {
                    let source = selector_values.source_text(selector_values.source_range());
                    selectors.push(CompiledSelector::from_compounds::<1>(smallvec::smallvec![
                        CompoundSelector {
                            combinator: if selector_type == SelectorType::Standalone {
                                Combinator::None
                            } else {
                                Combinator::Descendant
                            },
                            is_implicit_universal_anchor: false,
                            simple_selectors: smallvec::smallvec![SimpleSelector::Invalid(source.into())],
                        }
                    ]));
                }
                Err(()) => return Err(()),
            }
        }
        if selectors.is_empty() && parsing_mode != SelectorParsingMode::Forgiving {
            return Err(());
        }
        Ok(selectors)
    }

    fn parse_complex_selector<const COMPOUNDS: usize>(
        &mut self,
        values: Values<'_>,
        selector_type: SelectorType,
    ) -> Result<CompiledSelector, ()> {
        let mut stream = values;
        let mut first_combinator = self.parse_combinator(&mut stream);
        match selector_type {
            SelectorType::Standalone => {
                if first_combinator.is_some_and(|combinator| combinator != Combinator::Descendant) {
                    return Err(());
                }
                first_combinator = Some(Combinator::None);
            }
            SelectorType::Relative if first_combinator.is_none() => {
                first_combinator = Some(Combinator::Descendant);
            }
            SelectorType::Relative => {}
        }

        let mut first = self.parse_compound_selector(&mut stream)?;
        if first.simple_selectors.is_empty() {
            return Err(());
        }
        first.combinator = first_combinator.unwrap_or(Combinator::None);
        let mut compounds = SmallVec::<[CompoundSelector; COMPOUNDS]>::new();
        compounds.push(first);

        while !stream.is_empty() {
            let Some(combinator) = self.parse_combinator(&mut stream) else {
                break;
            };
            let mut compound = self.parse_compound_selector(&mut stream)?;
            if compound.simple_selectors.is_empty() {
                if !stream.is_empty() || combinator != Combinator::Descendant {
                    return Err(());
                }
                break;
            }
            compound.combinator = combinator;
            compounds.push(compound);
        }
        if !stream.is_empty() {
            return Err(());
        }

        Self::compile_complex_selector(compounds)
    }

    /// Checks the compound selectors of a complex selector, and makes the selector of them.
    // NB: This is not part of the function that a nested selector calls again, so that its memory is not on the stack
    //     at each depth.
    #[inline(never)]
    fn compile_complex_selector<const COMPOUNDS: usize>(
        compounds: SmallVec<[CompoundSelector; COMPOUNDS]>,
    ) -> Result<CompiledSelector, ()> {
        let compounds = normalize_pseudo_element_transitions(compounds);
        // NB: Only the first two pseudo-elements and their count decide whether the selector is valid.
        let mut pseudo_elements = [PseudoElementType::Part; 2];
        let mut pseudo_element_count = 0_usize;
        let mut saw_pseudo_element_transition = false;
        for compound in &compounds {
            if saw_pseudo_element_transition && compound.combinator != Combinator::PseudoElement {
                return Err(());
            }
            saw_pseudo_element_transition |= compound.combinator == Combinator::PseudoElement;
            if let Some(SimpleSelector::PseudoElement(pseudo_element)) = compound.simple_selectors.first() {
                if let Some(slot) = pseudo_elements.get_mut(pseudo_element_count) {
                    *slot = pseudo_element.pseudo_element;
                }
                pseudo_element_count += 1;
            }
        }
        if pseudo_element_count > 1
            && !(pseudo_element_count == 2
                && pseudo_elements[0] == PseudoElementType::Part
                && pseudo_elements[1] != PseudoElementType::Part)
        {
            return Err(());
        }
        for compound in &compounds {
            if !matches!(compound.simple_selectors.first(), Some(SimpleSelector::PseudoElement(pseudo_element))
                if pseudo_element.pseudo_element == PseudoElementType::SearchText)
            {
                continue;
            }
            match &compound.simple_selectors[1..] {
                [] => {}
                [filter] if search_text_match_filter(filter).is_some() => {}
                _ => return Err(()),
            }
        }
        for compound in &compounds {
            let Some(SimpleSelector::PseudoElement(pseudo_element)) = compound.simple_selectors.first() else {
                continue;
            };
            // https://drafts.csswg.org/css-shadow-1/#slotted-pseudo
            // INTEROP: Selectors 4 permits logical and user action pseudo-classes after pseudo-elements, but
            //          browsers require these tests inside the ::slotted() argument.
            if pseudo_element.pseudo_element == PseudoElementType::Slotted && compound.simple_selectors.len() > 1 {
                return Err(());
            }
            // https://drafts.csswg.org/css-pseudo-4/#highlight-selectors
            // INTEROP: Selectors 4 allows logical combination and user action pseudo-classes after any
            //          pseudo-element, but browsers reject every simple selector after a highlight pseudo-element.
            //          ::search-text is checked above, as it allows :current.
            if matches!(
                pseudo_element.pseudo_element,
                PseudoElementType::Highlight | PseudoElementType::Selection | PseudoElementType::SpellingError
            ) && compound.simple_selectors.len() > 1
            {
                return Err(());
            }
            // https://drafts.csswg.org/css-view-transitions-1/#pseudo-root
            // If a descendant pseudo of a pseudo-element root has no other siblings, then :only-child matches that
            // pseudo.
            // INTEROP: Selectors 4 allows logical combination and user action pseudo-classes after any
            //          pseudo-element, but browsers reject every pseudo-class other than :only-child after a view
            //          transition pseudo-element, and none at all after ::view-transition.
            let allows_only_child = match pseudo_element.pseudo_element {
                PseudoElementType::ViewTransition => false,
                PseudoElementType::ViewTransitionGroup
                | PseudoElementType::ViewTransitionImagePair
                | PseudoElementType::ViewTransitionNew
                | PseudoElementType::ViewTransitionOld => true,
                _ => continue,
            };
            let is_only_child = |simple_selector: &SimpleSelector| {
                matches!(simple_selector, SimpleSelector::PseudoClass(pseudo_class)
                    if pseudo_class.pseudo_class == PseudoClassType::OnlyChild)
            };
            if !compound.simple_selectors[1..]
                .iter()
                .all(|simple_selector| allows_only_child && is_only_child(simple_selector))
            {
                return Err(());
            }
        }
        Ok(CompiledSelector::from_compounds(compounds))
    }

    // NB: This keeps the memory for a name out of the stack of the functions that a nested selector calls again.
    #[inline(never)]
    fn parse_qualified_name(&self, stream: &mut Values<'_>, allow_wildcard: bool) -> Option<QualifiedName> {
        fn name(value: Cv<'_>) -> Option<Name> {
            if value.is_delim(b'*') {
                return Some(Name::from_slice(&[b'*' as u16]));
            }
            value.ident()
        }

        let original = *stream;
        let first = stream.next()?;
        if first.is_delim(b'|') {
            let parsed_name = stream.next().and_then(name)?;
            if !allow_wildcard && parsed_name[..] == [b'*' as u16] {
                *stream = original;
                return None;
            }
            return Some(QualifiedName::new(
                NamespaceType::None,
                StyleAtomID::NONE,
                self.qualified_name_part(&parsed_name),
            ));
        }

        let Some(first_name) = name(first) else {
            *stream = original;
            return None;
        };
        if stream.peek().is_some_and(|value| value.is_delim(b'|')) && stream.peek_n(1).and_then(name).is_some() {
            stream.skip();
            let parsed_name = name(stream.next().unwrap()).unwrap();
            if !allow_wildcard && parsed_name[..] == [b'*' as u16] {
                *stream = original;
                return None;
            }
            let namespace_type = if first_name[..] == [b'*' as u16] {
                NamespaceType::Any
            } else {
                NamespaceType::Named
            };
            if namespace_type == NamespaceType::Named
                && self.declared_namespaces.is_some_and(|declared_namespaces| {
                    !declared_namespaces.iter().any(|namespace| {
                        namespace.len() == first_name.len()
                            && first_name
                                .iter()
                                .enumerate()
                                .all(|(index, code_unit)| namespace.code_unit_at(index) == *code_unit)
                    })
                })
            {
                *stream = original;
                return None;
            }
            return Some(QualifiedName::new(
                namespace_type,
                if namespace_type == NamespaceType::Named {
                    self.name(&first_name)
                } else {
                    StyleAtomID::NONE
                },
                self.qualified_name_part(&parsed_name),
            ));
        }
        if !allow_wildcard && first_name[..] == [b'*' as u16] {
            *stream = original;
            return None;
        }
        Some(QualifiedName::new(
            NamespaceType::Default,
            StyleAtomID::NONE,
            self.qualified_name_part(&first_name),
        ))
    }

    #[inline(never)]
    fn next_is_pseudo_element(&self, stream: &Values<'_>) -> bool {
        if !stream.peek().is_some_and(|value| value.is_colon()) {
            return false;
        }
        if stream.peek_n(1).is_some_and(|value| value.is_colon()) {
            return true;
        }
        let Some(name) = stream.peek_n(1).and_then(|value| value.ident()) else {
            return false;
        };
        PseudoElementType::from_name(&name).is_some_and(|(pseudo_element, _)| {
            matches!(
                pseudo_element,
                PseudoElementType::After
                    | PseudoElementType::Before
                    | PseudoElementType::FirstLetter
                    | PseudoElementType::FirstLine
            )
        })
    }

    fn parse_simple_selector(&mut self, stream: &mut Values<'_>) -> Result<Option<SimpleSelector>, ()> {
        if stream
            .peek()
            .is_none_or(|value| value.is_whitespace() || value.is_comma())
        {
            return Ok(None);
        }

        let original = *stream;
        if let Some(qualified_name) = self.parse_qualified_name(stream, true) {
            return Ok(Some(if qualified_name.name.is_none() {
                SimpleSelector::Universal(Box::new(qualified_name))
            } else {
                SimpleSelector::TagName(Box::new(qualified_name))
            }));
        }
        *stream = original;

        if self.next_is_pseudo_element(stream) {
            return self.parse_pseudo_element(stream).map(Some);
        }
        if stream.peek().is_some_and(|value| value.is_colon()) {
            return self.parse_pseudo_class(stream).map(Some);
        }
        if stream.peek().is_some_and(|value| {
            value.is_delim(b'>') || value.is_delim(b'+') || value.is_delim(b'~') || value.is_delim(b'|')
        }) {
            return Ok(None);
        }

        let first = stream.next().ok_or(())?;
        if first.is_delim(b'&') {
            return Ok(Some(SimpleSelector::Nesting));
        }
        if first.is_delim(b'.') {
            return self.parse_class(stream.next()).map(Some);
        }
        if let Some(id) = self.parse_id(first) {
            return Ok(Some(id));
        }
        if let Some(values) = first.square_block() {
            return self.parse_attribute(values).map(Some);
        }
        Err(())
    }

    #[inline(never)]
    fn parse_class(&self, name: Option<Cv<'_>>) -> Result<SimpleSelector, ()> {
        let class_name = name.and_then(|value| value.ident()).ok_or(())?;
        Ok(SimpleSelector::Class(NameSelector::new(self.name(&class_name))))
    }

    #[inline(never)]
    fn parse_id(&self, value: Cv<'_>) -> Option<SimpleSelector> {
        let id = value.id()?;
        Some(SimpleSelector::Id(NameSelector::new(self.name(&id))))
    }

    /// The pseudo-class that `value` names, and the contents of `value` if it is a function.
    #[inline(never)]
    fn pseudo_class_named(value: Cv<'_>) -> Result<(PseudoClassType, Option<Values<'_>>), ()> {
        if let Some(name) = value.ident() {
            return Ok((PseudoClassType::from_name(&name).ok_or(())?, None));
        }
        let (name, function_values) = value.function().ok_or(())?;
        Ok((PseudoClassType::from_name(&name).ok_or(())?, Some(function_values)))
    }

    #[inline(never)]
    fn parse_attribute(&self, values: Values<'_>) -> Result<SimpleSelector, ()> {
        let mut stream = values;
        stream.discard_whitespace();
        let qualified_name = self.parse_qualified_name(&mut stream, false).ok_or(())?;
        stream.discard_whitespace();
        if stream.is_empty() {
            return Ok(SimpleSelector::Attribute(Box::new(AttributeSelector {
                match_type: AttributeMatchType::HasAttribute,
                qualified_name,
                value: SelectorString::default(),
                value_identity: StyleAtomID::NONE,
                case_type: AttributeCaseType::Default,
            })));
        }

        let first = stream.next().ok_or(())?;
        let match_type = if first.is_delim(b'=') {
            AttributeMatchType::ExactValue
        } else {
            let second = stream.next().ok_or(())?;
            if !second.is_delim(b'=') {
                return Err(());
            }
            if first.is_delim(b'~') {
                AttributeMatchType::ContainsWord
            } else if first.is_delim(b'*') {
                AttributeMatchType::ContainsString
            } else if first.is_delim(b'|') {
                AttributeMatchType::StartsWithSegment
            } else if first.is_delim(b'^') {
                AttributeMatchType::StartsWithString
            } else if first.is_delim(b'$') {
                AttributeMatchType::EndsWithString
            } else {
                return Err(());
            }
        };
        stream.discard_whitespace();
        let value: SelectorString = stream
            .next()
            .and_then(|value| value.ident().or_else(|| value.string()))
            .ok_or(())?
            .as_slice()
            .into();
        stream.discard_whitespace();
        let case_type = match stream.next() {
            None => AttributeCaseType::Default,
            Some(value) if value.ident().is_some_and(|ident| ascii_eq(&ident, "i")) => AttributeCaseType::Insensitive,
            Some(value) if value.ident().is_some_and(|ident| ascii_eq(&ident, "s")) => AttributeCaseType::Sensitive,
            Some(_) => return Err(()),
        };
        stream.discard_whitespace();
        if !stream.is_empty() {
            return Err(());
        }
        Ok(SimpleSelector::Attribute(Box::new(AttributeSelector {
            match_type,
            qualified_name,
            value_identity: self.name(value.units()),
            value,
            case_type,
        })))
    }

    fn parse_pseudo_class(&mut self, stream: &mut Values<'_>) -> Result<SimpleSelector, ()> {
        if !stream.next().is_some_and(|value| value.is_colon()) {
            return Err(());
        }
        let (pseudo_class, function_values) = Self::pseudo_class_named(stream.next().ok_or(())?)?;
        let Some(function_values) = function_values else {
            if !pseudo_class.metadata().is_valid_as_identifier {
                return Err(());
            }
            if pseudo_class == PseudoClassType::Current
                && self.compound_pseudo_element != Some(PseudoElementType::SearchText)
                && !self.bare_current_allowed
            {
                return Err(());
            }
            return Ok(SimpleSelector::PseudoClass(Box::new(pseudo_class_selector(
                pseudo_class,
            ))));
        };
        let metadata = pseudo_class.metadata();
        if !metadata.is_valid_as_function || function_values.is_empty() {
            return Err(());
        }
        if pseudo_class == PseudoClassType::Has && self.pseudo_class_context.contains(&PseudoClassType::Has) {
            return Err(());
        }
        if self.pseudo_class_context.len() >= MAXIMUM_SELECTOR_NESTING_DEPTH {
            self.nesting_limit_exceeded = true;
            return Err(());
        }

        let bare_current_allowed = std::mem::replace(
            &mut self.bare_current_allowed,
            pseudo_class == PseudoClassType::Not && self.compound_pseudo_element == Some(PseudoElementType::SearchText),
        );
        self.pseudo_class_context.push(pseudo_class);
        let result = self.parse_pseudo_class_function(pseudo_class, metadata.parameter_type, function_values);
        self.pseudo_class_context.pop();
        self.bare_current_allowed = bare_current_allowed;
        result.map(SimpleSelector::PseudoClass)
    }

    fn parse_pseudo_class_function(
        &mut self,
        pseudo_class: PseudoClassType,
        parameter_type: PseudoClassParameterType,
        values: Values<'_>,
    ) -> Result<Box<PseudoClassSelector>, ()> {
        if self.pseudo_class_context.len() > MAXIMUM_SELECTOR_NESTING_DEPTH {
            self.nesting_limit_exceeded = true;
            return Err(());
        }
        let mut selector = Box::new(pseudo_class_selector(pseudo_class));
        match parameter_type {
            PseudoClassParameterType::AnPlusB | PseudoClassParameterType::AnPlusBOf => {
                let mut stream = values;
                selector.an_plus_b_pattern = parse_an_plus_b(&mut stream).ok_or(())?;
                stream.discard_whitespace();
                if stream.is_empty() {
                    return Ok(selector);
                }
                if parameter_type != PseudoClassParameterType::AnPlusBOf || !next_is_ident(&mut stream, "of") {
                    return Err(());
                }
                stream.discard_whitespace();
                selector.argument_selector_list =
                    self.parse_selector_list(stream, SelectorType::Standalone, SelectorParsingMode::Standard)?;
            }
            PseudoClassParameterType::CompoundSelector => {
                let mut stream = values;
                let mut compound = self.parse_compound_selector(&mut stream)?;
                stream.discard_whitespace();
                if !stream.is_empty() || compound.simple_selectors.is_empty() {
                    return Err(());
                }
                compound.combinator = Combinator::None;
                selector.argument_selector_list = Box::new([CompiledSelector::new(Box::new([compound]))]);
            }
            PseudoClassParameterType::ForgivingSelectorList => {
                selector.argument_selector_list =
                    self.parse_selector_list(values, SelectorType::Standalone, SelectorParsingMode::Forgiving)?;
                selector.is_forgiving = true;
            }
            PseudoClassParameterType::Ident
            | PseudoClassParameterType::LanguageRanges
            | PseudoClassParameterType::LevelList => {
                self.parse_pseudo_class_arguments(&mut selector, parameter_type, values)?;
            }
            PseudoClassParameterType::SelectorList | PseudoClassParameterType::RelativeSelectorList => {
                let selector_type = if parameter_type == PseudoClassParameterType::SelectorList {
                    SelectorType::Standalone
                } else {
                    SelectorType::Relative
                };
                selector.argument_selector_list =
                    self.parse_selector_list(values, selector_type, SelectorParsingMode::Standard)?;
                if parameter_type == PseudoClassParameterType::SelectorList
                    && selector.argument_selector_list.iter().any(contains_pseudo_element)
                {
                    return Err(());
                }
            }
            PseudoClassParameterType::None => return Err(()),
        }
        Ok(selector)
    }

    /// Parses the arguments of a pseudo-class that are not selectors.
    #[inline(never)]
    fn parse_pseudo_class_arguments(
        &self,
        selector: &mut PseudoClassSelector,
        parameter_type: PseudoClassParameterType,
        values: Values<'_>,
    ) -> Result<(), ()> {
        match parameter_type {
            PseudoClassParameterType::Ident => {
                let mut stream = values;
                stream.discard_whitespace();
                let ident = stream.next().and_then(|value| value.ident()).ok_or(())?;
                stream.discard_whitespace();
                if !stream.is_empty() {
                    return Err(());
                }
                selector.direction = Some(if ascii_eq(&ident, "ltr") {
                    Direction::LeftToRight
                } else if ascii_eq(&ident, "rtl") {
                    Direction::RightToLeft
                } else {
                    Direction::Other
                });
                selector.identifier_identity = self.name(&ident);
                selector.identifier = Some(ident.as_slice().into());
            }
            PseudoClassParameterType::LanguageRanges => {
                let mut languages = Vec::new();
                for values in split_on_commas(values) {
                    let mut stream = values;
                    stream.discard_whitespace();
                    let value = stream.next().ok_or(())?;
                    let (value, is_string) = if let Some(ident) = value.ident() {
                        (SelectorString::from_utf16(&ident), false)
                    } else if let Some(string) = value.string() {
                        (SelectorString::from_utf16(&string), true)
                    } else {
                        return Err(());
                    };
                    stream.discard_whitespace();
                    if !stream.is_empty() {
                        return Err(());
                    }
                    languages.push(LanguageRange { value, is_string });
                }
                selector.languages = languages.into_boxed_slice();
            }
            PseudoClassParameterType::LevelList => {
                let mut levels = Vec::new();
                for values in split_on_commas(values) {
                    let mut stream = values;
                    stream.discard_whitespace();
                    let Some((value, true, _)) = stream.next().and_then(|value| value.number()) else {
                        return Err(());
                    };
                    if !value.is_finite() || value.fract() != 0.0 || value < i64::MIN as f64 || value > i64::MAX as f64
                    {
                        return Err(());
                    }
                    stream.discard_whitespace();
                    if !stream.is_empty() {
                        return Err(());
                    }
                    levels.push(value as i64);
                }
                selector.levels = levels.into_boxed_slice();
            }
            _ => unreachable!("the pseudo-class takes selectors or nothing"),
        }
        Ok(())
    }

    #[inline(never)]
    fn parse_pseudo_element(&mut self, stream: &mut Values<'_>) -> Result<SimpleSelector, ()> {
        if !stream.next().is_some_and(|value| value.is_colon()) {
            return Err(());
        }
        let double_colon = stream.peek().is_some_and(|value| value.is_colon());
        if double_colon {
            stream.skip();
        }
        let name_value = stream.next().ok_or(())?;
        let (name, function_values) = if let Some(name) = name_value.ident() {
            (name, None)
        } else if let Some((name, values)) = name_value.function() {
            (name, Some(values))
        } else {
            return Err(());
        };

        let Some((pseudo_element, alias)) = PseudoElementType::from_name(&name) else {
            if function_values.is_none() && ascii_starts_with(&name, "-webkit-") && !self.inside_has() {
                return Ok(SimpleSelector::PseudoElement(Box::new(PseudoElementSelector {
                    pseudo_element: PseudoElementType::UnknownWebKit,
                    serialized_name: Some(lowercase(&name)),
                    value: PseudoElementValue::None,
                    identifier_identities: Box::new([]),
                })));
            }
            return Err(());
        };
        if self.inside_has() {
            return Err(());
        }
        if !double_colon {
            if function_values.is_some()
                || !matches!(
                    pseudo_element,
                    PseudoElementType::After
                        | PseudoElementType::Before
                        | PseudoElementType::FirstLetter
                        | PseudoElementType::FirstLine
                )
            {
                return Err(());
            }
            return Ok(SimpleSelector::PseudoElement(Box::new(PseudoElementSelector {
                pseudo_element,
                serialized_name: alias.map(|alias| alias.encode_utf16().collect::<Vec<_>>().into()),
                value: PseudoElementValue::None,
                identifier_identities: Box::new([]),
            })));
        }

        let metadata = pseudo_element.metadata();
        let value = match function_values {
            Some(_) if !metadata.is_valid_as_function => return Err(()),
            None if !metadata.is_valid_as_identifier => return Err(()),
            None => PseudoElementValue::None,
            Some(values) => self.parse_pseudo_element_function(metadata.parameter_type, values)?,
        };
        Ok(SimpleSelector::PseudoElement(Box::new(PseudoElementSelector {
            pseudo_element,
            serialized_name: alias.map(|alias| alias.encode_utf16().collect::<Vec<_>>().into()),
            identifier_identities: match &value {
                PseudoElementValue::Identifiers(identifiers) => identifiers
                    .iter()
                    .map(|identifier| self.name(identifier.units()))
                    .collect(),
                _ => Box::new([]),
            },
            value,
        })))
    }

    fn inside_has(&self) -> bool {
        self.pseudo_class_context.contains(&PseudoClassType::Has)
    }

    fn parse_pseudo_element_function(
        &mut self,
        parameter_type: PseudoElementParameterType,
        values: Values<'_>,
    ) -> Result<PseudoElementValue, ()> {
        let mut stream = values;
        stream.discard_whitespace();
        match parameter_type {
            PseudoElementParameterType::None => {
                if !stream.is_empty() {
                    return Err(());
                }
                Ok(PseudoElementValue::None)
            }
            PseudoElementParameterType::CompoundSelector => {
                let mut compound = self.parse_compound_selector(&mut stream)?;
                stream.discard_whitespace();
                if !stream.is_empty() || compound.simple_selectors.is_empty() {
                    return Err(());
                }
                compound.combinator = Combinator::None;
                Ok(PseudoElementValue::CompoundSelector(Arc::new(CompiledSelector::new(
                    Box::new([compound]),
                ))))
            }
            // https://drafts.csswg.org/css-pseudo-4/#typedef-highlight-name-selector
            // <highlight-name-selector> = '*' | <custom-ident>
            PseudoElementParameterType::HighlightNameSelector => {
                let argument = stream.next().ok_or(())?;
                let value = if argument.is_delim(b'*') {
                    PseudoElementValue::Universal
                } else {
                    let name = argument.ident().ok_or(())?;
                    if is_css_wide_keyword(&name) || ascii_eq(&name, "default") {
                        return Err(());
                    }
                    PseudoElementValue::Identifiers(Box::new([name.as_slice().into()]))
                };
                stream.discard_whitespace();
                if !stream.is_empty() {
                    return Err(());
                }
                Ok(value)
            }
            PseudoElementParameterType::IdentList => {
                let mut identifiers = Vec::new();
                while !stream.is_empty() {
                    identifiers.push(
                        stream
                            .next()
                            .and_then(|value| value.ident())
                            .ok_or(())?
                            .as_slice()
                            .into(),
                    );
                    stream.discard_whitespace();
                }
                if identifiers.is_empty() {
                    return Err(());
                }
                Ok(PseudoElementValue::Identifiers(identifiers.into_boxed_slice()))
            }
            // https://drafts.csswg.org/css-view-transitions-2/#typedef-pt-name-and-class-selector
            // <pt-name-and-class-selector> = <pt-name-selector> <pt-class-selector>? | <pt-class-selector>
            // <pt-class-selector> = ['.' <custom-ident>]+
            // When interpreting the above grammar, white space is forbidden:
            // * Between <pt-name-selector> and <pt-class-selector>
            // * Between any of the components of <pt-class-selector>.
            PseudoElementParameterType::PTNameAndClassSelector => {
                fn custom_ident(value: Option<Cv<'_>>) -> Result<SelectorString, ()> {
                    let ident = value.and_then(|value| value.ident()).ok_or(())?;
                    if is_css_wide_keyword(&ident) || ascii_eq(&ident, "default") {
                        return Err(());
                    }
                    Ok(ident.as_slice().into())
                }

                // https://drafts.csswg.org/css-view-transitions-1/#typedef-pt-name-selector
                // <pt-name-selector> = '*' | <custom-ident>
                let name = if stream.peek().is_some_and(|value| value.is_delim(b'*')) {
                    stream.skip();
                    Some(TransitionNameSelector::Universal)
                } else if stream.peek().is_some_and(|value| value.is_ident()) {
                    Some(TransitionNameSelector::Name(custom_ident(stream.next())?))
                } else {
                    None
                };

                let mut classes = Vec::new();
                while stream.peek().is_some_and(|value| value.is_delim(b'.')) {
                    stream.skip();
                    classes.push(custom_ident(stream.next())?);
                }
                if name.is_none() && classes.is_empty() {
                    return Err(());
                }

                stream.discard_whitespace();
                if !stream.is_empty() {
                    return Err(());
                }
                Ok(PseudoElementValue::TransitionName(Box::new(
                    TransitionNameAndClassSelector {
                        name,
                        classes: classes.into_boxed_slice(),
                    },
                )))
            }
        }
    }

    fn parse_compound_selector(&mut self, stream: &mut Values<'_>) -> Result<CompoundSelector, ()> {
        let outer_pseudo_element = self.compound_pseudo_element.take();
        let simple_selectors = self.parse_compound_simple_selectors(stream);
        self.compound_pseudo_element = outer_pseudo_element;
        Ok(CompoundSelector {
            combinator: Combinator::None,
            is_implicit_universal_anchor: false,
            simple_selectors: simple_selectors?,
        })
    }

    fn parse_compound_simple_selectors(
        &mut self,
        stream: &mut Values<'_>,
    ) -> Result<SmallVec<[SimpleSelector; 4]>, ()> {
        let mut simple_selectors = SmallVec::new();
        while let Some(simple_selector) = self.parse_simple_selector(stream)? {
            if matches!(
                simple_selector,
                SimpleSelector::TagName(_) | SimpleSelector::Universal(_)
            ) && !simple_selectors.is_empty()
            {
                return Err(());
            }
            if let SimpleSelector::PseudoElement(pseudo_element) = &simple_selector {
                self.compound_pseudo_element = Some(pseudo_element.pseudo_element);
            }
            simple_selectors.push(simple_selector);
        }
        Ok(simple_selectors)
    }

    fn parse_combinator(&self, stream: &mut Values<'_>) -> Option<Combinator> {
        let original = *stream;
        let had_whitespace = stream.peek().is_some_and(|value| value.is_whitespace());
        stream.discard_whitespace();
        let combinator = if stream.peek().is_some_and(|value| value.is_delim(b'>')) {
            stream.skip();
            Some(Combinator::ImmediateChild)
        } else if stream.peek().is_some_and(|value| value.is_delim(b'+')) {
            stream.skip();
            Some(Combinator::NextSibling)
        } else if stream.peek().is_some_and(|value| value.is_delim(b'~')) {
            stream.skip();
            Some(Combinator::SubsequentSibling)
        } else if stream.peek().is_some_and(|value| value.is_delim(b'|'))
            && stream.peek_n(1).is_some_and(|value| value.is_delim(b'|'))
        {
            stream.skip();
            stream.skip();
            Some(Combinator::Column)
        } else if had_whitespace {
            Some(Combinator::Descendant)
        } else {
            *stream = original;
            None
        };
        if combinator.is_some() {
            stream.discard_whitespace();
        }
        combinator
    }
}

fn contains_pseudo_element(selector: &CompiledSelector) -> bool {
    selector.compound_selectors().iter().any(|compound| {
        compound
            .simple_selectors
            .iter()
            .any(|simple| matches!(simple, SimpleSelector::PseudoElement(_)))
    })
}

fn normalize_pseudo_element_transitions<const COMPOUNDS: usize>(
    compounds: SmallVec<[CompoundSelector; COMPOUNDS]>,
) -> SmallVec<[CompoundSelector; COMPOUNDS]> {
    if !compounds.iter().any(|compound| {
        compound
            .simple_selectors
            .iter()
            .any(|simple| matches!(simple, SimpleSelector::PseudoElement(_)))
    }) {
        return compounds;
    }

    let mut normalized = SmallVec::new();
    for compound in compounds {
        let mut combinator = compound.combinator;
        let mut current = SimpleSelectorVec::new();
        for simple in compound.simple_selectors {
            if matches!(simple, SimpleSelector::PseudoElement(_)) {
                if current.is_empty() {
                    normalized.push(CompoundSelector {
                        combinator,
                        is_implicit_universal_anchor: true,
                        simple_selectors: smallvec::smallvec![SimpleSelector::Universal(Box::new(QualifiedName::new(
                            NamespaceType::Any,
                            StyleAtomID::NONE,
                            StyleAtomID::NONE,
                        )))],
                    });
                } else {
                    normalized.push(CompoundSelector {
                        combinator,
                        is_implicit_universal_anchor: false,
                        simple_selectors: std::mem::take(&mut current),
                    });
                }
                combinator = Combinator::PseudoElement;
            }
            current.push(simple);
        }
        if !current.is_empty() {
            normalized.push(CompoundSelector {
                combinator,
                is_implicit_universal_anchor: false,
                simple_selectors: current,
            });
        }
    }
    normalized
}

pub(crate) fn parse_selector_list<'a>(
    input: impl Into<TokenizerInput<'a>>,
    declared_namespaces: &[TokenizerInput<'_>],
    selector_type: SelectorType,
    parsing_mode: SelectorParsingMode,
) -> Result<ParsedSelectorTree, ()> {
    let filtered = filter_code_points(input.into());
    let input = filtered.as_input();
    let mut rows = Rows::default();
    rows.fill_from(input)?;
    parse_selector_list_with_mode(
        rows.values(input, false),
        None,
        declared_namespaces,
        selector_type,
        parsing_mode,
    )
}

fn parse_selector_list_with_mode(
    values: Values<'_>,
    name_cache: Option<&std::cell::RefCell<NameCache>>,
    declared_namespaces: &[TokenizerInput<'_>],
    selector_type: SelectorType,
    parsing_mode: SelectorParsingMode,
) -> Result<ParsedSelectorTree, ()> {
    let mut parser = SelectorParser::new(Some(declared_namespaces));
    parser.name_cache = name_cache;
    let selectors = parser.parse_selectors(values, selector_type, parsing_mode)?;
    Ok(ParsedSelectorTree {
        selectors,
        _names: parser.names.into_inner(),
    })
}

/// Parses the selector list that `values` are. The parser reads their source text.
pub(crate) fn parse_selector_list_from_component_values(
    values: &[ComponentValue],
    declared_namespaces: &[TokenizerInput<'_>],
    selector_type: SelectorType,
) -> Result<ParsedSelectorTree, ()> {
    let source = values
        .iter()
        .flat_map(|value| value.original_source_text.iter())
        .collect::<Vec<_>>();
    parse_selector_list(
        TokenizerInput::Utf16(&source),
        declared_namespaces,
        selector_type,
        SelectorParsingMode::Standard,
    )
}

/// Parses the selector list whose tokens are `rows`, which is the prelude of a qualified rule in `source`, which is
/// preprocessed text. `names` holds the names of the selectors until their arena leases them.
pub(crate) fn parse_selector_list_of_rows(
    rows: &Rows,
    names: &std::cell::RefCell<NameCache>,
    source: TokenizerInput<'_>,
    declared_namespaces: &[TokenizerInput<'_>],
    selector_type: SelectorType,
) -> Result<ParsedSelectorTree, ()> {
    // NB: The whitespace before a rule is not part of its prelude.
    parse_selector_list_with_mode(
        rows.values(source, true),
        Some(names),
        declared_namespaces,
        selector_type,
        SelectorParsingMode::Standard,
    )
}

/// The selectors of a list that the parser built, in place where they are few.
pub(crate) type ParsedSelectors = SmallVec<[CompiledSelector; 3]>;

/// A selector list as the parser built it, with the names it interned, before it is packed into
/// the arena a parsed list keeps.
pub(crate) struct ParsedSelectorTree {
    pub(crate) selectors: ParsedSelectors,
    /// A reference to each name the tree names, until the arena leases them.
    _names: Vec<AtomLease>,
}

impl ParsedSelectorTree {
    pub(crate) fn adapt_for_nesting(mut self, parent: StyleNestingParent) -> Self {
        if parent == StyleNestingParent::None {
            return self;
        }
        for selector in &mut self.selectors {
            let first = selector
                .compound_selectors()
                .first()
                .map_or(Combinator::None, |compound| compound.combinator);
            let insert_nesting = parent == StyleNestingParent::Style
                && (!matches!(first, Combinator::None | Combinator::Descendant)
                    || !super::selector_operations::contains_nesting(selector));
            if insert_nesting {
                *selector = super::selector_operations::relative_to(selector, SimpleSelector::Nesting);
            } else if first == Combinator::Descendant {
                let mut compounds = selector.to_compounds();
                compounds[0].combinator = Combinator::None;
                *selector = CompiledSelector::from_compounds::<4>(compounds.into_vec().into());
            }
        }
        self
    }

    pub(crate) fn contains_pseudo_element(&self) -> bool {
        self.selectors
            .iter()
            .any(|selector| super::selector_tree::target_pseudo_element(selector).is_some())
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.selectors.is_empty()
    }

    /// The list, packed into an arena of its own, for a rule nested in `nesting_parent`. A `&`
    /// naming a style rule is bound to it as the rule is placed.
    pub(crate) fn into_list(self, nesting_parent: StyleNestingParent) -> RustParsedSelectorList {
        self.into_list_declaring(nesting_parent, super::selector_arena::NamespaceDeclarations::default())
    }

    /// As [`Self::into_list`], for the selectors of a style rule of a sheet declaring `declarations`.
    pub(crate) fn into_list_declaring(
        self,
        nesting_parent: StyleNestingParent,
        declarations: super::selector_arena::NamespaceDeclarations,
    ) -> RustParsedSelectorList {
        let nesting = match nesting_parent {
            StyleNestingParent::None => NestingTarget::Root,
            StyleNestingParent::Scope => NestingTarget::Scope,
            StyleNestingParent::Style => NestingTarget::Unbound,
        };
        let mut builder = super::selector_arena::SheetSelectorsBuilder::default();
        builder.declare_namespaces(declarations);
        RustParsedSelectorList {
            selectors: builder.push(&self.selectors, self._names, nesting),
        }
    }

    /// The selectors of a style rule, packed into the arena `selectors` builds, which it reads once
    /// that is frozen, with the ordinal a `&` names the rule by there.
    pub(crate) fn into_rule_list_in(
        self,
        selectors: &mut super::selector_arena::SheetSelectorsBuilder,
        nesting: NestingTarget,
    ) -> (RustParsedSelectorList, Option<RuleOrdinal>) {
        let (selectors, ordinal) = selectors.push_rule(&self.selectors, self._names, nesting);
        (RustParsedSelectorList { selectors }, ordinal)
    }
}

fn parse_pseudo_element_selector(
    input: TokenizerInput<'_>,
    for_computed_style: bool,
) -> Result<(SelectorRef, PseudoElementType), ()> {
    let filtered = filter_code_points(input);
    let input = filtered.as_input();
    let mut rows = Rows::default();
    rows.fill_from(input)?;
    let mut stream = rows.values(input, false);
    // NB: The parser holds the names it interned until the selector built below leases them.
    let mut parser = SelectorParser::new(Some(&[]));
    let simple = parser.parse_pseudo_element(&mut stream)?;
    if !stream.is_empty() {
        return Err(());
    }
    let SimpleSelector::PseudoElement(pseudo_element) = &simple else {
        unreachable!();
    };
    // https://drafts.csswg.org/cssom/#dom-window-getcomputedstyle
    // Parse pseudoElt as a <pseudo-element-selector>, and let type be the result.
    // NB: Computed style lookup identifies one pseudo-element, so a view-transition argument must name it exactly.
    if for_computed_style
        && let PseudoElementValue::TransitionName(argument) = &pseudo_element.value
        && (!matches!(argument.name, Some(TransitionNameSelector::Name(_))) || !argument.classes.is_empty())
    {
        return Err(());
    }
    let pseudo_element_type = pseudo_element.pseudo_element;
    let mut compounds = SmallVec::new();
    compounds.push(CompoundSelector {
        combinator: Combinator::None,
        is_implicit_universal_anchor: false,
        simple_selectors: smallvec::smallvec![simple],
    });
    let selector = CompiledSelector::from_compounds::<4>(normalize_pseudo_element_transitions(compounds));
    let list = SelectorListRef::pack(&[selector]);
    let selector = list.owned().next().expect("the list holds the selector");
    Ok((selector, pseudo_element_type))
}

/// A parsed selector list, in the arena it is packed into.
#[derive(Clone, Debug, PartialEq)]
pub struct RustParsedSelectorList {
    pub(crate) selectors: SelectorListRef,
}

// Parsed selectors and their interned names can be shared across threads.
const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<RustParsedSelectorList>();
};

impl RustParsedSelectorList {
    /// Whether both are the same list of the same arena, not only equal ones.
    #[cfg(test)]
    pub(crate) fn is_same_list(&self, other: &Self) -> bool {
        self.selectors.is_same_list(&other.selectors)
    }

    #[cfg(test)]
    pub(crate) fn selectors(&self) -> &SelectorListRef {
        &self.selectors
    }
}

/// # Safety
/// `input` and each namespace view must point to readable storage for the duration of this call.
#[cfg(test)]
pub(crate) unsafe fn selector_parse(
    input: FfiUtf16View,
    namespaces: *const FfiStringView,
    namespace_count: usize,
    is_relative: bool,
    is_forgiving: bool,
    nesting_parent: StyleNestingParent,
) -> *mut RustParsedSelectorList {
    unsafe {
        parse_selector_list_from_ffi(
            input,
            namespaces,
            namespace_count,
            is_relative,
            is_forgiving,
            nesting_parent,
        )
    }
    .map_or(std::ptr::null_mut(), |selectors| Box::into_raw(Box::new(selectors)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_selector_parse_with_namespace_context(
    input: FfiUtf16View,
    namespaces: Option<&crate::css::rule::NativeNamespaceContext>,
    is_relative: bool,
    is_forgiving: bool,
) -> *mut RustParsedSelectorList {
    let Some(input) = input.units() else {
        return std::ptr::null_mut();
    };
    let declarations = namespaces.map(crate::css::rule::NativeNamespaceContext::declarations);
    let namespaces: Vec<_> = namespaces
        .into_iter()
        .flat_map(|namespaces| namespaces.prefixes.iter())
        .map(|namespace| TokenizerInput::Utf16(namespace.units()))
        .collect();
    parse_selector_list(
        input,
        &namespaces,
        if is_relative {
            SelectorType::Relative
        } else {
            SelectorType::Standalone
        },
        if is_forgiving {
            SelectorParsingMode::Forgiving
        } else {
            SelectorParsingMode::Standard
        },
    )
    .map_or(std::ptr::null_mut(), |selectors| {
        Box::into_raw(Box::new(
            selectors.into_list_declaring(StyleNestingParent::None, declarations.unwrap_or_default()),
        ))
    })
}

#[cfg(test)]
pub(crate) unsafe fn parse_selector_list_from_ffi(
    input: FfiUtf16View,
    namespaces: *const FfiStringView,
    namespace_count: usize,
    is_relative: bool,
    is_forgiving: bool,
    nesting_parent: StyleNestingParent,
) -> Option<RustParsedSelectorList> {
    unsafe {
        let input = input.units()?;
        let namespace_views = if namespace_count == 0 {
            &[]
        } else {
            assert!(!namespaces.is_null());
            ak::slice_from_raw_parts(namespaces, namespace_count)
        };
        let namespace_storage = namespace_views
            .iter()
            .map(|namespace| {
                if namespace.length == 0 {
                    TokenizerInput::Utf16(&[])
                } else {
                    assert!(!namespace.data.is_null());
                    TokenizerInput::Utf16(ak::slice_from_raw_parts(namespace.data, namespace.length))
                }
            })
            .collect::<Vec<_>>();
        let Ok(selectors) = parse_selector_list(
            input,
            &namespace_storage,
            if is_relative {
                SelectorType::Relative
            } else {
                SelectorType::Standalone
            },
            if is_forgiving {
                SelectorParsingMode::Forgiving
            } else {
                SelectorParsingMode::Standard
            },
        ) else {
            return None;
        };
        Some(selectors.adapt_for_nesting(nesting_parent).into_list(nesting_parent))
    }
}

#[repr(C)]
pub struct FfiParsedPseudoElement {
    pub selector: *mut RustSelector,
    pub pseudo_element: u8,
    pub highlight_name: usize,
}

/// Parses the restricted pseudo-element syntax accepted by `getComputedStyle()` and animation APIs.
#[unsafe(no_mangle)]
pub extern "C" fn rust_selector_parse_pseudo_element(
    input: FfiUtf16View,
    for_computed_style: bool,
) -> FfiParsedPseudoElement {
    let failure = FfiParsedPseudoElement {
        selector: std::ptr::null_mut(),
        pseudo_element: u8::MAX,
        highlight_name: 0,
    };
    let Some(input) = input.units() else {
        return failure;
    };
    let Ok((selector, pseudo_element)) = parse_pseudo_element_selector(input, for_computed_style) else {
        return failure;
    };
    let highlight_name = selector.view().target_highlight_name_text().map_or(0, |name| {
        std::mem::ManuallyDrop::new(RetainedUtf16FlyString::from_utf16(name)).raw()
    });
    FfiParsedPseudoElement {
        selector: Box::into_raw(Box::new(RustSelector { selector })),
        pseudo_element: pseudo_element as u8,
        highlight_name,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parsed_selector_list_destroy(list: OwnedBox<RustParsedSelectorList>) {
    drop(list);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_parsed_selector_list_length(list: &RustParsedSelectorList) -> usize {
    list.selectors.len()
}

/// The selector at `index`, which must be in bounds, as a new selector that C++ owns.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parsed_selector_list_selector(list: &RustParsedSelectorList, index: usize) -> *mut RustSelector {
    Box::into_raw(Box::new(RustSelector {
        selector: list.selectors.owned().nth(index).expect("the index is in bounds"),
    }))
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse(source: &str) -> Box<RustParsedSelectorList> {
        let units: Vec<_> = source.encode_utf16().collect();
        let prefix: Vec<_> = "é".encode_utf16().collect();
        let prefix = FfiStringView {
            data: prefix.as_ptr(),
            length: prefix.len(),
        };
        unsafe {
            let parsed = selector_parse(
                FfiUtf16View::from_utf16(&units),
                &prefix,
                1,
                false,
                false,
                StyleNestingParent::None,
            );
            assert!(!parsed.is_null());
            Box::from_raw(parsed)
        }
    }

    fn serialize(selector: &SelectorRef) -> Vec<u16> {
        crate::css::selector_serialization::serialize_selector_without_namespaces(&RustSelector {
            selector: selector.clone(),
        })
    }

    #[test]
    fn independent_clones_outlive_the_parsed_list_and_each_other() {
        for source in [
            "DIV#test.item[data-name=\"value\"]",
            ":is(.first, :not(#second))",
            ":nth-child(2n+1 of .item)",
            "::slotted(.item)",
            "::part(label)",
        ] {
            let parsed = std::thread::spawn(move || parse(source)).join().unwrap();
            let first = parsed.clone();
            let second = parsed.clone();
            let first_selector = first.selectors.owned().next().unwrap();
            let second_selector = second.selectors.owned().next().unwrap();
            assert_eq!(first_selector.view(), second_selector.view());
            drop(parsed);
            let expected = serialize(&first_selector);
            assert_eq!(serialize(&second_selector), expected);
            assert_eq!(
                first_selector.view().specificity(),
                second_selector.view().specificity()
            );
            drop((first, first_selector));
            assert_eq!(serialize(&second_selector), expected);
        }
    }

    #[test]
    fn worker_parsing_reading_and_release_only_use_thread_safe_callbacks() {
        use crate::css::ffi_stats::THREAD_UNSAFE_CPP_CALLBACK_COUNT;
        let parsed = std::thread::spawn(|| {
            let parsed = parse("é|DIV#😀:is(.é, :not([data-name='😀'])):nth-child(2n+1 of .item)::part(label)");
            assert_eq!(THREAD_UNSAFE_CPP_CALLBACK_COUNT.get(), 0);
            parsed
        })
        .join()
        .unwrap();
        let bound = std::thread::scope(|scope| {
            let parsed = &parsed;
            scope.spawn(move || {
                for _ in 0..100 {
                    assert_eq!(parsed.selectors.len(), 1);
                }
                assert_eq!(THREAD_UNSAFE_CPP_CALLBACK_COUNT.get(), 0);
            });
            parsed.clone()
        });
        std::thread::spawn(move || {
            drop(parsed);
            assert_eq!(THREAD_UNSAFE_CPP_CALLBACK_COUNT.get(), 0);
        })
        .join()
        .unwrap();
        assert_eq!(
            serialize(&bound.selectors.owned().next().unwrap()),
            "é|DIV#😀:is(.é, :not([data-name=\"😀\"])):nth-child(2n+1 of .item)::part(label)"
                .encode_utf16()
                .collect::<Vec<_>>()
        );
    }
}
