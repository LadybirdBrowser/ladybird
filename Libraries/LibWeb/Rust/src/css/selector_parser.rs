/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSS selector parsing.

use std::rc::Rc;

use super::css_tokenizer::{CssNumberType, ParserToken, ParserTokenKind, tokenize_for_parser};
use super::ffi_support::ascii_lowercase;
use super::selector::{
    AnPlusBPattern, AttributeCaseType, AttributeMatchType, AttributeSelector, Combinator, CompiledSelector,
    CompoundSelector, Direction, NameSelector, NamespaceType, PseudoClassParameterType, PseudoClassSelector,
    PseudoClassType, PseudoElementParameterType, PseudoElementSelector, PseudoElementType, PseudoElementValue,
    QualifiedName, SelectorList, SelectorString, SimpleSelector,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelectorType {
    Standalone,
    Relative,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SelectorParsingMode {
    Standard,
    Forgiving,
}

#[derive(Clone, Debug, PartialEq)]
enum ComponentKind {
    Token(ParserTokenKind),
    Function {
        name: SelectorString,
        value: Box<[ComponentValue]>,
    },
    SimpleBlock {
        opening: ParserTokenKind,
        value: Box<[ComponentValue]>,
    },
}

#[derive(Clone, Debug, PartialEq)]
struct ComponentValue {
    kind: ComponentKind,
    source: SelectorString,
}

impl ComponentValue {
    fn is_whitespace(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Whitespace))
    }

    fn is_comma(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Comma))
    }

    fn is_colon(&self) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Colon))
    }

    fn is_delim(&self, expected: u8) -> bool {
        matches!(self.kind, ComponentKind::Token(ParserTokenKind::Delim(value)) if value == u32::from(expected))
    }

    fn ident(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::Ident(value)) => Some(value),
            _ => None,
        }
    }

    fn string(&self) -> Option<&[u16]> {
        match &self.kind {
            ComponentKind::Token(ParserTokenKind::String(value)) => Some(value),
            _ => None,
        }
    }

    fn function(&self) -> Option<(&[u16], &[ComponentValue])> {
        match &self.kind {
            ComponentKind::Function { name, value } => Some((name, value)),
            _ => None,
        }
    }

    fn square_block(&self) -> Option<&[ComponentValue]> {
        match &self.kind {
            ComponentKind::SimpleBlock {
                opening: ParserTokenKind::OpenSquare,
                value,
            } => Some(value),
            _ => None,
        }
    }
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
        .into_boxed_slice()
}

fn selector_name(value: &[u16]) -> NameSelector {
    NameSelector {
        name: value.into(),
        interned_name: None,
        interned_lowercase_name: None,
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
        identifier_identity: None,
        identifier_lowercase_identity: None,
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

fn integer(value: &ComponentValue) -> Option<(i32, bool)> {
    let ComponentKind::Token(ParserTokenKind::Number { value, number_type }) = value.kind else {
        return None;
    };
    if !matches!(
        number_type,
        CssNumberType::Integer | CssNumberType::IntegerWithExplicitSign
    ) || !value.is_finite()
        || value.fract() != 0.0
        || value < f64::from(i32::MIN)
        || value > f64::from(i32::MAX)
    {
        return None;
    }
    Some((value as i32, number_type == CssNumberType::IntegerWithExplicitSign))
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
    ) || !value.is_finite()
        || value.fract() != 0.0
        || *value < f64::from(i32::MIN)
        || *value > f64::from(i32::MAX)
    {
        return None;
    }
    Some((*value as i32, unit))
}

fn ascii_i32(value: &[u16]) -> Option<i32> {
    let bytes = value
        .iter()
        .map(|&unit| u8::try_from(unit).ok())
        .collect::<Option<Vec<_>>>()?;
    std::str::from_utf8(&bytes).ok()?.parse().ok()
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
    let child = stream.position;
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
    stream.position = child;
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
            let offset = ascii_i32(&unit[1..])?;
            stream.position += 1;
            return Some(AnPlusBPattern { step_size, offset });
        }
    }
    if let Some(ident) = stream.peek().and_then(ComponentValue::ident) {
        if ident.len() > 3 && ascii_starts_with(ident, "-n-") && digits(&ident[3..]) {
            let offset = ascii_i32(&ident[2..])?;
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
        let offset = ascii_i32(&ident[1..])?;
        return Some(AnPlusBPattern { step_size: 1, offset });
    }
    stream.position = original;
    None
}

fn append_source(target: &mut Vec<u16>, source: &[u16]) {
    target.extend_from_slice(source);
}

