/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS selector parsing.

use std::sync::Arc;

use super::css_tokenizer::{CssNumberType, ParserTokenKind, TokenizerInput, tokenize_for_parser};
use super::ffi_support::FfiUtf16View;
use super::ffi_support::ascii_lowercase;
use super::parser::component_value::{ComponentKind, ComponentValue, consume_a_list_of_component_values};
use super::parser::token_stream::TokenStream as Stream;
use super::retained_fly_string::RetainedUtf16FlyString;
use super::selector::{
    AnPlusBPattern, AttributeCaseType, AttributeMatchType, Combinator, Direction, LanguageRange, NamespaceType,
    PseudoClassParameterType, PseudoClassType, PseudoElementParameterType, PseudoElementType, SelectorString,
};
use super::selector::{
    AttributeSelector, CompiledSelector, CompoundSelector, NameSelector, PseudoClassSelector, PseudoElementSelector,
    PseudoElementValue, QualifiedName, SelectorList, SimpleSelector,
};
use super::selector::{FfiStringView, RustSelector};

const MAXIMUM_SELECTOR_NESTING_DEPTH: usize = 128;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelectorType {
    Standalone,
    Relative,
}

pub use super::ffi_support::StyleNestingParent;

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

fn lowercase_shared(value: &SelectorString) -> SelectorString {
    if value.iter().all(|&unit| ascii_lowercase(unit) == unit) {
        value.clone()
    } else {
        lowercase(value)
    }
}

fn selector_name(value: &[u16]) -> NameSelector {
    let name = SelectorString::from(value);
    let lowercase_name = lowercase_shared(&name);
    NameSelector {
        interned_name: name.to_fly_string(),
        interned_lowercase_name: lowercase_name.to_fly_string(),
        name,
    }
}

fn pseudo_class_selector(pseudo_class: PseudoClassType) -> PseudoClassSelector {
    PseudoClassSelector {
        pseudo_class,
        an_plus_b_pattern: AnPlusBPattern::default(),
        argument_selector_list: Box::new([]),
        languages: Box::new([]),
        direction: None,
        identifier: None,
        identifier_identity: RetainedUtf16FlyString::none(),
        identifier_lowercase_identity: RetainedUtf16FlyString::none(),
        levels: Box::new([]),
        is_forgiving: false,
    }
}