fn consume_component_values(
    tokens: &[ParserToken],
    position: &mut usize,
    ending: Option<&ParserTokenKind>,
) -> Vec<ComponentValue> {
    let mut values = Vec::new();
    while let Some(token) = tokens.get(*position) {
        let closes_current = matches!(
            (&token.kind, ending),
            (ParserTokenKind::CloseParen, Some(ParserTokenKind::CloseParen))
                | (ParserTokenKind::CloseSquare, Some(ParserTokenKind::CloseSquare))
                | (ParserTokenKind::CloseCurly, Some(ParserTokenKind::CloseCurly))
        );
        if closes_current {
            break;
        }

        *position += 1;
        let mut source = token.source.to_vec();
        let kind = match &token.kind {
            ParserTokenKind::Function(name) => {
                let value = consume_component_values(tokens, position, Some(&ParserTokenKind::CloseParen));
                for component in &value {
                    append_source(&mut source, &component.source);
                }
                if let Some(closing) = tokens.get(*position)
                    && matches!(closing.kind, ParserTokenKind::CloseParen)
                {
                    append_source(&mut source, &closing.source);
                    *position += 1;
                }
                ComponentKind::Function {
                    name: name.clone(),
                    value: value.into_boxed_slice(),
                }
            }
            ParserTokenKind::OpenSquare | ParserTokenKind::OpenParen | ParserTokenKind::OpenCurly => {
                let ending = match token.kind {
                    ParserTokenKind::OpenSquare => ParserTokenKind::CloseSquare,
                    ParserTokenKind::OpenParen => ParserTokenKind::CloseParen,
                    ParserTokenKind::OpenCurly => ParserTokenKind::CloseCurly,
                    _ => unreachable!(),
                };
                let value = consume_component_values(tokens, position, Some(&ending));
                for component in &value {
                    append_source(&mut source, &component.source);
                }
                if let Some(closing) = tokens.get(*position)
                    && std::mem::discriminant(&closing.kind) == std::mem::discriminant(&ending)
                {
                    append_source(&mut source, &closing.source);
                    *position += 1;
                }
                ComponentKind::SimpleBlock {
                    opening: token.kind.clone(),
                    value: value.into_boxed_slice(),
                }
            }
            _ => ComponentKind::Token(token.kind.clone()),
        };
        values.push(ComponentValue {
            kind,
            source: source.into_boxed_slice(),
        });
    }
    values
}

#[derive(Clone)]
struct Stream<'a> {
    values: &'a [ComponentValue],
    position: usize,
}

impl<'a> Stream<'a> {
    fn new(values: &'a [ComponentValue]) -> Self {
        Self { values, position: 0 }
    }

    fn peek(&self) -> Option<&'a ComponentValue> {
        self.values.get(self.position)
    }

    fn peek_n(&self, offset: usize) -> Option<&'a ComponentValue> {
        self.values.get(self.position + offset)
    }

    fn next(&mut self) -> Option<&'a ComponentValue> {
        let value = self.peek()?;
        self.position += 1;
        Some(value)
    }

    fn is_empty(&self) -> bool {
        self.position == self.values.len()
    }

    fn discard_whitespace(&mut self) {
        while self.peek().is_some_and(ComponentValue::is_whitespace) {
            self.position += 1;
        }
    }
}

struct SelectorParser<'a> {
    declared_namespaces: &'a [&'a [u16]],
    pseudo_class_context: Vec<PseudoClassType>,
}

impl<'a> SelectorParser<'a> {
    fn new(declared_namespaces: &'a [&'a [u16]]) -> Self {
        Self {
            declared_namespaces,
            pseudo_class_context: Vec::new(),
        }
    }

    fn parse_selector_list(
        &mut self,
        values: &[ComponentValue],
        selector_type: SelectorType,
        parsing_mode: SelectorParsingMode,
    ) -> Result<SelectorList, ()> {
        let mut selectors = Vec::new();
        let mut start = 0;
        loop {
            let end = values[start..]
                .iter()
                .position(ComponentValue::is_comma)
                .map_or(values.len(), |offset| start + offset);
            let selector_values = &values[start..end];
            match self.parse_complex_selector(selector_values, selector_type) {
                Ok(selector) => selectors.push(selector),
                Err(()) if parsing_mode == SelectorParsingMode::Forgiving => {
                    let mut source = selector_values
                        .iter()
                        .flat_map(|value| value.source.iter().copied())
                        .collect::<Vec<_>>();
                    while source.first() == Some(&(b' ' as u16)) {
                        source.remove(0);
                    }
                    while source.last() == Some(&(b' ' as u16)) {
                        source.pop();
                    }
                    selectors.push(CompiledSelector::new(
                        vec![CompoundSelector {
                            combinator: if selector_type == SelectorType::Standalone {
                                Combinator::None
                            } else {
                                Combinator::Descendant
                            },
                            is_implicit_universal_anchor: false,
                            simple_selectors: vec![SimpleSelector::Invalid(source.into_boxed_slice())]
                                .into_boxed_slice(),
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
    ) -> Result<Rc<CompiledSelector>, ()> {
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
                return Some(Box::new([b'*' as u16]));
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
            return Some(QualifiedName {
                namespace_type: NamespaceType::None,
                namespace: Box::new([]),
                lowercase_name: lowercase(&parsed_name),
                name: parsed_name,
                interned_name: None,
                interned_lowercase_name: None,
                interned_namespace: None,
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
                && !self
                    .declared_namespaces
                    .iter()
                    .any(|namespace| *namespace == first_name.as_ref())
            {
                stream.position = original;
                return None;
            }
            return Some(QualifiedName {
                namespace_type,
                namespace: first_name,
                lowercase_name: lowercase(&parsed_name),
                name: parsed_name,
                interned_name: None,
                interned_lowercase_name: None,
                interned_namespace: None,
            });
        }
        if !allow_wildcard && first_name.as_ref() == [b'*' as u16] {
            stream.position = original;
            return None;
        }
        Some(QualifiedName {
            namespace_type: NamespaceType::Default,
            namespace: Box::new([]),
            lowercase_name: lowercase(&first_name),
            name: first_name,
            interned_name: None,
            interned_lowercase_name: None,
            interned_namespace: None,
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
                SimpleSelector::Universal(qualified_name)
            } else {
                SimpleSelector::TagName(qualified_name)
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
            return Ok(SimpleSelector::Attribute(AttributeSelector {
                match_type: AttributeMatchType::HasAttribute,
                qualified_name,
                value: Box::new([]),
                value_identity: None,
                case_type: AttributeCaseType::Default,
            }));
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
        let value = stream
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
        Ok(SimpleSelector::Attribute(AttributeSelector {
            match_type,
            qualified_name,
            value,
            value_identity: None,
            case_type,
        }))
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
            return Ok(SimpleSelector::PseudoClass(pseudo_class_selector(pseudo_class)));
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

        self.pseudo_class_context.push(pseudo_class);
        let result = self.parse_pseudo_class_function(pseudo_class, metadata.parameter_type, function_values);
        self.pseudo_class_context.pop();
        result.map(SimpleSelector::PseudoClass)
    }

    fn parse_pseudo_class_function(
        &mut self,
        pseudo_class: PseudoClassType,
        parameter_type: PseudoClassParameterType,
        values: &[ComponentValue],
    ) -> Result<PseudoClassSelector, ()> {
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
                selector.identifier = Some(ident.into());
            }
            PseudoClassParameterType::LanguageRanges => {
                let mut languages = Vec::new();
                for values in split_on_commas(values) {
                    let mut stream = Stream::new(values);
                    stream.discard_whitespace();
                    let language = stream
                        .next()
                        .and_then(|value| value.ident().or_else(|| value.string()))
                        .ok_or(())?;
                    stream.discard_whitespace();
                    if !stream.is_empty() {
                        return Err(());
                    }
                    languages.push(language.into());
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
                return Ok(SimpleSelector::PseudoElement(PseudoElementSelector {
                    pseudo_element: PseudoElementType::UnknownWebKit,
                    serialized_name: Some(lowercase(name)),
                    value: PseudoElementValue::None,
                    identifier_identities: Box::new([]),
                }));
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
            return Ok(SimpleSelector::PseudoElement(PseudoElementSelector {
                pseudo_element,
                serialized_name: alias.map(|alias| alias.encode_utf16().collect::<Vec<_>>().into_boxed_slice()),
                value: PseudoElementValue::None,
                identifier_identities: Box::new([]),
            }));
        }

        let metadata = pseudo_element.metadata();
        let value = match function_values {
            Some(_) if !metadata.is_valid_as_function => return Err(()),
            None if !metadata.is_valid_as_identifier => return Err(()),
            None => PseudoElementValue::None,
            Some(values) => self.parse_pseudo_element_function(metadata.parameter_type, values)?,
        };
        Ok(SimpleSelector::PseudoElement(PseudoElementSelector {
            pseudo_element,
            serialized_name: alias.map(|alias| alias.encode_utf16().collect::<Vec<_>>().into_boxed_slice()),
            value,
            identifier_identities: Box::new([]),
        }))
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
                Ok(PseudoElementValue::Identifiers(identifiers.into_boxed_slice()))
            }
            PseudoElementParameterType::PTNameSelector => {
                let (is_universal, value) = if stream.peek().is_some_and(|value| value.is_delim(b'*')) {
                    stream.position += 1;
                    (true, Box::new([]) as SelectorString)
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
                        simple_selectors: vec![SimpleSelector::Universal(QualifiedName {
                            namespace_type: NamespaceType::Any,
                            namespace: Box::new([]),
                            name: Box::new([b'*' as u16]),
                            lowercase_name: Box::new([b'*' as u16]),
                            interned_name: None,
                            interned_lowercase_name: None,
                            interned_namespace: None,
                        })]
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

pub(crate) fn parse_selector_list(
    input: &[u8],
    declared_namespaces: &[&[u16]],
    selector_type: SelectorType,
    parsing_mode: SelectorParsingMode,
) -> Result<SelectorList, ()> {
    let tokens = tokenize_for_parser(input);
    let mut position = 0;
    let values = consume_component_values(&tokens, &mut position, None);
    SelectorParser::new(declared_namespaces).parse_selector_list(&values, selector_type, parsing_mode)
}