fn split_on_commas(values: &[ComponentValue]) -> Vec<&[ComponentValue]> {
    let mut result = Vec::new();
    let mut start = 0;
    loop {
        let end = values[start..]
            .iter()
            .position(ComponentValue::is_comma)
            .map_or(values.len(), |offset| start + offset);
        result.push(&values[start..end]);
        if end == values.len() {
            break;
        }
        start = end + 1;
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

fn integer(value: &ComponentValue) -> Option<(i32, bool)> {
    let ComponentKind::Token(ParserTokenKind::Number { value, number_type }) = value.kind else {
        return None;
    };
    if !matches!(
        number_type,
        CssNumberType::Integer | CssNumberType::IntegerWithExplicitSign
    ) {
        return None;
    }
    Some((
        clamp_integer(value),
        number_type == CssNumberType::IntegerWithExplicitSign,
    ))
}

fn dimension(value: &ComponentValue) -> Option<(i32, &[u16])> {
    let ComponentKind::Token(ParserTokenKind::Dimension {
        value,
        number_type,
        unit,
    }) = &value.kind
    else {
        return None;
    };
    if !matches!(
        number_type,
        CssNumberType::Integer | CssNumberType::IntegerWithExplicitSign
    ) {
        return None;
    }
    Some((clamp_integer(*value), unit))
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

fn parse_an_plus_b_tail(stream: &mut Stream<'_>, step_size: i32) -> AnPlusBPattern {
    let after_step = stream.position;
    stream.discard_whitespace();
    if let Some((offset, true)) = stream.peek().and_then(integer) {
        stream.position += 1;
        return AnPlusBPattern { step_size, offset };
    }
    let sign = if stream.peek().is_some_and(|value| value.is_delim(b'+')) {
        1
    } else if stream.peek().is_some_and(|value| value.is_delim(b'-')) {
        -1
    } else {
        stream.position = after_step;
        return AnPlusBPattern { step_size, offset: 0 };
    };
    stream.position += 1;
    stream.discard_whitespace();
    if let Some((offset, false)) = stream.peek().and_then(integer) {
        stream.position += 1;
        return AnPlusBPattern {
            step_size,
            offset: offset.saturating_mul(sign),
        };
    }
    stream.position = after_step;
    AnPlusBPattern { step_size, offset: 0 }
}

// https://drafts.csswg.org/css-syntax-3/#anb-microsyntax
fn parse_an_plus_b(stream: &mut Stream<'_>) -> Option<AnPlusBPattern> {
    let original = stream.position;
    stream.discard_whitespace();
    if stream
        .peek()
        .and_then(ComponentValue::ident)
        .is_some_and(|ident| ascii_eq(ident, "odd"))
    {
        stream.position += 1;
        return Some(AnPlusBPattern {
            step_size: 2,
            offset: 1,
        });
    }
    if stream
        .peek()
        .and_then(ComponentValue::ident)
        .is_some_and(|ident| ascii_eq(ident, "even"))
    {
        stream.position += 1;
        return Some(AnPlusBPattern {
            step_size: 2,
            offset: 0,
        });
    }
    if let Some((offset, _)) = stream.peek().and_then(integer) {
        stream.position += 1;
        return Some(AnPlusBPattern { step_size: 0, offset });
    }
    if let Some((step_size, unit)) = stream.peek().and_then(dimension) {
        if ascii_eq(unit, "n") {
            stream.position += 1;
            return Some(parse_an_plus_b_tail(stream, step_size));
        }
        if ascii_eq(unit, "n-") {
            stream.position += 1;
            if let Some((offset, false)) = stream.peek().and_then(integer) {
                stream.position += 1;
                return Some(AnPlusBPattern {
                    step_size,
                    offset: offset.saturating_neg(),
                });
            }
            stream.position = original;
            return None;
        }
        if unit.len() > 2 && ascii_starts_with(unit, "n-") && digits(&unit[2..]) {
            let offset = ascii_i32_saturating(&unit[1..])?;
            stream.position += 1;
            return Some(AnPlusBPattern { step_size, offset });
        }
    }
    if let Some(ident) = stream.peek().and_then(ComponentValue::ident) {
        if ident.len() > 3 && ascii_starts_with(ident, "-n-") && digits(&ident[3..]) {
            let offset = ascii_i32_saturating(&ident[2..])?;
            stream.position += 1;
            return Some(AnPlusBPattern { step_size: -1, offset });
        }
        if ascii_eq(ident, "-n") {
            stream.position += 1;
            return Some(parse_an_plus_b_tail(stream, -1));
        }
        if ascii_eq(ident, "-n-") {
            stream.position += 1;
            stream.discard_whitespace();
            if let Some((offset, false)) = stream.peek().and_then(integer) {
                stream.position += 1;
                return Some(AnPlusBPattern {
                    step_size: -1,
                    offset: offset.saturating_neg(),
                });
            }
            stream.position = original;
            return None;
        }
    }

    if stream.peek().is_some_and(|value| value.is_delim(b'+')) {
        stream.position += 1;
    }
    let ident = stream.next().and_then(ComponentValue::ident);
    if ident.is_some_and(|ident| ascii_eq(ident, "n")) {
        return Some(parse_an_plus_b_tail(stream, 1));
    }
    if ident.is_some_and(|ident| ascii_eq(ident, "n-")) {
        stream.discard_whitespace();
        if let Some((offset, false)) = stream.peek().and_then(integer) {
            stream.position += 1;
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
    stream.position = original;
    None
}

struct SelectorParser<'a> {
    declared_namespaces: Option<&'a [TokenizerInput<'a>]>,
    pseudo_class_context: Vec<PseudoClassType>,
    nesting_limit_exceeded: bool,
}

impl<'a> SelectorParser<'a> {
    fn new(declared_namespaces: Option<&'a [TokenizerInput<'a>]>) -> Self {
        Self {
            declared_namespaces,
            pseudo_class_context: Vec::new(),
            nesting_limit_exceeded: false,
        }
    }

    fn parse_selector_list(
        &mut self,
        values: &[ComponentValue],
        selector_type: SelectorType,
        parsing_mode: SelectorParsingMode,
    ) -> Result<SelectorList, ()> {
        if self.pseudo_class_context.len() > MAXIMUM_SELECTOR_NESTING_DEPTH {
            self.nesting_limit_exceeded = true;
            return Err(());
        }
        let mut selectors = Vec::new();
        let mut start = 0;
        loop {
            let end = values[start..]
                .iter()
                .position(ComponentValue::is_comma)
                .map_or(values.len(), |offset| start + offset);
            let selector_values = &values[start..end];
            let selector = self.parse_complex_selector(selector_values, selector_type);
            if self.nesting_limit_exceeded {
                return Err(());
            }
            match selector {
                Ok(selector) => selectors.push(selector),
                Err(()) if parsing_mode == SelectorParsingMode::Forgiving => {
                    let mut invalid_values = selector_values;
                    while invalid_values.first().is_some_and(ComponentValue::is_whitespace) {
                        invalid_values = &invalid_values[1..];
                    }
                    while invalid_values.last().is_some_and(ComponentValue::is_whitespace) {
                        invalid_values = &invalid_values[..invalid_values.len() - 1];
                    }
                    let source = invalid_values
                        .iter()
                        .flat_map(|value| value.original_source_text.iter())
                        .collect::<Vec<_>>();
                    selectors.push(CompiledSelector::new(
                        vec![CompoundSelector {
                            combinator: if selector_type == SelectorType::Standalone {
                                Combinator::None
                            } else {
                                Combinator::Descendant
                            },
                            is_implicit_universal_anchor: false,
                            simple_selectors: vec![SimpleSelector::Invalid(source.into())].into_boxed_slice(),
                        }]
                        .into_boxed_slice(),
                    ));
                }
                Err(()) => return Err(()),
            }
            if end == values.len() {
                break;
            }
            start = end + 1;
        }
        if selectors.is_empty() && parsing_mode != SelectorParsingMode::Forgiving {
            return Err(());
        }
        Ok(selectors.into_boxed_slice())
    }

    fn parse_complex_selector(
        &mut self,
        values: &[ComponentValue],
        selector_type: SelectorType,
    ) -> Result<Arc<CompiledSelector>, ()> {
        let mut stream = Stream::new(values);
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
        let mut compounds = vec![first];

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

        let compounds = normalize_pseudo_element_transitions(compounds);
        let mut pseudo_elements = Vec::new();
        let mut saw_pseudo_element_transition = false;
        for compound in &compounds {
            if saw_pseudo_element_transition && compound.combinator != Combinator::PseudoElement {
                return Err(());
            }
            saw_pseudo_element_transition |= compound.combinator == Combinator::PseudoElement;
            if let Some(SimpleSelector::PseudoElement(pseudo_element)) = compound.simple_selectors.first() {
                pseudo_elements.push(pseudo_element.pseudo_element);
            }
        }
        if pseudo_elements.len() > 1
            && !(pseudo_elements.len() == 2
                && pseudo_elements[0] == PseudoElementType::Part
                && pseudo_elements[1] != PseudoElementType::Part)
        {
            return Err(());
        }
        Ok(CompiledSelector::new(compounds.into_boxed_slice()))
    }

    fn parse_qualified_name(&self, stream: &mut Stream<'_>, allow_wildcard: bool) -> Option<QualifiedName> {
        fn name(value: &ComponentValue) -> Option<SelectorString> {
            if value.is_delim(b'*') {
                return Some(vec![b'*' as u16].into());
            }
            value.ident().map(Into::into)
        }

        let original = stream.position;
        let first = stream.next()?;
        if first.is_delim(b'|') {
            let parsed_name = stream.next().and_then(name)?;
            if !allow_wildcard && parsed_name.as_ref() == [b'*' as u16] {
                stream.position = original;
                return None;
            }
            let lowercase_name = lowercase_shared(&parsed_name);
            return Some(QualifiedName {
                namespace_type: NamespaceType::None,
                namespace: SelectorString::default(),
                interned_name: parsed_name.to_fly_string(),
                interned_lowercase_name: lowercase_name.to_fly_string(),
                interned_namespace: RetainedUtf16FlyString::none(),
                lowercase_name,
                name: parsed_name,
            });
        }

        let Some(first_name) = name(first) else {
            stream.position = original;
            return None;
        };
        if stream.peek().is_some_and(|value| value.is_delim(b'|')) && stream.peek_n(1).and_then(name).is_some() {
            stream.position += 1;
            let parsed_name = name(stream.next().unwrap()).unwrap();
            if !allow_wildcard && parsed_name.as_ref() == [b'*' as u16] {
                stream.position = original;
                return None;
            }
            let namespace_type = if first_name.as_ref() == [b'*' as u16] {
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
                stream.position = original;
                return None;
            }
            let lowercase_name = lowercase_shared(&parsed_name);
            return Some(QualifiedName {
                namespace_type,
                interned_name: parsed_name.to_fly_string(),
                interned_lowercase_name: lowercase_name.to_fly_string(),
                interned_namespace: if namespace_type == NamespaceType::Named {
                    first_name.to_fly_string()
                } else {
                    RetainedUtf16FlyString::none()
                },
                namespace: first_name,
                lowercase_name,
                name: parsed_name,
            });
        }
        if !allow_wildcard && first_name.as_ref() == [b'*' as u16] {
            stream.position = original;
            return None;
        }
        let lowercase_name = lowercase_shared(&first_name);
        Some(QualifiedName {
            namespace_type: NamespaceType::Default,
            namespace: SelectorString::default(),
            interned_name: first_name.to_fly_string(),
            interned_lowercase_name: lowercase_name.to_fly_string(),
            interned_namespace: RetainedUtf16FlyString::none(),
            lowercase_name,
            name: first_name,
        })
    }

    fn next_is_pseudo_element(&self, stream: &Stream<'_>) -> bool {
        if !stream.peek().is_some_and(ComponentValue::is_colon) {
            return false;
        }
        if stream.peek_n(1).is_some_and(ComponentValue::is_colon) {
            return true;
        }
        let Some(name) = stream.peek_n(1).and_then(ComponentValue::ident) else {
            return false;
        };
        PseudoElementType::from_name(name).is_some_and(|(pseudo_element, _)| {
            matches!(
                pseudo_element,
                PseudoElementType::After
                    | PseudoElementType::Before
                    | PseudoElementType::FirstLetter
                    | PseudoElementType::FirstLine
            )
        })
    }

    fn parse_simple_selector(&mut self, stream: &mut Stream<'_>) -> Result<Option<SimpleSelector>, ()> {
        if stream
            .peek()
            .is_none_or(|value| value.is_whitespace() || value.is_comma())
        {
            return Ok(None);
        }

        let original = stream.position;
        if let Some(qualified_name) = self.parse_qualified_name(stream, true) {
            return Ok(Some(if qualified_name.name.as_ref() == [b'*' as u16] {
                SimpleSelector::Universal(Box::new(qualified_name))
            } else {
                SimpleSelector::TagName(Box::new(qualified_name))
            }));
        }
        stream.position = original;

        if self.next_is_pseudo_element(stream) {
            return self.parse_pseudo_element(stream).map(Some);
        }
        if stream.peek().is_some_and(ComponentValue::is_colon) {
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
            let class_name = stream.next().and_then(ComponentValue::ident).ok_or(())?;
            return Ok(Some(SimpleSelector::Class(selector_name(class_name))));
        }
        if let ComponentKind::Token(ParserTokenKind::Hash { value, is_id: true }) = &first.kind {
            return Ok(Some(SimpleSelector::Id(selector_name(value))));
        }
        if let Some(values) = first.square_block() {
            return self.parse_attribute(values).map(Some);
        }
        Err(())
    }

    fn parse_attribute(&self, values: &[ComponentValue]) -> Result<SimpleSelector, ()> {
        let mut stream = Stream::new(values);
        stream.discard_whitespace();
        let qualified_name = self.parse_qualified_name(&mut stream, false).ok_or(())?;
        stream.discard_whitespace();
        if stream.is_empty() {
            return Ok(SimpleSelector::Attribute(Box::new(AttributeSelector {
                match_type: AttributeMatchType::HasAttribute,
                qualified_name,
                value: SelectorString::default(),
                value_identity: RetainedUtf16FlyString::from_utf16(&[]),
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
            .into();
        stream.discard_whitespace();
        let case_type = match stream.next() {
            None => AttributeCaseType::Default,
            Some(value) if value.ident().is_some_and(|ident| ascii_eq(ident, "i")) => AttributeCaseType::Insensitive,
            Some(value) if value.ident().is_some_and(|ident| ascii_eq(ident, "s")) => AttributeCaseType::Sensitive,
            Some(_) => return Err(()),
        };
        stream.discard_whitespace();
        if !stream.is_empty() {
            return Err(());
        }
        Ok(SimpleSelector::Attribute(Box::new(AttributeSelector {
            match_type,
            qualified_name,
            value_identity: value.to_fly_string(),
            value,
            case_type,
        })))
    }

    fn parse_pseudo_class(&mut self, stream: &mut Stream<'_>) -> Result<SimpleSelector, ()> {
        if !stream.next().is_some_and(ComponentValue::is_colon) {
            return Err(());
        }
        let value = stream.next().ok_or(())?;
        if let Some(name) = value.ident() {
            let pseudo_class = PseudoClassType::from_name(name).ok_or(())?;
            if !pseudo_class.metadata().is_valid_as_identifier {
                return Err(());
            }
            return Ok(SimpleSelector::PseudoClass(Box::new(pseudo_class_selector(
                pseudo_class,
            ))));
        }

        let (name, function_values) = value.function().ok_or(())?;
        let pseudo_class = PseudoClassType::from_name(name).ok_or(())?;
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

        self.pseudo_class_context.push(pseudo_class);
        let result = self.parse_pseudo_class_function(pseudo_class, metadata.parameter_type, function_values);
        self.pseudo_class_context.pop();
        result.map(|selector| SimpleSelector::PseudoClass(Box::new(selector)))
    }

    fn parse_pseudo_class_function(
        &mut self,
        pseudo_class: PseudoClassType,
        parameter_type: PseudoClassParameterType,
        values: &[ComponentValue],
    ) -> Result<PseudoClassSelector, ()> {
        if self.pseudo_class_context.len() > MAXIMUM_SELECTOR_NESTING_DEPTH {
            self.nesting_limit_exceeded = true;
            return Err(());
        }
        let mut selector = pseudo_class_selector(pseudo_class);
        match parameter_type {
            PseudoClassParameterType::AnPlusB | PseudoClassParameterType::AnPlusBOf => {
                let mut stream = Stream::new(values);
                selector.an_plus_b_pattern = parse_an_plus_b(&mut stream).ok_or(())?;
                stream.discard_whitespace();
                if stream.is_empty() {
                    return Ok(selector);
                }
                if parameter_type != PseudoClassParameterType::AnPlusBOf
                    || !stream
                        .next()
                        .and_then(ComponentValue::ident)
                        .is_some_and(|ident| ascii_eq(ident, "of"))
                {
                    return Err(());
                }
                stream.discard_whitespace();
                selector.argument_selector_list = self.parse_selector_list(
                    &stream.values[stream.position..],
                    SelectorType::Standalone,
                    SelectorParsingMode::Standard,
                )?;
            }
            PseudoClassParameterType::CompoundSelector => {
                let mut stream = Stream::new(values);
                let mut compound = self.parse_compound_selector(&mut stream)?;
                stream.discard_whitespace();
                if !stream.is_empty() || compound.simple_selectors.is_empty() {
                    return Err(());
                }
                compound.combinator = Combinator::None;
                selector.argument_selector_list = Box::new([CompiledSelector::new(Box::new([compound]))]);
            }
            PseudoClassParameterType::ForgivingSelectorList
            | PseudoClassParameterType::ForgivingRelativeSelectorList => {
                let selector_type = if parameter_type == PseudoClassParameterType::ForgivingSelectorList {
                    SelectorType::Standalone
                } else {
                    SelectorType::Relative
                };
                selector.argument_selector_list =
                    self.parse_selector_list(values, selector_type, SelectorParsingMode::Forgiving)?;
                selector.is_forgiving = true;
            }
            PseudoClassParameterType::Ident => {
                let mut stream = Stream::new(values);
                stream.discard_whitespace();
                let ident = stream.next().and_then(ComponentValue::ident).ok_or(())?;
                stream.discard_whitespace();
                if !stream.is_empty() {
                    return Err(());
                }
                selector.direction = Some(if ascii_eq(ident, "ltr") {
                    Direction::LeftToRight
                } else if ascii_eq(ident, "rtl") {
                    Direction::RightToLeft
                } else {
                    Direction::Other
                });
                selector.identifier_identity = RetainedUtf16FlyString::from_utf16(ident);
                selector.identifier_lowercase_identity = lowercase(ident).to_fly_string();
                selector.identifier = Some(ident.into());
            }
            PseudoClassParameterType::LanguageRanges => {
                let mut languages = Vec::new();
                for values in split_on_commas(values) {
                    let mut stream = Stream::new(values);
                    stream.discard_whitespace();
                    let (value, is_string) = match &stream.next().ok_or(())?.kind {
                        ComponentKind::Token(ParserTokenKind::Ident(value)) => {
                            (SelectorString::from_utf16(value), false)
                        }
                        ComponentKind::Token(ParserTokenKind::String(value)) => {
                            (SelectorString::from_utf16(value), true)
                        }
                        _ => return Err(()),
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
                    let mut stream = Stream::new(values);
                    stream.discard_whitespace();
                    let Some(ComponentValue {
                        kind:
                            ComponentKind::Token(ParserTokenKind::Number {
                                value,
                                number_type: CssNumberType::Integer | CssNumberType::IntegerWithExplicitSign,
                            }),
                        ..
                    }) = stream.next()
                    else {
                        return Err(());
                    };
                    if !value.is_finite()
                        || value.fract() != 0.0
                        || *value < i64::MIN as f64
                        || *value > i64::MAX as f64
                    {
                        return Err(());
                    }
                    stream.discard_whitespace();
                    if !stream.is_empty() {
                        return Err(());
                    }
                    levels.push(*value as i64);
                }
                selector.levels = levels.into_boxed_slice();
            }
            PseudoClassParameterType::SelectorList | PseudoClassParameterType::RelativeSelectorList => {
                let selector_type = if parameter_type == PseudoClassParameterType::SelectorList {
                    SelectorType::Standalone
                } else {
                    SelectorType::Relative
                };
                selector.argument_selector_list =
                    self.parse_selector_list(values, selector_type, SelectorParsingMode::Standard)?;
            }
            PseudoClassParameterType::None => return Err(()),
        }
        Ok(selector)
    }

    fn parse_pseudo_element(&mut self, stream: &mut Stream<'_>) -> Result<SimpleSelector, ()> {
        if !stream.next().is_some_and(ComponentValue::is_colon) {
            return Err(());
        }
        let double_colon = stream.peek().is_some_and(ComponentValue::is_colon);
        if double_colon {
            stream.position += 1;
        }
        let name_value = stream.next().ok_or(())?;
        let (name, function_values) = if let Some(name) = name_value.ident() {
            (name, None)
        } else if let Some((name, values)) = name_value.function() {
            (name, Some(values))
        } else {
            return Err(());
        };

        let Some((pseudo_element, alias)) = PseudoElementType::from_name(name) else {
            if function_values.is_none() && ascii_starts_with(name, "-webkit-") && !self.inside_has() {
                return Ok(SimpleSelector::PseudoElement(Box::new(PseudoElementSelector {
                    pseudo_element: PseudoElementType::UnknownWebKit,
                    serialized_name: Some(lowercase(name)),
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
                PseudoElementValue::Identifiers(identifiers) => {
                    identifiers.iter().map(SelectorString::to_fly_string).collect()
                }
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
        values: &[ComponentValue],
    ) -> Result<PseudoElementValue, ()> {
        let mut stream = Stream::new(values);
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
                Ok(PseudoElementValue::CompoundSelector(CompiledSelector::new(Box::new([
                    compound,
                ]))))
            }
            PseudoElementParameterType::IdentList => {
                let mut identifiers = Vec::new();
                while !stream.is_empty() {
                    identifiers.push(stream.next().and_then(ComponentValue::ident).ok_or(())?.into());
                    stream.discard_whitespace();
                }
                if identifiers.is_empty() {
                    return Err(());
                }
                Ok(PseudoElementValue::Identifiers(identifiers.into_boxed_slice()))
            }
            PseudoElementParameterType::PTNameSelector => {
                let (is_universal, value) = if stream.peek().is_some_and(|value| value.is_delim(b'*')) {
                    stream.position += 1;
                    (true, SelectorString::default())
                } else {
                    let ident = stream.next().and_then(ComponentValue::ident).ok_or(())?;
                    if is_css_wide_keyword(ident) || ascii_eq(ident, "default") {
                        return Err(());
                    }
                    (false, ident.into())
                };
                stream.discard_whitespace();
                if !stream.is_empty() {
                    return Err(());
                }
                Ok(PseudoElementValue::TransitionName { is_universal, value })
            }
        }
    }

    fn parse_compound_selector(&mut self, stream: &mut Stream<'_>) -> Result<CompoundSelector, ()> {
        let mut simple_selectors = Vec::new();
        while let Some(simple_selector) = self.parse_simple_selector(stream)? {
            if matches!(simple_selector, SimpleSelector::TagName(_)) && !simple_selectors.is_empty() {
                return Err(());
            }
            simple_selectors.push(simple_selector);
        }
        Ok(CompoundSelector {
            combinator: Combinator::None,
            is_implicit_universal_anchor: false,
            simple_selectors: simple_selectors.into_boxed_slice(),
        })
    }

    fn parse_combinator(&self, stream: &mut Stream<'_>) -> Option<Combinator> {
        let original = stream.position;
        let had_whitespace = stream.peek().is_some_and(ComponentValue::is_whitespace);
        stream.discard_whitespace();
        let combinator = if stream.peek().is_some_and(|value| value.is_delim(b'>')) {
            stream.position += 1;
            Some(Combinator::ImmediateChild)
        } else if stream.peek().is_some_and(|value| value.is_delim(b'+')) {
            stream.position += 1;
            Some(Combinator::NextSibling)
        } else if stream.peek().is_some_and(|value| value.is_delim(b'~')) {
            stream.position += 1;
            Some(Combinator::SubsequentSibling)
        } else if stream.peek().is_some_and(|value| value.is_delim(b'|'))
            && stream.peek_n(1).is_some_and(|value| value.is_delim(b'|'))
        {
            stream.position += 2;
            Some(Combinator::Column)
        } else if had_whitespace {
            Some(Combinator::Descendant)
        } else {
            stream.position = original;
            None
        };
        if combinator.is_some() {
            stream.discard_whitespace();
        }
        combinator
    }
}

fn normalize_pseudo_element_transitions(compounds: Vec<CompoundSelector>) -> Vec<CompoundSelector> {
    if !compounds.iter().any(|compound| {
        compound
            .simple_selectors
            .iter()
            .any(|simple| matches!(simple, SimpleSelector::PseudoElement(_)))
    }) {
        return compounds;
    }

    let mut normalized = Vec::new();
    for compound in compounds {
        let mut combinator = compound.combinator;
        let mut current = Vec::new();
        for simple in compound.simple_selectors {
            if matches!(simple, SimpleSelector::PseudoElement(_)) {
                if current.is_empty() {
                    normalized.push(CompoundSelector {
                        combinator,
                        is_implicit_universal_anchor: true,
                        simple_selectors: vec![SimpleSelector::Universal(Box::new(QualifiedName {
                            namespace_type: NamespaceType::Any,
                            namespace: SelectorString::default(),
                            name: vec![b'*' as u16].into(),
                            lowercase_name: vec![b'*' as u16].into(),
                            interned_name: RetainedUtf16FlyString::from_utf16(&[u16::from(b'*')]),
                            interned_lowercase_name: RetainedUtf16FlyString::from_utf16(&[u16::from(b'*')]),
                            interned_namespace: RetainedUtf16FlyString::none(),
                        }))]
                        .into_boxed_slice(),
                    });
                } else {
                    normalized.push(CompoundSelector {
                        combinator,
                        is_implicit_universal_anchor: false,
                        simple_selectors: std::mem::take(&mut current).into_boxed_slice(),
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
                simple_selectors: current.into_boxed_slice(),
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
) -> Result<SelectorList, ()> {
    let tokens = tokenize_for_parser(input);
    let values = consume_a_list_of_component_values(tokens.as_slice())?;
    SelectorParser::new(Some(declared_namespaces)).parse_selector_list(&values, selector_type, parsing_mode)
}

pub(crate) fn parse_selector_list_from_component_values(
    values: &[ComponentValue],
    declared_namespaces: &[TokenizerInput<'_>],
    selector_type: SelectorType,
) -> Result<RustParsedSelectorList, ()> {
    let selectors = SelectorParser::new(Some(declared_namespaces)).parse_selector_list(
        values,
        selector_type,
        SelectorParsingMode::Standard,
    )?;
    Ok(RustParsedSelectorList::new(selectors))
}

fn parse_pseudo_element_selector(input: TokenizerInput<'_>) -> Result<(Arc<CompiledSelector>, PseudoElementType), ()> {
    let tokens = tokenize_for_parser(input);
    let values = consume_a_list_of_component_values(tokens.as_slice())?;
    let mut stream = Stream::new(&values);
    let simple = SelectorParser::new(Some(&[])).parse_pseudo_element(&mut stream)?;
    if !stream.is_empty() {
        return Err(());
    }
    let SimpleSelector::PseudoElement(pseudo_element) = &simple else {
        unreachable!();
    };
    let pseudo_element_type = pseudo_element.pseudo_element;
    Ok((
        CompiledSelector::new(
            normalize_pseudo_element_transitions(vec![CompoundSelector {
                combinator: Combinator::None,
                is_implicit_universal_anchor: false,
                simple_selectors: Box::new([simple]),
            }])
            .into_boxed_slice(),
        ),
        pseudo_element_type,
    ))
}

#[derive(Clone, Debug, PartialEq)]
pub struct RustParsedSelectorList {
    pub(crate) selectors: SelectorList,
}

// Parsed selectors and their interned names can be shared across threads.
const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<RustParsedSelectorList>();
};

impl RustParsedSelectorList {
    pub(crate) fn adapt_for_nesting(mut self, parent: StyleNestingParent) -> Self {
        if parent == StyleNestingParent::None {
            return self;
        }
        for selector in &mut self.selectors {
            let first = selector
                .compound_selectors
                .first()
                .map_or(Combinator::None, |compound| compound.combinator);
            let insert_nesting = parent == StyleNestingParent::Style
                && (!matches!(first, Combinator::None | Combinator::Descendant)
                    || !super::selector_operations::contains_nesting(selector));
            if insert_nesting {
                *selector = super::selector_operations::relative_to(selector, SimpleSelector::Nesting);
            } else if first == Combinator::Descendant {
                let mut compounds = selector.compound_selectors.clone();
                compounds[0].combinator = Combinator::None;
                *selector = CompiledSelector::new(compounds);
            }
        }
        // Adding an implicit nesting selector does not introduce any names to bind.
        self
    }

    pub(crate) fn new(selectors: SelectorList) -> Self {
        Self { selectors }
    }

    #[cfg(test)]
    pub(crate) fn selectors(&self) -> &SelectorList {
        &self.selectors
    }

    pub(crate) fn contains_pseudo_element(&self) -> bool {
        self.selectors
            .iter()
            .any(|selector| selector.target_pseudo_element.is_some())
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.selectors.is_empty()
    }
}

/// # Safety
/// `input` and each namespace view must point to readable storage for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_parse(
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

/// # Safety
/// Input and the optional immutable namespace context must be readable for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_parse_with_namespace_context(
    input: FfiUtf16View,
    namespaces: *const std::ffi::c_void,
    is_relative: bool,
    is_forgiving: bool,
) -> *mut RustParsedSelectorList {
    let Some(input) = (unsafe { input.units() }) else {
        return std::ptr::null_mut();
    };
    let namespaces = unsafe { namespaces.cast::<crate::css::rule::NativeNamespaceContext>().as_ref() };
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
        Box::into_raw(Box::new(RustParsedSelectorList::new(selectors)))
    })
}

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
            std::slice::from_raw_parts(namespaces, namespace_count)
        };
        let namespace_storage = namespace_views
            .iter()
            .map(|namespace| {
                if namespace.length == 0 {
                    TokenizerInput::Utf16(&[])
                } else {
                    assert!(!namespace.data.is_null());
                    TokenizerInput::Utf16(std::slice::from_raw_parts(namespace.data, namespace.length))
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
        Some(RustParsedSelectorList::new(selectors).adapt_for_nesting(nesting_parent))
    }
}

#[repr(C)]
pub struct FfiParsedPseudoElement {
    pub selector: *mut RustSelector,
    pub pseudo_element: u8,
}

/// Parses the restricted pseudo-element syntax accepted by `getComputedStyle()` and animation APIs.
///
/// # Safety
/// `input` must contain readable storage for the duration of this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_selector_parse_pseudo_element(input: FfiUtf16View) -> FfiParsedPseudoElement {
    unsafe {
        let Some(input) = input.units() else {
            return FfiParsedPseudoElement {
                selector: std::ptr::null_mut(),
                pseudo_element: u8::MAX,
            };
        };
        let Ok((selector, pseudo_element)) = parse_pseudo_element_selector(input) else {
            return FfiParsedPseudoElement {
                selector: std::ptr::null_mut(),
                pseudo_element: u8::MAX,
            };
        };
        FfiParsedPseudoElement {
            selector: Box::into_raw(Box::new(RustSelector { selector })),
            pseudo_element: pseudo_element as u8,
        }
    }
}

/// # Safety
/// `list` must be null or an owned selector list that has not been destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parsed_selector_list_destroy(list: *mut RustParsedSelectorList) {
    unsafe {
        if !list.is_null() {
            drop(Box::from_raw(list));
        }
    }
}

/// # Safety
/// `list` must point to a live parsed selector list.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parsed_selector_list_length(list: *const RustParsedSelectorList) -> usize {
    unsafe { (&*list).selectors.len() }
}

/// # Safety
/// `list` must point to a live selector list and `index` must be in bounds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parsed_selector_list_selector(
    list: *const RustParsedSelectorList,
    index: usize,
) -> *mut RustSelector {
    unsafe {
        Box::into_raw(Box::new(RustSelector {
            selector: (&(*list).selectors)[index].clone(),
        }))
    }
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
            let parsed = rust_selector_parse(
                FfiUtf16View {
                    utf16: units.as_ptr(),
                    length: units.len(),
                    ..Default::default()
                },
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

    fn serialize(selector: &Arc<CompiledSelector>) -> Vec<u16> {
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
            assert!(Arc::ptr_eq(&first.selectors[0], &second.selectors[0]));
            drop(parsed);
            let expected = serialize(&first.selectors[0]);
            assert_eq!(serialize(&second.selectors[0]), expected);
            assert_eq!(first.selectors[0].specificity(), second.selectors[0].specificity());
            drop(first);
            assert_eq!(serialize(&second.selectors[0]), expected);
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
            serialize(&bound.selectors[0]),
            "é|DIV#😀:is(.é, :not([data-name=\"😀\"])):nth-child(2n+1 of .item)::part(label)"
                .encode_utf16()
                .collect::<Vec<_>>()
        );
    }
}
