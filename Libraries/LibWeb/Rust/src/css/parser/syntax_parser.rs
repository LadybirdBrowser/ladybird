/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{keyword_from_ascii_case_insensitive, keyword_to_generic_font_family};
use crate::css::css_tokenizer::{
    ParserString, ParserToken, ParserTokenKind, SourcePosition, TokenizerInput, tokenize_for_parser,
};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{
    ComponentKind, ComponentValue, consume_a_component_value, consume_a_list_of_component_values, trim_whitespace,
};
use crate::css::parser::descriptor_parser::parse_descriptor;
use crate::css::parser::value_parser::{
    FfiParseStatus, FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome,
    is_valid_custom_ident, parse_css_value_with_utf16_source,
};
use std::borrow::Cow;
use std::ffi::c_void;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[allow(dead_code)] // SupportsCondition is constructed through the C++ FFI.
pub(crate) enum RuleContext {
    Unknown,
    Style,
    AtContainer,
    AtCounterStyle,
    AtMedia,
    AtFontFace,
    AtFontFeatureValues,
    FontFeatureValue,
    AtFunction,
    AtKeyframes,
    Keyframe,
    AtSupports,
    AtScope,
    SupportsCondition,
    AtLayer,
    AtProperty,
    AtPage,
    Margin,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum Rule {
    At(AtRule),
    Qualified(QualifiedRule),
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) enum RuleOrDeclarations {
    Rule(Rule),
    Declarations(Vec<Declaration>),
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct AtRule {
    pub name: ParserString,
    pub prelude: Vec<ComponentValue>,
    pub children: Vec<RuleOrDeclarations>,
    pub has_block: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct QualifiedRule {
    pub prelude: Vec<ComponentValue>,
    pub prelude_is_selector: bool,
    pub declarations: Vec<Declaration>,
    pub children: Vec<RuleOrDeclarations>,
    pub source_position: Option<SourcePosition>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct Declaration {
    pub name: ParserString,
    pub value: Vec<ComponentValue>,
    pub important: bool,
    pub original_value_text: Option<Box<[u16]>>,
    pub original_full_text: Option<Box<[u16]>>,
    pub source_position: SourcePosition,
    pub is_property: bool,
    pub rule_context: RuleContext,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Nested {
    No,
    Yes,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum QualifiedRuleResult {
    Invalid,
    InvalidInContext,
}

#[derive(Clone, Debug, PartialEq)]
enum ParsedRulePrelude {
    Unparsed,
    Invalid,
    Empty,
    Name(ParserString),
    Names(Vec<ParserString>),
    KeyframeSelectors(Vec<f64>),
    Namespace {
        prefix: Option<ParserString>,
        uri: ParserString,
    },
    PageSelectors(Vec<ParsedPageSelector>),
    FontFamilyNames(Vec<ParserString>),
    Scope {
        start: Option<ParserString>,
        end: Option<ParserString>,
    },
    Import(Vec<(Option<ParserString>, u8)>),
    Function {
        name: ParserString,
        parameters: Vec<(ParserString, Option<ParserString>, Option<ParserString>)>,
        return_type: Option<ParserString>,
    },
}

#[derive(Clone, Debug, PartialEq)]
struct ParsedPageSelector {
    name: Option<ParserString>,
    pseudo_classes: Vec<u8>,
}

fn non_whitespace(values: &[ComponentValue]) -> Vec<&ComponentValue> {
    values.iter().filter(|value| !value.is_whitespace()).collect()
}

fn parse_single_name(values: &[ComponentValue], validate: impl Fn(&[u16]) -> bool) -> ParsedRulePrelude {
    let values = non_whitespace(values);
    let Some(name) = values.as_slice().first().and_then(|value| value.ident()) else {
        return ParsedRulePrelude::Invalid;
    };
    if values.len() != 1 || !validate(name) {
        return ParsedRulePrelude::Invalid;
    }
    ParsedRulePrelude::Name(name.to_vec().into_boxed_slice().into())
}

fn is_css_wide_identifier(value: &[u16]) -> bool {
    [b"inherit".as_slice(), b"initial", b"unset", b"revert", b"revert-layer"]
        .iter()
        .any(|keyword| equals_ascii_case_insensitive(value, keyword))
}

fn parse_layer_name(values: &[ComponentValue], allow_blank: bool) -> Option<ParserString> {
    let mut position = 0;
    while values.get(position).is_some_and(ComponentValue::is_whitespace) {
        position += 1;
    }
    if position == values.len() && allow_blank {
        return Some(Vec::new().into_boxed_slice().into());
    }
    let first = values.get(position)?.ident()?;
    if is_css_wide_identifier(first) {
        return None;
    }
    let mut name = first.to_vec();
    position += 1;
    while values.get(position).is_some_and(|value| value.is_delim(b'.')) {
        let part = values.get(position + 1)?.ident()?;
        if is_css_wide_identifier(part) {
            return None;
        }
        name.push(u16::from(b'.'));
        name.extend_from_slice(part);
        position += 2;
    }
    while values.get(position).is_some_and(ComponentValue::is_whitespace) {
        position += 1;
    }
    (position == values.len()).then(|| name.into_boxed_slice().into())
}

fn parse_layer_prelude(values: &[ComponentValue], has_block: bool) -> ParsedRulePrelude {
    if has_block {
        return parse_layer_name(values, true)
            .map(ParsedRulePrelude::Name)
            .unwrap_or(ParsedRulePrelude::Invalid);
    }
    let names = values
        .split(ComponentValue::is_comma)
        .map(|values| parse_layer_name(values, false))
        .collect::<Option<Vec<_>>>();
    match names {
        Some(names) if !names.is_empty() => ParsedRulePrelude::Names(names),
        _ => ParsedRulePrelude::Invalid,
    }
}

fn parse_keyframe_selectors(values: &[ComponentValue]) -> ParsedRulePrelude {
    let mut selectors = Vec::new();
    for selector in values.split(ComponentValue::is_comma) {
        let selector = non_whitespace(selector);
        let Some(value) = selector.as_slice().first() else {
            return ParsedRulePrelude::Invalid;
        };
        if selector.len() != 1 {
            return ParsedRulePrelude::Invalid;
        }
        let percentage = if value
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"from"))
        {
            0.0
        } else if value
            .ident()
            .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"to"))
        {
            100.0
        } else if let ComponentKind::Token(ParserTokenKind::Percentage { value, .. }) = &value.kind {
            *value
        } else {
            return ParsedRulePrelude::Invalid;
        };
        if !(0.0..=100.0).contains(&percentage) {
            return ParsedRulePrelude::Invalid;
        }
        selectors.push(percentage);
    }
    if selectors.is_empty() {
        ParsedRulePrelude::Invalid
    } else {
        ParsedRulePrelude::KeyframeSelectors(selectors)
    }
}

fn parse_namespace_prelude(values: &[ComponentValue]) -> ParsedRulePrelude {
    let values = non_whitespace(values);
    let (prefix, uri_value) = match values.as_slice() {
        [uri] => (None, *uri),
        [prefix, uri] => {
            let Some(prefix) = prefix.ident() else {
                return ParsedRulePrelude::Invalid;
            };
            (Some(prefix.to_vec().into_boxed_slice().into()), *uri)
        }
        _ => return ParsedRulePrelude::Invalid,
    };
    let uri = match &uri_value.kind {
        ComponentKind::Token(ParserTokenKind::Url(value) | ParserTokenKind::String(value)) => value.clone(),
        ComponentKind::Function { name, values } if equals_ascii_case_insensitive(name, b"url") => {
            let values = non_whitespace(values);
            let Some(value) = values.as_slice().first().and_then(|value| value.string()) else {
                return ParsedRulePrelude::Invalid;
            };
            if values.len() != 1 {
                return ParsedRulePrelude::Invalid;
            }
            value.to_vec().into_boxed_slice().into()
        }
        _ => return ParsedRulePrelude::Invalid,
    };
    ParsedRulePrelude::Namespace { prefix, uri }
}

fn parse_page_selectors(values: &[ComponentValue]) -> ParsedRulePrelude {
    if non_whitespace(values).is_empty() {
        return ParsedRulePrelude::PageSelectors(Vec::new());
    }
    let mut selectors = Vec::new();
    for selector in values.split(ComponentValue::is_comma) {
        let values = trim_whitespace(selector);
        if values.is_empty() {
            return ParsedRulePrelude::Invalid;
        }
        if values.iter().any(|value| value.is_whitespace()) {
            return ParsedRulePrelude::Invalid;
        }
        let mut position = 0;
        let name = values.get(position).and_then(|value| value.ident()).map(|name| {
            position += 1;
            ParserString::from(name.to_vec().into_boxed_slice())
        });
        let mut pseudo_classes = Vec::new();
        while position < values.len() {
            if !values[position].is_colon() {
                return ParsedRulePrelude::Invalid;
            }
            position += 1;
            let Some(pseudo_class) = values.get(position).and_then(|value| value.ident()) else {
                return ParsedRulePrelude::Invalid;
            };
            let pseudo_class = if equals_ascii_case_insensitive(pseudo_class, b"left") {
                0
            } else if equals_ascii_case_insensitive(pseudo_class, b"right") {
                1
            } else if equals_ascii_case_insensitive(pseudo_class, b"first") {
                2
            } else if equals_ascii_case_insensitive(pseudo_class, b"blank") {
                3
            } else {
                return ParsedRulePrelude::Invalid;
            };
            pseudo_classes.push(pseudo_class);
            position += 1;
        }
        if name.is_none() && pseudo_classes.is_empty() {
            return ParsedRulePrelude::Invalid;
        }
        selectors.push(ParsedPageSelector { name, pseudo_classes });
    }
    ParsedRulePrelude::PageSelectors(selectors)
}

fn parse_font_family_names(values: &[ComponentValue]) -> ParsedRulePrelude {
    let mut names = Vec::new();
    for family in values.split(ComponentValue::is_comma) {
        let values = non_whitespace(family);
        let name = if let [value] = values.as_slice()
            && let Some(string) = value.string()
        {
            string.to_vec()
        } else {
            let Some(parts) = values.iter().map(|value| value.ident()).collect::<Option<Vec<_>>>() else {
                return ParsedRulePrelude::Invalid;
            };
            if parts.is_empty()
                || parts.len() == 1
                    && (!is_valid_custom_ident(parts[0], &[])
                        || keyword_from_ascii_case_insensitive(parts[0])
                            .is_some_and(|keyword| keyword_to_generic_font_family(keyword).is_some()))
            {
                return ParsedRulePrelude::Invalid;
            }
            let mut name = Vec::new();
            for (index, part) in parts.iter().enumerate() {
                if index != 0 {
                    name.push(u16::from(b' '));
                }
                name.extend_from_slice(part);
            }
            name
        };
        names.push(name.into_boxed_slice().into());
    }
    if names.is_empty() {
        ParsedRulePrelude::Invalid
    } else {
        ParsedRulePrelude::FontFamilyNames(names)
    }
}

fn parenthesized_source(value: &ComponentValue) -> Option<ParserString> {
    let ComponentKind::SimpleBlock {
        opening: ParserTokenKind::OpenParen,
        values,
    } = &value.kind
    else {
        return None;
    };
    Some(component_list_source(values).into_owned().into_boxed_slice().into())
}

fn parse_scope_prelude(values: &[ComponentValue]) -> ParsedRulePrelude {
    let values = non_whitespace(values);
    let mut position = 0;
    let start = values.get(position).and_then(|value| {
        let source = parenthesized_source(value)?;
        position += 1;
        Some(source)
    });
    let end = if values
        .get(position)
        .and_then(|value| value.ident())
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"to"))
    {
        position += 1;
        let Some(source) = values.get(position).and_then(|value| parenthesized_source(value)) else {
            return ParsedRulePrelude::Invalid;
        };
        position += 1;
        Some(source)
    } else {
        None
    };
    if position != values.len() {
        return ParsedRulePrelude::Invalid;
    }
    ParsedRulePrelude::Scope { start, end }
}

fn skip_whitespace(values: &[ComponentValue], position: &mut usize) {
    while values.get(*position).is_some_and(ComponentValue::is_whitespace) {
        *position += 1;
    }
}

fn component_slice_source(values: &[ComponentValue]) -> ParserString {
    component_list_source(values).into_owned().into_boxed_slice().into()
}

fn parse_import_scope(contents: &[ComponentValue]) -> Option<(Option<ParserString>, Option<ParserString>)> {
    let mut position = 0;
    skip_whitespace(contents, &mut position);
    let start = contents.get(position).and_then(|value| {
        let source = parenthesized_source(value)?;
        position += 1;
        Some(source)
    });
    skip_whitespace(contents, &mut position);
    let end = if contents
        .get(position)
        .and_then(|value| value.ident())
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"to"))
    {
        position += 1;
        skip_whitespace(contents, &mut position);
        let source = contents.get(position).and_then(parenthesized_source)?;
        position += 1;
        Some(source)
    } else {
        None
    };
    skip_whitespace(contents, &mut position);
    let start = if start.is_none() && end.is_none() && position < contents.len() {
        let mut previous_non_whitespace = None;
        for value in &contents[position..] {
            if value.is_whitespace() {
                continue;
            }
            if value
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"to"))
                && previous_non_whitespace
                    .is_none_or(|previous: &ComponentValue| !previous.is_delim(b'.') && !previous.is_colon())
            {
                return None;
            }
            previous_non_whitespace = Some(value);
        }
        let source = component_slice_source(&contents[position..]);
        position = contents.len();
        Some(source)
    } else {
        start
    };
    (position == contents.len()).then_some((start, end))
}

fn parse_import_prelude(values: &[ComponentValue]) -> ParsedRulePrelude {
    let mut position = 0;
    skip_whitespace(values, &mut position);
    let Some(url) = values.get(position) else {
        return ParsedRulePrelude::Invalid;
    };
    let url_item = match &url.kind {
        ComponentKind::Token(ParserTokenKind::Url(url) | ParserTokenKind::String(url)) => (Some(url.clone()), 7),
        ComponentKind::Function { name, .. } if equals_ascii_case_insensitive(name, b"url") => {
            (Some(component_slice_source(std::slice::from_ref(url))), 0)
        }
        _ => return ParsedRulePrelude::Invalid,
    };
    let mut items = vec![url_item];
    position += 1;
    let mut has_layer = false;
    let mut has_scope = false;
    let mut has_supports = false;
    loop {
        skip_whitespace(values, &mut position);
        let Some(value) = values.get(position) else {
            break;
        };
        if !has_layer
            && value
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"layer"))
        {
            items.push((Some(Vec::new().into_boxed_slice().into()), 1));
            has_layer = true;
            position += 1;
            continue;
        }
        if !has_layer
            && let Some((name, contents)) = value.function()
            && equals_ascii_case_insensitive(name, b"layer")
            && let Some(layer) = parse_layer_name(contents, false)
        {
            items.push((Some(layer), 1));
            has_layer = true;
            position += 1;
            continue;
        }
        if !has_scope
            && value
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"scope"))
        {
            items.push((None, 2));
            has_scope = true;
            position += 1;
            continue;
        }
        if !has_scope
            && let Some((name, contents)) = value.function()
            && equals_ascii_case_insensitive(name, b"scope")
            && let Some((start, end)) = parse_import_scope(contents)
        {
            items.push((None, 2));
            if let Some(start) = start {
                items.push((Some(start), 3));
            }
            if let Some(end) = end {
                items.push((Some(end), 4));
            }
            has_scope = true;
            position += 1;
            continue;
        }
        if !has_supports
            && let Some((name, contents)) = value.function()
            && equals_ascii_case_insensitive(name, b"supports")
        {
            items.push((Some(component_slice_source(contents)), 5));
            has_supports = true;
            position += 1;
            continue;
        }
        break;
    }
    if position < values.len() {
        items.push((Some(component_slice_source(&values[position..])), 6));
    }
    ParsedRulePrelude::Import(items)
}

fn parse_css_type(values: &[ComponentValue], position: &mut usize) -> Option<ParserString> {
    let start = *position;
    if let Some(consumed) = crate::css::parser::syntax::parse_syntax_component_values(&values[start..], false) {
        *position += consumed;
        return Some(component_slice_source(&values[start..*position]));
    }
    skip_whitespace(values, position);
    let (name, contents) = values.get(*position)?.function()?;
    if !equals_ascii_case_insensitive(name, b"type") {
        *position = start;
        return None;
    }
    *position += 1;
    Some(component_slice_source(contents))
}

fn parse_function_prelude(values: &[ComponentValue]) -> ParsedRulePrelude {
    let mut position = 0;
    skip_whitespace(values, &mut position);
    let Some((name, parameter_values)) = values.get(position).and_then(ComponentValue::function) else {
        return ParsedRulePrelude::Invalid;
    };
    // https://drafts.csswg.org/css-mixins-1/#function-prelude
    // The <function-token> production must start with two dashes (U+002D HYPHEN-MINUS), similar to <dashed-ident>, or
    // else the definition is invalid.
    if !name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) {
        return ParsedRulePrelude::Invalid;
    }
    position += 1;
    let mut parameters = Vec::new();
    if !non_whitespace(parameter_values).is_empty() {
        for parameter in parameter_values.split(ComponentValue::is_comma) {
            let mut parameter_position = 0;
            skip_whitespace(parameter, &mut parameter_position);
            let Some(parameter_name) = parameter.get(parameter_position).and_then(ComponentValue::ident) else {
                return ParsedRulePrelude::Invalid;
            };
            if !parameter_name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) || parameter_name.len() <= 2 {
                return ParsedRulePrelude::Invalid;
            }
            parameter_position += 1;
            let type_source = parse_css_type(parameter, &mut parameter_position);
            skip_whitespace(parameter, &mut parameter_position);
            let default_source = if parameter.get(parameter_position).is_some_and(ComponentValue::is_colon) {
                parameter_position += 1;
                let source = component_slice_source(&parameter[parameter_position..]);
                parameter_position = parameter.len();
                Some(source)
            } else {
                None
            };
            skip_whitespace(parameter, &mut parameter_position);
            if parameter_position != parameter.len() {
                return ParsedRulePrelude::Invalid;
            }
            parameters.push((
                parameter_name.to_vec().into_boxed_slice().into(),
                type_source,
                default_source,
            ));
        }
    }
    skip_whitespace(values, &mut position);
    let return_type = if values
        .get(position)
        .and_then(ComponentValue::ident)
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"returns"))
    {
        position += 1;
        let Some(return_type) = parse_css_type(values, &mut position) else {
            return ParsedRulePrelude::Invalid;
        };
        Some(return_type)
    } else {
        None
    };
    skip_whitespace(values, &mut position);
    if position != values.len() {
        return ParsedRulePrelude::Invalid;
    }
    ParsedRulePrelude::Function {
        name: name.to_vec().into_boxed_slice().into(),
        parameters,
        return_type,
    }
}

fn parse_rule_prelude(rule: &Rule) -> ParsedRulePrelude {
    match rule {
        Rule::Qualified(rule) if !rule.prelude_is_selector => parse_keyframe_selectors(&rule.prelude),
        Rule::Qualified(_) => ParsedRulePrelude::Unparsed,
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"layer") => {
            parse_layer_prelude(&rule.prelude, rule.has_block)
        }
        Rule::At(rule)
            if equals_ascii_case_insensitive(rule.name.as_ref(), b"property")
                || equals_ascii_case_insensitive(rule.name.as_ref(), b"counter-style") =>
        {
            let blacklist = if equals_ascii_case_insensitive(rule.name.as_ref(), b"counter-style") {
                ["none"].as_slice()
            } else {
                [].as_slice()
            };
            parse_single_name(&rule.prelude, |name| {
                if equals_ascii_case_insensitive(rule.name.as_ref(), b"property") {
                    name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) && name.len() > 2
                } else {
                    is_valid_custom_ident(name, blacklist)
                }
            })
        }
        Rule::At(rule)
            if equals_ascii_case_insensitive(rule.name.as_ref(), b"keyframes")
                || equals_ascii_case_insensitive(rule.name.as_ref(), b"-webkit-keyframes") =>
        {
            let values = non_whitespace(&rule.prelude);
            match values.as_slice() {
                [value] if value.string().is_some() => {
                    ParsedRulePrelude::Name(value.string().unwrap().to_vec().into_boxed_slice().into())
                }
                [value] if value.ident().is_some_and(|name| is_valid_custom_ident(name, &["none"])) => {
                    ParsedRulePrelude::Name(value.ident().unwrap().to_vec().into_boxed_slice().into())
                }
                _ => ParsedRulePrelude::Invalid,
            }
        }
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"namespace") => {
            parse_namespace_prelude(&rule.prelude)
        }
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"page") => {
            parse_page_selectors(&rule.prelude)
        }
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"font-feature-values") => {
            parse_font_family_names(&rule.prelude)
        }
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"scope") => {
            parse_scope_prelude(&rule.prelude)
        }
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"import") => {
            parse_import_prelude(&rule.prelude)
        }
        Rule::At(rule) if equals_ascii_case_insensitive(rule.name.as_ref(), b"function") => {
            parse_function_prelude(&rule.prelude)
        }
        Rule::At(rule)
            if equals_ascii_case_insensitive(rule.name.as_ref(), b"font-face")
                || is_margin_rule_name(rule.name.as_ref()) =>
        {
            if non_whitespace(&rule.prelude).is_empty() {
                ParsedRulePrelude::Empty
            } else {
                ParsedRulePrelude::Invalid
            }
        }
        Rule::At(_) => ParsedRulePrelude::Unparsed,
    }
}

struct Parser {
    tokens: Vec<ParserToken>,
    position: usize,
    rule_context: Vec<RuleContext>,
}

fn equals_ascii_case_insensitive(value: &[u16], expected: &[u8]) -> bool {
    value.len() == expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left).is_ok_and(|left| left.eq_ignore_ascii_case(&right)))
}

fn starts_with_ascii(value: &[u16], expected: &[u8]) -> bool {
    value.len() >= expected.len()
        && value
            .iter()
            .zip(expected)
            .all(|(&left, &right)| u8::try_from(left) == Ok(right))
}

fn is_margin_rule_name(name: &[u16]) -> bool {
    [
        b"top-left-corner".as_slice(),
        b"top-left",
        b"top-center",
        b"top-right",
        b"top-right-corner",
        b"bottom-left-corner",
        b"bottom-left",
        b"bottom-center",
        b"bottom-right",
        b"bottom-right-corner",
        b"left-top",
        b"left-middle",
        b"left-bottom",
        b"right-top",
        b"right-middle",
        b"right-bottom",
    ]
    .iter()
    .any(|expected| equals_ascii_case_insensitive(name, expected))
}

fn is_font_feature_value_rule_name(name: &[u16]) -> bool {
    [
        b"annotation".as_slice(),
        b"character-variant",
        b"ornaments",
        b"styleset",
        b"stylistic",
        b"swash",
    ]
    .iter()
    .any(|expected| equals_ascii_case_insensitive(name, expected))
}

fn context_for_at_rule(name: &[u16]) -> RuleContext {
    if equals_ascii_case_insensitive(name, b"container") {
        RuleContext::AtContainer
    } else if equals_ascii_case_insensitive(name, b"counter-style") {
        RuleContext::AtCounterStyle
    } else if equals_ascii_case_insensitive(name, b"font-face") {
        RuleContext::AtFontFace
    } else if equals_ascii_case_insensitive(name, b"font-feature-values") {
        RuleContext::AtFontFeatureValues
    } else if is_font_feature_value_rule_name(name) {
        RuleContext::FontFeatureValue
    } else if equals_ascii_case_insensitive(name, b"function") {
        RuleContext::AtFunction
    } else if equals_ascii_case_insensitive(name, b"keyframes")
        || equals_ascii_case_insensitive(name, b"-webkit-keyframes")
    {
        RuleContext::AtKeyframes
    } else if equals_ascii_case_insensitive(name, b"layer") {
        RuleContext::AtLayer
    } else if equals_ascii_case_insensitive(name, b"media") {
        RuleContext::AtMedia
    } else if equals_ascii_case_insensitive(name, b"page") {
        RuleContext::AtPage
    } else if equals_ascii_case_insensitive(name, b"property") {
        RuleContext::AtProperty
    } else if equals_ascii_case_insensitive(name, b"scope") {
        RuleContext::AtScope
    } else if equals_ascii_case_insensitive(name, b"supports") {
        RuleContext::AtSupports
    } else if is_margin_rule_name(name) {
        RuleContext::Margin
    } else {
        RuleContext::Unknown
    }
}

impl Parser {
    fn new(tokens: Vec<ParserToken>, rule_context: Vec<RuleContext>) -> Self {
        Self {
            tokens,
            position: 0,
            rule_context,
        }
    }

    fn from_source<'a>(
        source: impl Into<TokenizerInput<'a>>,
        rule_context: Vec<RuleContext>,
    ) -> (Self, Vec<FfiSyntaxDiagnostic>) {
        let tokens = tokenize_for_parser(source);
        let diagnostics = token_diagnostics(&tokens);
        (Self::new(tokens, rule_context), diagnostics)
    }

    fn next_kind(&self) -> Option<&ParserTokenKind> {
        self.tokens.get(self.position).map(|token| &token.kind)
    }

    fn peek_kind(&self, offset: usize) -> Option<&ParserTokenKind> {
        self.tokens.get(self.position + offset).map(|token| &token.kind)
    }

    fn discard_whitespace(&mut self) {
        while matches!(self.next_kind(), Some(ParserTokenKind::Whitespace)) {
            self.position += 1;
        }
    }

    fn consume_component_value(&mut self) -> Result<ComponentValue, ()> {
        consume_a_component_value(&mut self.tokens, &mut self.position, 0, true)
    }

    fn clone_component_value(&mut self) -> Result<ComponentValue, ()> {
        consume_a_component_value(&mut self.tokens, &mut self.position, 0, false)
    }

    fn declaration_can_take_tokens(&self, save_original_text: bool) -> bool {
        if save_original_text {
            return false;
        }
        let mut nesting_depth = 0usize;
        for token in &self.tokens[self.position..] {
            match token.kind {
                ParserTokenKind::Function(_) | ParserTokenKind::OpenSquare | ParserTokenKind::OpenParen => {
                    nesting_depth += 1;
                }
                ParserTokenKind::OpenCurly if nesting_depth == 0 => return false,
                ParserTokenKind::OpenCurly => nesting_depth += 1,
                ParserTokenKind::CloseSquare | ParserTokenKind::CloseParen | ParserTokenKind::CloseCurly
                    if nesting_depth > 0 =>
                {
                    nesting_depth -= 1;
                }
                ParserTokenKind::Semicolon | ParserTokenKind::CloseCurly if nesting_depth == 0 => break,
                _ => {}
            }
        }
        true
    }

    fn consume_component_values_until(
        &mut self,
        stop: Option<&ParserTokenKind>,
        nested: Nested,
        take_tokens: bool,
    ) -> Vec<ComponentValue> {
        let mut values = Vec::with_capacity(4);
        while let Some(kind) = self.next_kind() {
            if stop.is_some_and(|stop| std::mem::discriminant(kind) == std::mem::discriminant(stop))
                || nested == Nested::Yes && matches!(kind, ParserTokenKind::CloseCurly)
            {
                break;
            }
            let value = if take_tokens {
                self.consume_component_value()
            } else {
                self.clone_component_value()
            };
            let Ok(value) = value else {
                break;
            };
            values.push(value);
        }
        values
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-stylesheet-contents
    fn consume_stylesheet_contents(&mut self) -> Vec<Rule> {
        let mut rules = Vec::new();
        loop {
            match self.next_kind() {
                Some(ParserTokenKind::Whitespace | ParserTokenKind::Cdo | ParserTokenKind::Cdc) => self.position += 1,
                None => return rules,
                Some(ParserTokenKind::AtKeyword(_)) => {
                    if let Some(rule) = self.consume_at_rule(Nested::No) {
                        rules.push(Rule::At(rule));
                    }
                }
                _ => {
                    if let Ok(rule) = self.consume_qualified_rule(None, Nested::No) {
                        rules.push(Rule::Qualified(rule));
                    }
                }
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-at-rule
    fn consume_at_rule(&mut self, nested: Nested) -> Option<AtRule> {
        let ParserTokenKind::AtKeyword(name) = self.next_kind()?.clone() else {
            return None;
        };
        self.position += 1;
        let mut rule = AtRule {
            name,
            prelude: Vec::with_capacity(4),
            children: Vec::new(),
            has_block: false,
        };
        loop {
            match self.next_kind() {
                None | Some(ParserTokenKind::Semicolon) => {
                    self.position += usize::from(self.next_kind().is_some());
                    return self.at_rule_is_valid(&rule).then_some(rule);
                }
                Some(ParserTokenKind::CloseCurly) if nested == Nested::Yes => {
                    return self.at_rule_is_valid(&rule).then_some(rule);
                }
                Some(ParserTokenKind::OpenCurly) => {
                    let context = context_for_at_rule(rule.name.as_ref());
                    self.rule_context.push(context);
                    rule.children = self.consume_block();
                    self.rule_context.pop();
                    rule.has_block = true;
                    return self.at_rule_is_valid(&rule).then_some(rule);
                }
                _ => rule.prelude.push(self.consume_component_value().ok()?),
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-qualified-rule
    fn consume_qualified_rule(
        &mut self,
        stop: Option<&ParserTokenKind>,
        nested: Nested,
    ) -> Result<QualifiedRule, QualifiedRuleResult> {
        let qualified_context = if self.rule_context.last() == Some(&RuleContext::AtKeyframes) {
            RuleContext::Keyframe
        } else {
            RuleContext::Style
        };
        let mut rule = QualifiedRule {
            prelude: Vec::with_capacity(8),
            prelude_is_selector: qualified_context == RuleContext::Style,
            declarations: Vec::new(),
            children: Vec::new(),
            source_position: None,
        };
        loop {
            let Some(kind) = self.next_kind() else {
                return Err(QualifiedRuleResult::Invalid);
            };
            if stop.is_some_and(|stop| std::mem::discriminant(kind) == std::mem::discriminant(stop)) {
                return Err(QualifiedRuleResult::Invalid);
            }
            match kind {
                ParserTokenKind::CloseCurly if nested == Nested::Yes => return Err(QualifiedRuleResult::Invalid),
                ParserTokenKind::OpenCurly => {
                    let mut non_whitespace = rule.prelude.iter().filter(|value| !value.is_whitespace());
                    let looks_like_custom_property = non_whitespace
                        .next()
                        .and_then(ComponentValue::ident)
                        .is_some_and(|ident| starts_with_ascii(ident, b"--"))
                        && non_whitespace.next().is_some_and(ComponentValue::is_colon);
                    if looks_like_custom_property {
                        if nested == Nested::Yes {
                            self.consume_bad_declaration(nested);
                        } else {
                            self.consume_block();
                        }
                        return Err(QualifiedRuleResult::Invalid);
                    }
                    self.rule_context.push(qualified_context);
                    rule.children = self.consume_block();
                    self.rule_context.pop();
                    if matches!(rule.children.first(), Some(RuleOrDeclarations::Declarations(_))) {
                        let RuleOrDeclarations::Declarations(declarations) = rule.children.remove(0) else {
                            unreachable!();
                        };
                        rule.declarations = declarations;
                    }
                    return if self.qualified_rule_is_valid() {
                        Ok(rule)
                    } else {
                        Err(QualifiedRuleResult::InvalidInContext)
                    };
                }
                _ => {
                    let value = self
                        .consume_component_value()
                        .map_err(|()| QualifiedRuleResult::Invalid)?;
                    if rule.source_position.is_none() && !value.is_whitespace() {
                        rule.source_position = Some(value.start_position);
                    }
                    rule.prelude.push(value);
                }
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-block
    fn consume_block(&mut self) -> Vec<RuleOrDeclarations> {
        debug_assert!(matches!(self.next_kind(), Some(ParserTokenKind::OpenCurly)));
        self.position += 1;
        let result = self.consume_block_contents();
        self.position += usize::from(matches!(self.next_kind(), Some(ParserTokenKind::CloseCurly)));
        result
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-block-contents
    fn consume_block_contents(&mut self) -> Vec<RuleOrDeclarations> {
        let mut result = Vec::with_capacity(4);
        let mut declarations = Vec::with_capacity(8);
        loop {
            match self.next_kind() {
                Some(ParserTokenKind::Whitespace | ParserTokenKind::Semicolon) => self.position += 1,
                None | Some(ParserTokenKind::CloseCurly) => {
                    if !declarations.is_empty() {
                        result.push(RuleOrDeclarations::Declarations(declarations));
                    }
                    return result;
                }
                Some(ParserTokenKind::AtKeyword(_)) => {
                    if !declarations.is_empty() {
                        result.push(RuleOrDeclarations::Declarations(std::mem::take(&mut declarations)));
                    }
                    if let Some(rule) = self.consume_at_rule(Nested::Yes) {
                        result.push(RuleOrDeclarations::Rule(Rule::At(rule)));
                    }
                }
                _ => {
                    let could_be_declaration = matches!(self.next_kind(), Some(ParserTokenKind::Ident(_))) && {
                        let mut lookahead = 1;
                        while matches!(self.peek_kind(lookahead), Some(ParserTokenKind::Whitespace)) {
                            lookahead += 1;
                        }
                        matches!(self.peek_kind(lookahead), Some(ParserTokenKind::Colon))
                    };
                    let start = self.position;
                    if could_be_declaration && let Some(declaration) = self.consume_declaration(Nested::Yes, false) {
                        declarations.push(declaration);
                        continue;
                    }
                    self.position = start;
                    match self.consume_qualified_rule(Some(&ParserTokenKind::Semicolon), Nested::Yes) {
                        Ok(rule) => {
                            if !declarations.is_empty() {
                                result.push(RuleOrDeclarations::Declarations(std::mem::take(&mut declarations)));
                            }
                            result.push(RuleOrDeclarations::Rule(Rule::Qualified(rule)));
                        }
                        Err(QualifiedRuleResult::InvalidInContext) => {
                            if !declarations.is_empty() {
                                result.push(RuleOrDeclarations::Declarations(std::mem::take(&mut declarations)));
                            }
                        }
                        Err(QualifiedRuleResult::Invalid) => {
                            self.position += usize::from(matches!(self.next_kind(), Some(ParserTokenKind::Semicolon)));
                        }
                    }
                }
            }
        }
    }

    // https://drafts.csswg.org/css-syntax-3/#consume-declaration
    fn consume_declaration(&mut self, nested: Nested, save_original_text: bool) -> Option<Declaration> {
        let start = self.position;
        let token = self.tokens.get(self.position)?.clone();
        let ParserTokenKind::Ident(name) = token.kind else {
            self.consume_bad_declaration(nested);
            return None;
        };
        self.position += 1;
        self.discard_whitespace();
        if !matches!(self.next_kind(), Some(ParserTokenKind::Colon)) {
            self.consume_bad_declaration(nested);
            return None;
        }
        self.position += 1;
        self.discard_whitespace();
        let can_take_tokens = self.declaration_can_take_tokens(save_original_text);
        let mut value = self.consume_component_values_until(Some(&ParserTokenKind::Semicolon), nested, can_take_tokens);

        let mut important = false;
        if value.len() >= 2 {
            let mut important_index = None;
            for index in (1..value.len()).rev() {
                if value[index]
                    .ident()
                    .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"important"))
                {
                    important_index = Some(index);
                    break;
                }
                if !value[index].is_whitespace() {
                    break;
                }
            }
            if let Some(important_index) = important_index {
                let mut bang_index = None;
                for index in (1..important_index).rev() {
                    if value[index].is_delim(b'!') {
                        bang_index = Some(index);
                        break;
                    }
                    if !value[index].is_whitespace() {
                        break;
                    }
                }
                if let Some(bang_index) = bang_index {
                    value.remove(important_index);
                    value.remove(bang_index);
                    important = true;
                }
            }
        }
        while value.last().is_some_and(ComponentValue::is_whitespace) {
            value.pop();
        }

        let is_custom_property = starts_with_ascii(name.as_ref(), b"--");
        if is_custom_property && name.as_ref().len() == 2 {
            return None;
        }
        if !is_custom_property {
            let mut contains_curly = false;
            let mut contains_non_whitespace = false;
            for component in &value {
                if matches!(
                    component.kind,
                    ComponentKind::SimpleBlock {
                        opening: ParserTokenKind::OpenCurly,
                        ..
                    }
                ) {
                    if contains_non_whitespace {
                        return None;
                    }
                    contains_curly = true;
                } else if !component.is_whitespace() {
                    if contains_curly {
                        return None;
                    }
                    contains_non_whitespace = true;
                }
            }
        }
        let original_value_text = is_custom_property.then(|| {
            value
                .iter()
                .flat_map(|value| value.original_source_text.iter())
                .collect::<Vec<_>>()
                .into_boxed_slice()
        });
        if !self.declaration_is_valid(name.as_ref()) {
            return None;
        }
        let original_full_text = save_original_text.then(|| {
            self.tokens[start..self.position]
                .iter()
                .flat_map(|token| token.source.iter())
                .collect::<Vec<_>>()
                .into_boxed_slice()
        });
        let is_property = self.declaration_is_property();
        let rule_context = if !is_property && self.rule_context.contains(&RuleContext::AtFunction) {
            RuleContext::AtFunction
        } else {
            self.rule_context.last().copied().unwrap_or(RuleContext::Unknown)
        };
        Some(Declaration {
            name,
            value,
            important,
            original_value_text,
            original_full_text,
            source_position: token.start_position,
            is_property,
            rule_context,
        })
    }

    fn consume_bad_declaration(&mut self, nested: Nested) {
        loop {
            match self.next_kind() {
                None | Some(ParserTokenKind::Semicolon) => {
                    self.position += usize::from(self.next_kind().is_some());
                    return;
                }
                Some(ParserTokenKind::CloseCurly) if nested == Nested::Yes => return,
                _ => {
                    if self.consume_component_value().is_err() {
                        return;
                    }
                }
            }
        }
    }

    fn declaration_is_valid(&self, name: &[u16]) -> bool {
        let Some(context) = self.rule_context.last() else {
            return false;
        };
        match context {
            RuleContext::Unknown | RuleContext::AtKeyframes => false,
            RuleContext::Keyframe => ![
                b"animation".as_slice(),
                b"animation-delay",
                b"animation-direction",
                b"animation-duration",
                b"animation-fill-mode",
                b"animation-iteration-count",
                b"animation-name",
                b"animation-play-state",
                b"animation-timeline",
            ]
            .iter()
            .any(|property| equals_ascii_case_insensitive(name, property)),
            RuleContext::AtContainer | RuleContext::AtLayer | RuleContext::AtMedia | RuleContext::AtSupports => self
                .rule_context
                .iter()
                .any(|context| matches!(context, RuleContext::Style | RuleContext::AtFunction)),
            _ => true,
        }
    }

    fn declaration_is_property(&self) -> bool {
        match self.rule_context.last() {
            Some(RuleContext::Style | RuleContext::Keyframe | RuleContext::AtScope) => true,
            Some(RuleContext::AtContainer | RuleContext::AtLayer | RuleContext::AtMedia | RuleContext::AtSupports) => {
                self.rule_context.contains(&RuleContext::Style)
            }
            _ => false,
        }
    }

    fn at_rule_is_valid(&self, rule: &AtRule) -> bool {
        let name = rule.name.as_ref();
        if self.rule_context.is_empty() {
            return !is_margin_rule_name(name);
        }
        if self.rule_context.contains(&RuleContext::Style) {
            return [b"container".as_slice(), b"layer", b"media", b"scope", b"supports"]
                .iter()
                .any(|expected| equals_ascii_case_insensitive(name, expected));
        }
        if self.rule_context.contains(&RuleContext::AtFunction) {
            return [b"container".as_slice(), b"media", b"supports"]
                .iter()
                .any(|expected| equals_ascii_case_insensitive(name, expected));
        }
        match self.rule_context.last().unwrap() {
            RuleContext::AtContainer
            | RuleContext::AtLayer
            | RuleContext::AtMedia
            | RuleContext::AtScope
            | RuleContext::AtSupports => {
                !equals_ascii_case_insensitive(name, b"import") && !equals_ascii_case_insensitive(name, b"namespace")
            }
            RuleContext::AtPage => is_margin_rule_name(name),
            RuleContext::AtFontFeatureValues => is_font_feature_value_rule_name(name),
            _ => false,
        }
    }

    fn qualified_rule_is_valid(&self) -> bool {
        matches!(
            self.rule_context.last(),
            None | Some(
                RuleContext::Style
                    | RuleContext::AtContainer
                    | RuleContext::AtLayer
                    | RuleContext::AtMedia
                    | RuleContext::AtScope
                    | RuleContext::AtSupports
                    | RuleContext::AtKeyframes
            )
        )
    }
}

#[cfg(test)]
pub(crate) fn parse_stylesheet<'a>(source: impl Into<TokenizerInput<'a>>) -> Vec<Rule> {
    Parser::from_source(source, Vec::new()).0.consume_stylesheet_contents()
}

fn parse_rule_from_tokens(tokens: Vec<ParserToken>, rule_context: Vec<RuleContext>, nested: bool) -> Option<Rule> {
    let mut parser = Parser::new(tokens, rule_context);
    let nested = if nested { Nested::Yes } else { Nested::No };
    parser.discard_whitespace();
    let rule = match parser.next_kind() {
        None => return None,
        Some(ParserTokenKind::AtKeyword(_)) => Rule::At(parser.consume_at_rule(nested)?),
        _ => Rule::Qualified(parser.consume_qualified_rule(None, nested).ok()?),
    };
    parser.discard_whitespace();
    parser.next_kind().is_none().then_some(rule)
}

#[cfg(test)]
pub(crate) fn parse_rule<'a>(
    source: impl Into<TokenizerInput<'a>>,
    rule_context: Vec<RuleContext>,
    nested: bool,
) -> Option<Rule> {
    let (parser, _) = Parser::from_source(source, rule_context);
    parse_rule_from_tokens(parser.tokens, parser.rule_context, nested)
}

#[cfg(test)]
pub(crate) fn parse_block_contents<'a>(
    source: impl Into<TokenizerInput<'a>>,
    rule_context: Vec<RuleContext>,
) -> Vec<RuleOrDeclarations> {
    Parser::from_source(source, rule_context).0.consume_block_contents()
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxComponent {
    pub component_type: u8,
    pub token_type: u8,
    pub hash_type: u8,
    pub number_type: u8,
    pub number_value: f64,
    pub delim: u32,
    pub value_offset: usize,
    pub value_length: usize,
    pub source_offset: usize,
    pub source_length: usize,
    pub end_source_offset: usize,
    pub end_source_length: usize,
    pub children_start: usize,
    pub child_count: usize,
    pub start_line: usize,
    pub start_column: usize,
    pub end_line: usize,
    pub end_column: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxDeclaration {
    pub name_offset: usize,
    pub name_length: usize,
    pub values_start: usize,
    pub value_count: usize,
    pub value_source_offset: usize,
    pub value_source_length: usize,
    pub is_property: bool,
    pub important: bool,
    pub original_value_offset: usize,
    pub original_value_length: usize,
    pub original_full_text_offset: usize,
    pub original_full_text_length: usize,
    pub start_line: usize,
    pub start_column: usize,
    pub property_id: u16,
    pub parse_status: FfiParseStatus,
    pub parsed_value: *const c_void,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxRule {
    pub rule_type: u8,
    pub name_offset: usize,
    pub name_length: usize,
    pub prelude_start: usize,
    pub prelude_count: usize,
    pub prelude_source_offset: usize,
    pub prelude_source_length: usize,
    pub declarations_start: usize,
    pub declaration_count: usize,
    pub children_start: usize,
    pub child_count: usize,
    pub has_block: bool,
    pub has_source_position: bool,
    pub start_line: usize,
    pub start_column: usize,
    pub parsed_prelude_kind: u8,
    pub parsed_prelude_name_offset: usize,
    pub parsed_prelude_name_length: usize,
    pub parsed_prelude_secondary_offset: usize,
    pub parsed_prelude_secondary_length: usize,
    pub parsed_prelude_items_start: usize,
    pub parsed_prelude_item_count: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxPreludeItem {
    pub value_offset: usize,
    pub value_length: usize,
    pub number_value: f64,
    pub flags: u8,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxItem {
    pub item_type: u8,
    pub start: usize,
    pub count: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxDiagnostic {
    pub code: u8,
    pub start_line: usize,
    pub start_column: usize,
    pub end_line: usize,
    pub end_column: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxParseData {
    pub values: *const u16,
    pub value_count: usize,
    pub components: *const FfiSyntaxComponent,
    pub component_count: usize,
    pub component_indices: *const usize,
    pub component_index_count: usize,
    pub declarations: *const FfiSyntaxDeclaration,
    pub declaration_count: usize,
    pub rules: *const FfiSyntaxRule,
    pub rule_count: usize,
    pub items: *const FfiSyntaxItem,
    pub item_count: usize,
    pub item_indices: *const usize,
    pub item_index_count: usize,
    pub roots: *const usize,
    pub root_count: usize,
    pub prelude_items: *const FfiSyntaxPreludeItem,
    pub prelude_item_count: usize,
    pub diagnostics: *const FfiSyntaxDiagnostic,
    pub diagnostic_count: usize,
}

pub struct FfiSyntaxParse {
    values: Vec<u16>,
    components: Vec<FfiSyntaxComponent>,
    component_indices: Vec<usize>,
    declarations: Vec<FfiSyntaxDeclaration>,
    rules: Vec<FfiSyntaxRule>,
    items: Vec<FfiSyntaxItem>,
    item_indices: Vec<usize>,
    roots: Vec<usize>,
    prelude_items: Vec<FfiSyntaxPreludeItem>,
    diagnostics: Vec<FfiSyntaxDiagnostic>,
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
    preserve_property_source_text: bool,
}

fn component_list_source(values: &[ComponentValue]) -> Cow<'_, [u16]> {
    if values.is_empty() {
        return Cow::Borrowed(&[]);
    }
    Cow::Owned(
        values
            .iter()
            .flat_map(|value| value.original_source_text.iter())
            .collect(),
    )
}

impl FfiSyntaxParse {
    pub(crate) fn new(
        parse_context: *const ParseContext,
        resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
        preserve_property_source_text: bool,
    ) -> Self {
        Self {
            values: Vec::new(),
            components: Vec::new(),
            component_indices: Vec::new(),
            declarations: Vec::new(),
            rules: Vec::new(),
            items: Vec::new(),
            item_indices: Vec::new(),
            roots: Vec::new(),
            prelude_items: Vec::new(),
            diagnostics: Vec::new(),
            parse_context,
            resolve_property_id,
            preserve_property_source_text,
        }
    }

    pub(crate) fn append_value(&mut self, value: &[u16]) -> (usize, usize) {
        let offset = self.values.len();
        self.values.extend_from_slice(value);
        (offset, value.len())
    }

    fn append_optional_value(&mut self, value: Option<&[u16]>) -> (usize, usize) {
        value.map_or((usize::MAX, 0), |value| self.append_value(value))
    }

    fn append_source(&mut self, source: &crate::css::css_tokenizer::ParserSource) -> (usize, usize) {
        let offset = self.values.len();
        source.append_to(&mut self.values);
        (offset, source.len())
    }

    pub(crate) fn append_component_list(&mut self, values: &[ComponentValue]) -> (usize, usize) {
        let indices = values
            .iter()
            .map(|value| self.append_component(value))
            .collect::<Vec<_>>();
        let start = self.component_indices.len();
        self.component_indices.extend(indices);
        (start, values.len())
    }

    fn append_component(&mut self, component: &ComponentValue) -> usize {
        let (component_type, token_type, hash_type, number_type, number_value, delim, payload, children) =
            match &component.kind {
                ComponentKind::Token(kind) => match kind {
                    ParserTokenKind::EndOfFile => (0, 1, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Ident(value) => (0, 2, 0, 0, 0.0, 0, value.as_ref(), &[][..]),
                    ParserTokenKind::Function(value) => (0, 3, 0, 0, 0.0, 0, value.as_ref(), &[][..]),
                    ParserTokenKind::AtKeyword(value) => (0, 4, 0, 0, 0.0, 0, value.as_ref(), &[][..]),
                    ParserTokenKind::Hash { value, is_id } => {
                        (0, 5, u8::from(!is_id), 0, 0.0, 0, value.as_ref(), &[][..])
                    }
                    ParserTokenKind::String(value) => (0, 6, 0, 0, 0.0, 0, value.as_ref(), &[][..]),
                    ParserTokenKind::BadString => (0, 7, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Url(value) => (0, 8, 0, 0, 0.0, 0, value.as_ref(), &[][..]),
                    ParserTokenKind::BadUrl => (0, 9, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Delim(value) => (0, 10, 0, 0, 0.0, *value, &[][..], &[][..]),
                    ParserTokenKind::Number { value, number_type } => {
                        (0, 11, 0, *number_type as u8, *value, 0, &[][..], &[][..])
                    }
                    ParserTokenKind::Percentage { value, number_type } => {
                        (0, 12, 0, *number_type as u8, *value, 0, &[][..], &[][..])
                    }
                    ParserTokenKind::Dimension {
                        value,
                        number_type,
                        unit,
                    } => (0, 13, 0, *number_type as u8, *value, 0, unit.as_ref(), &[][..]),
                    ParserTokenKind::Whitespace => (0, 14, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Cdo => (0, 15, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Cdc => (0, 16, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Colon => (0, 17, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Semicolon => (0, 18, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::Comma => (0, 19, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::OpenSquare => (0, 20, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::CloseSquare => (0, 21, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::OpenParen => (0, 22, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::CloseParen => (0, 23, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::OpenCurly => (0, 24, 0, 0, 0.0, 0, &[][..], &[][..]),
                    ParserTokenKind::CloseCurly => (0, 25, 0, 0, 0.0, 0, &[][..], &[][..]),
                },
                ComponentKind::Function { name, values } => (1, 3, 0, 0, 0.0, 0, name.as_ref(), values.as_ref()),
                ComponentKind::SimpleBlock { opening, values } => {
                    let token_type = match opening {
                        ParserTokenKind::OpenSquare => 20,
                        ParserTokenKind::OpenParen => 22,
                        ParserTokenKind::OpenCurly => 24,
                        _ => unreachable!(),
                    };
                    (2, token_type, 0, 0, 0.0, 0, &[][..], values.as_ref())
                }
            };
        let (children_start, child_count) = self.append_component_list(children);
        let (value_offset, value_length) = self.append_value(payload);
        let full_source_length = component.original_source_text.len();
        let (full_source_offset, _) = self.append_source(&component.original_source_text);
        let source_offset = full_source_offset;
        let source_length = component.opening_source_length;
        let end_source_offset = full_source_offset + full_source_length - component.closing_source_length;
        let end_source_length = component.closing_source_length;
        let index = self.components.len();
        self.components.push(FfiSyntaxComponent {
            component_type,
            token_type,
            hash_type,
            number_type,
            number_value,
            delim,
            value_offset,
            value_length,
            source_offset,
            source_length,
            end_source_offset,
            end_source_length,
            children_start,
            child_count,
            start_line: component.start_position.line,
            start_column: component.start_position.column,
            end_line: component.end_position.line,
            end_column: component.end_position.column,
        });
        index
    }

    fn append_declaration(&mut self, declaration: &Declaration) -> usize {
        let (name_offset, name_length) = self.append_value(declaration.name.as_ref());
        let value_source = component_list_source(&declaration.value);
        let (values_start, value_count) = if declaration.is_property {
            (0, 0)
        } else {
            self.append_component_list(&declaration.value)
        };
        let (original_value_offset, original_value_length) =
            self.append_optional_value(declaration.original_value_text.as_deref());
        let (original_full_text_offset, original_full_text_length) =
            self.append_optional_value(declaration.original_full_text.as_deref());
        let (property_id, parse_status, parsed_value) =
            self.parse_declaration_value(declaration, value_source.as_ref());
        let needs_value_text = self.preserve_property_source_text
            || parse_status != FfiParseStatus::Parsed
            || equals_ascii_case_insensitive(declaration.name.as_ref(), b"-webkit-box-orient");
        let (value_source_offset, value_source_length) = if needs_value_text {
            self.append_value(value_source.as_ref())
        } else {
            (0, 0)
        };
        let index = self.declarations.len();
        self.declarations.push(FfiSyntaxDeclaration {
            name_offset,
            name_length,
            values_start,
            value_count,
            value_source_offset,
            value_source_length,
            is_property: declaration.is_property,
            important: declaration.important,
            original_value_offset,
            original_value_length,
            original_full_text_offset,
            original_full_text_length,
            start_line: declaration.source_position.line,
            start_column: declaration.source_position.column,
            property_id,
            parse_status,
            parsed_value,
        });
        index
    }

    fn parse_declaration_value(
        &self,
        declaration: &Declaration,
        source_utf16: &[u16],
    ) -> (u16, FfiParseStatus, *const c_void) {
        if self.parse_context.is_null() {
            return (u16::MAX, FfiParseStatus::NotHandled, std::ptr::null());
        }
        if !declaration.is_property {
            let at_rule = match declaration.rule_context {
                RuleContext::AtFontFace => 0,
                RuleContext::AtPage => 1,
                RuleContext::AtProperty => 2,
                RuleContext::AtCounterStyle => 3,
                RuleContext::AtFunction => 4,
                _ => return (u16::MAX, FfiParseStatus::NotHandled, std::ptr::null()),
            };
            let parse_context = unsafe { &*self.parse_context };
            return match parse_descriptor(
                parse_context,
                at_rule,
                declaration.name.as_ref(),
                &declaration.value,
                source_utf16,
            ) {
                Some(descriptor) => (
                    u16::MAX,
                    FfiParseStatus::Parsed,
                    Arc::into_raw(descriptor.value).cast::<c_void>(),
                ),
                None => (u16::MAX, FfiParseStatus::Invalid, std::ptr::null()),
            };
        }
        let Some(resolve_property_id) = self.resolve_property_id else {
            return (u16::MAX, FfiParseStatus::NotHandled, std::ptr::null());
        };
        // SAFETY: The declaration name remains live for this callback and the caller owns the callback.
        let property_id = unsafe { resolve_property_id(declaration.name.as_ptr(), declaration.name.len()) };
        if property_id == u16::MAX {
            return (property_id, FfiParseStatus::NotHandled, std::ptr::null());
        }

        let parse_context = unsafe { &*self.parse_context };
        let mut value_contexts = if parse_context.value_context_count == 0 {
            Vec::new()
        } else {
            // SAFETY: C++ keeps the context storage live for the complete syntax parse call.
            unsafe { std::slice::from_raw_parts(parse_context.value_contexts, parse_context.value_context_count) }
                .to_vec()
        };
        value_contexts.push(FfiValueParsingContext {
            kind: FfiValueParsingContextKind::Property,
            value: property_id,
            secondary_value: 0,
            name: Default::default(),
        });
        let mut context = *parse_context;
        context.value_contexts = value_contexts.as_ptr();
        context.value_context_count = value_contexts.len();

        let outcome = parse_css_value_with_utf16_source(&context, property_id, &declaration.value, source_utf16);
        if let Some(random_function_index) = unsafe { parse_context.random_function_index.as_mut() } {
            *random_function_index = 0;
        }
        match outcome {
            ParseOutcome::Parsed(value) => (
                property_id,
                FfiParseStatus::Parsed,
                Arc::into_raw(value).cast::<c_void>(),
            ),
            ParseOutcome::Invalid => (property_id, FfiParseStatus::Invalid, std::ptr::null()),
            ParseOutcome::NotHandled => (property_id, FfiParseStatus::NotHandled, std::ptr::null()),
        }
    }

    fn append_declarations(&mut self, declarations: &[Declaration]) -> (usize, usize) {
        let start = self.declarations.len();
        for declaration in declarations {
            self.append_declaration(declaration);
        }
        (start, declarations.len())
    }

    fn append_items(&mut self, items: &[RuleOrDeclarations]) -> (usize, usize) {
        let indices = items.iter().map(|item| self.append_item(item)).collect::<Vec<_>>();
        let start = self.item_indices.len();
        self.item_indices.extend(indices);
        (start, items.len())
    }

    fn append_item(&mut self, item: &RuleOrDeclarations) -> usize {
        let (item_type, start, count) = match item {
            RuleOrDeclarations::Rule(rule) => (0, self.append_rule(rule), 1),
            RuleOrDeclarations::Declarations(declarations) => {
                let (start, count) = self.append_declarations(declarations);
                (1, start, count)
            }
        };
        let index = self.items.len();
        self.items.push(FfiSyntaxItem {
            item_type,
            start,
            count,
        });
        index
    }

    fn append_rule(&mut self, rule: &Rule) -> usize {
        let (rule_type, name, prelude, prelude_is_selector, declarations, children, has_block, source_position) =
            match rule {
                Rule::At(rule) => (
                    0,
                    rule.name.as_ref(),
                    rule.prelude.as_slice(),
                    false,
                    &[][..],
                    rule.children.as_slice(),
                    rule.has_block,
                    None,
                ),
                Rule::Qualified(rule) => (
                    1,
                    &[][..],
                    rule.prelude.as_slice(),
                    rule.prelude_is_selector,
                    rule.declarations.as_slice(),
                    rule.children.as_slice(),
                    true,
                    rule.source_position,
                ),
            };
        let (name_offset, name_length) = self.append_value(name);
        let prelude_source = component_list_source(prelude);
        let (prelude_source_offset, prelude_source_length) = self.append_value(prelude_source.as_ref());
        let (prelude_start, prelude_count) = if prelude_is_selector {
            (0, 0)
        } else {
            self.append_component_list(prelude)
        };
        let (declarations_start, declaration_count) = self.append_declarations(declarations);
        let (children_start, child_count) = self.append_items(children);
        let parsed_prelude = parse_rule_prelude(rule);
        let mut parsed_prelude_name = None;
        let mut parsed_prelude_secondary = None;
        let mut parsed_items = Vec::new();
        let parsed_prelude_kind = match parsed_prelude {
            ParsedRulePrelude::Unparsed => 0,
            ParsedRulePrelude::Invalid => 1,
            ParsedRulePrelude::Empty => 2,
            ParsedRulePrelude::Name(name) => {
                parsed_prelude_name = Some(name);
                3
            }
            ParsedRulePrelude::Names(names) => {
                parsed_items.extend(names.into_iter().map(|name| (Some(name), 0.0, 0)));
                4
            }
            ParsedRulePrelude::KeyframeSelectors(selectors) => {
                parsed_items.extend(selectors.into_iter().map(|value| (None, value, 0)));
                5
            }
            ParsedRulePrelude::Namespace { prefix, uri } => {
                parsed_prelude_name = prefix;
                parsed_prelude_secondary = Some(uri);
                6
            }
            ParsedRulePrelude::PageSelectors(selectors) => {
                for selector in selectors {
                    parsed_items.push((selector.name, 0.0, 0x80));
                    parsed_items.extend(
                        selector
                            .pseudo_classes
                            .into_iter()
                            .map(|pseudo_class| (None, 0.0, pseudo_class)),
                    );
                }
                7
            }
            ParsedRulePrelude::FontFamilyNames(names) => {
                parsed_items.extend(names.into_iter().map(|name| (Some(name), 0.0, 0)));
                8
            }
            ParsedRulePrelude::Scope { start, end } => {
                if let Some(start) = start {
                    parsed_items.push((Some(start), 0.0, 0));
                }
                if let Some(end) = end {
                    parsed_items.push((Some(end), 0.0, 1));
                }
                9
            }
            ParsedRulePrelude::Import(items) => {
                parsed_items.extend(items.into_iter().map(|(value, flags)| (value, 0.0, flags)));
                10
            }
            ParsedRulePrelude::Function {
                name,
                parameters,
                return_type,
            } => {
                parsed_prelude_name = Some(name);
                parsed_prelude_secondary = return_type;
                for (name, type_source, default_source) in parameters {
                    parsed_items.push((Some(name), 0.0, 0));
                    if let Some(type_source) = type_source {
                        parsed_items.push((Some(type_source), 0.0, 1));
                    }
                    if let Some(default_source) = default_source {
                        parsed_items.push((Some(default_source), 0.0, 2));
                    }
                }
                11
            }
        };
        let (parsed_prelude_name_offset, parsed_prelude_name_length) =
            self.append_optional_value(parsed_prelude_name.as_deref());
        let (parsed_prelude_secondary_offset, parsed_prelude_secondary_length) =
            self.append_optional_value(parsed_prelude_secondary.as_deref());
        let parsed_prelude_items_start = self.prelude_items.len();
        for (value, number_value, flags) in parsed_items {
            let (value_offset, value_length) = self.append_optional_value(value.as_deref());
            self.prelude_items.push(FfiSyntaxPreludeItem {
                value_offset,
                value_length,
                number_value,
                flags,
            });
        }
        let parsed_prelude_item_count = self.prelude_items.len() - parsed_prelude_items_start;
        let index = self.rules.len();
        self.rules.push(FfiSyntaxRule {
            rule_type,
            name_offset,
            name_length,
            prelude_start,
            prelude_count,
            prelude_source_offset,
            prelude_source_length,
            declarations_start,
            declaration_count,
            children_start,
            child_count,
            has_block,
            has_source_position: source_position.is_some(),
            start_line: source_position.map_or(0, |position| position.line),
            start_column: source_position.map_or(0, |position| position.column),
            parsed_prelude_kind,
            parsed_prelude_name_offset,
            parsed_prelude_name_length,
            parsed_prelude_secondary_offset,
            parsed_prelude_secondary_length,
            parsed_prelude_items_start,
            parsed_prelude_item_count,
        });
        index
    }

    fn append_roots(&mut self, rules: &[Rule]) {
        self.roots = rules.iter().map(|rule| self.append_rule(rule)).collect();
    }

    fn append_root_items(&mut self, items: &[RuleOrDeclarations]) {
        self.roots = items.iter().map(|item| self.append_item(item)).collect();
    }

    pub(crate) fn data(&self) -> FfiSyntaxParseData {
        FfiSyntaxParseData {
            values: self.values.as_ptr(),
            value_count: self.values.len(),
            components: self.components.as_ptr(),
            component_count: self.components.len(),
            component_indices: self.component_indices.as_ptr(),
            component_index_count: self.component_indices.len(),
            declarations: self.declarations.as_ptr(),
            declaration_count: self.declarations.len(),
            rules: self.rules.as_ptr(),
            rule_count: self.rules.len(),
            items: self.items.as_ptr(),
            item_count: self.items.len(),
            item_indices: self.item_indices.as_ptr(),
            item_index_count: self.item_indices.len(),
            roots: self.roots.as_ptr(),
            root_count: self.roots.len(),
            prelude_items: self.prelude_items.as_ptr(),
            prelude_item_count: self.prelude_items.len(),
            diagnostics: self.diagnostics.as_ptr(),
            diagnostic_count: self.diagnostics.len(),
        }
    }
}

fn token_diagnostics(tokens: &[ParserToken]) -> Vec<FfiSyntaxDiagnostic> {
    tokens
        .iter()
        .filter_map(|token| {
            let code = match &token.kind {
                ParserTokenKind::BadString => 0,
                ParserTokenKind::BadUrl => 1,
                _ => return None,
            };
            Some(FfiSyntaxDiagnostic {
                code,
                start_line: token.start_position.line,
                start_column: token.start_position.column,
                end_line: token.end_position.line,
                end_column: token.end_position.column,
            })
        })
        .collect()
}

/// Parses stylesheet structure into a Rust-owned arena.
///
/// # Safety
/// `source` must remain readable for `source_length` bytes during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_stylesheet_syntax(
    source: FfiUtf16View,
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
) -> *mut FfiSyntaxParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let (mut parser, diagnostics) = Parser::from_source(source, Vec::new());
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, false);
        parse.diagnostics = diagnostics;
        let rules = parser.consume_stylesheet_contents();
        parse.append_roots(&rules);
        Box::into_raw(Box::new(parse))
    })
}

/// Parses exactly one CSS rule into a Rust-owned arena.
///
/// # Safety
/// `source` must remain readable for `source_length` bytes during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_rule_syntax(
    source: FfiUtf16View,
    contexts: *const u8,
    context_count: usize,
    nested: bool,
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
) -> *mut FfiSyntaxParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(contexts) = (unsafe { crate::bytes_from_raw(contexts, context_count) }) else {
            return std::ptr::null_mut();
        };
        let Some(contexts) = contexts
            .iter()
            .copied()
            .map(|context| {
                if context > RuleContext::Margin as u8 {
                    return None;
                }
                // SAFETY: RuleContext has contiguous repr(u8) discriminants and the value was range-checked.
                Some(unsafe { std::mem::transmute::<u8, RuleContext>(context) })
            })
            .collect::<Option<Vec<_>>>()
        else {
            return std::ptr::null_mut();
        };
        let (parser, diagnostics) = Parser::from_source(source, contexts);
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, false);
        parse.diagnostics = diagnostics;
        let rule = parse_rule_from_tokens(parser.tokens, parser.rule_context, nested);
        if let Some(rule) = rule {
            parse.append_roots(std::slice::from_ref(&rule));
        }
        Box::into_raw(Box::new(parse))
    })
}

/// Parses a keyframe selector list into a Rust-owned syntax arena.
///
/// # Safety
/// `source` must remain readable for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_keyframe_selectors_syntax(
    source: FfiUtf16View,
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
) -> *mut FfiSyntaxParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let tokens = tokenize_for_parser(source);
        let diagnostics = token_diagnostics(&tokens);
        let prelude = consume_a_list_of_component_values(tokens).unwrap_or_default();
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, false);
        parse.diagnostics = diagnostics;
        parse.append_roots(&[Rule::Qualified(QualifiedRule {
            prelude,
            prelude_is_selector: false,
            declarations: Vec::new(),
            children: Vec::new(),
            source_position: None,
        })]);
        Box::into_raw(Box::new(parse))
    })
}

/// Parses block contents into a Rust-owned arena.
///
/// # Safety
/// All pointers must remain readable for their accompanying lengths during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_block_syntax(
    source: FfiUtf16View,
    contexts: *const u8,
    context_count: usize,
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
    preserve_property_source_text: bool,
) -> *mut FfiSyntaxParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let Some(contexts) = (unsafe { crate::bytes_from_raw(contexts, context_count) }) else {
            return std::ptr::null_mut();
        };
        let Some(contexts) = contexts
            .iter()
            .copied()
            .map(|context| {
                if context > RuleContext::Margin as u8 {
                    return None;
                }
                // SAFETY: RuleContext has contiguous repr(u8) discriminants and the value was range-checked.
                Some(unsafe { std::mem::transmute::<u8, RuleContext>(context) })
            })
            .collect::<Option<Vec<_>>>()
        else {
            return std::ptr::null_mut();
        };
        let (mut parser, diagnostics) = Parser::from_source(source, contexts);
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, preserve_property_source_text);
        parse.diagnostics = diagnostics;
        let items = parser.consume_block_contents();
        parse.append_root_items(&items);
        Box::into_raw(Box::new(parse))
    })
}

/// Returns borrowed arena slices which remain live until `rust_css_syntax_parse_free`.
///
/// # Safety
/// `parse` must be a live handle returned by a Rust syntax parse function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_syntax_parse_data(parse: *const FfiSyntaxParse) -> FfiSyntaxParseData {
    crate::abort_on_panic(|| unsafe { &*parse }.data())
}

/// # Safety
/// `parse` must be null or a live syntax parse handle and must only be freed once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_syntax_parse_free(parse: *mut FfiSyntaxParse) {
    crate::abort_on_panic(|| {
        if !parse.is_null() {
            drop(unsafe { Box::from_raw(parse) });
        }
    });
}

#[cfg(test)]
mod tests {
    use super::{
        Declaration, FfiSyntaxParse, ParsedRulePrelude, Rule, RuleContext, RuleOrDeclarations,
        consume_a_list_of_component_values, parse_block_contents, parse_keyframe_selectors, parse_rule,
        parse_rule_prelude, parse_stylesheet, token_diagnostics, tokenize_for_parser,
    };

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    #[test]
    fn parses_exactly_one_rule() {
        assert!(parse_rule(b" a { color: red } ", Vec::new(), false).is_some());
        assert!(parse_rule(b"@media {} trailing", Vec::new(), false).is_none());
        assert!(parse_rule(b"a {} b {}", Vec::new(), false).is_none());
        assert!(parse_rule(b"   ", Vec::new(), false).is_none());
        assert!(parse_rule(b"@media print {} foo", vec![RuleContext::AtMedia], true).is_none());
    }

    #[test]
    fn exposes_token_diagnostics_with_source_spans() {
        let mut parse = FfiSyntaxParse::new(std::ptr::null(), None, false);
        let tokens =
            crate::css::css_tokenizer::tokenize_for_parser(&utf16("a { color: \"bad\n; background: url(foo\"bar) }"));
        parse.diagnostics = token_diagnostics(&tokens);
        assert_eq!(parse.diagnostics.len(), 2);
        assert_eq!(parse.diagnostics[0].code, 0);
        assert_eq!(parse.diagnostics[0].start_line, 0);
        assert!(parse.diagnostics[0].end_column > parse.diagnostics[0].start_column);
        assert_eq!(parse.diagnostics[1].code, 1);
        assert!(parse.diagnostics[1].end_column > parse.diagnostics[1].start_column);
    }

    fn declarations(input: &str) -> Vec<Declaration> {
        parse_block_contents(input.as_bytes(), vec![RuleContext::Style])
            .into_iter()
            .flat_map(|item| match item {
                RuleOrDeclarations::Declarations(declarations) => declarations,
                RuleOrDeclarations::Rule(_) => Vec::new(),
            })
            .collect()
    }

    #[test]
    fn consumes_stylesheet_rules_and_nested_blocks() {
        let rules = parse_stylesheet(b"<!-- @media screen { a { color: red } } b { width: 1px } -->");
        assert_eq!(rules.len(), 2);
        let Rule::At(media) = &rules[0] else {
            panic!("expected @media");
        };
        assert_eq!(media.name.as_ref(), utf16("media"));
        assert!(media.has_block);
        assert_eq!(media.children.len(), 1);
        let Rule::Qualified(style) = &rules[1] else {
            panic!("expected style rule");
        };
        assert_eq!(style.declarations.len(), 1);
    }

    #[test]
    fn consumes_declarations_and_important() {
        let declarations = declarations("color: red ! important; width: 1px!important; --x: var(--y) !important");
        assert_eq!(declarations.len(), 3);
        assert!(declarations[0].important);
        assert!(declarations[1].important);
        assert!(declarations[2].important);
        assert_eq!(
            declarations[2].original_value_text.as_deref(),
            Some(utf16("var(--y)").as_slice())
        );
    }

    #[test]
    fn preserves_declaration_and_rule_positions() {
        let rules = parse_stylesheet(b"\n\na {\n color: red;\n}");
        let Rule::Qualified(rule) = &rules[0] else {
            panic!("expected qualified rule");
        };
        assert_eq!(
            (rule.source_position.unwrap().line, rule.source_position.unwrap().column),
            (2, 0)
        );
        assert_eq!(
            (
                rule.declarations[0].source_position.line,
                rule.declarations[0].source_position.column
            ),
            (3, 1)
        );
    }

    #[test]
    fn rejects_mixed_curly_block_values_and_invalid_contexts() {
        let declarations = declarations("color: {} red; --x: {} red; width: 1px");
        assert_eq!(declarations.len(), 2);
        assert_eq!(declarations[0].name.as_ref(), utf16("--x"));
        assert_eq!(declarations[1].name.as_ref(), utf16("width"));

        assert!(parse_block_contents(b"color: red", Vec::new()).is_empty());
        assert!(parse_stylesheet(b"@top-left { color: red }").is_empty());
    }

    #[test]
    fn preserves_tokens_when_a_nested_rule_looks_like_a_declaration() {
        let rules = parse_stylesheet(b"a { color: red; b:hover { width: 1px } }");
        let Rule::Qualified(rule) = &rules[0] else {
            panic!("expected qualified rule");
        };
        assert_eq!(rule.declarations.len(), 1);
        assert!(matches!(
            rule.children.first(),
            Some(RuleOrDeclarations::Rule(Rule::Qualified(_)))
        ));
    }

    #[test]
    fn transfers_selector_preludes_verbatim() {
        let rules = parse_stylesheet(b":heading(1.0) {}");
        let mut parse = super::FfiSyntaxParse::new(std::ptr::null(), None, false);
        parse.append_roots(&rules);
        let rule = parse.rules[parse.roots[0]];
        assert_eq!(
            &parse.values[rule.prelude_source_offset..rule.prelude_source_offset + rule.prelude_source_length],
            utf16(":heading(1.0) ")
        );
        assert!(
            crate::css::selector_parser::parse_selector_list(
                b":heading(1.0) ",
                &[],
                crate::css::selector_parser::SelectorType::Standalone,
                crate::css::selector_parser::SelectorParsingMode::Standard,
            )
            .is_err()
        );
    }

    #[test]
    fn parses_typed_at_rule_preludes() {
        let rules = parse_stylesheet(
            br#"@layer reset.theme, widgets; @namespace svg url("urn:svg");
                @keyframes "fade" { from, 50%, to {} }
                @property --accent { syntax: "<color>"; inherits: false }
                @page invoice:left:first, :blank {}
                @scope (.card) to (> .footer) {}"#,
        );
        assert_eq!(
            parse_rule_prelude(&rules[0]),
            ParsedRulePrelude::Names(vec![
                utf16("reset.theme").into_boxed_slice().into(),
                utf16("widgets").into_boxed_slice().into()
            ])
        );
        assert_eq!(
            parse_rule_prelude(&rules[1]),
            ParsedRulePrelude::Namespace {
                prefix: Some(utf16("svg").into_boxed_slice().into()),
                uri: utf16("urn:svg").into_boxed_slice().into(),
            }
        );
        assert_eq!(
            parse_rule_prelude(&rules[2]),
            ParsedRulePrelude::Name(utf16("fade").into_boxed_slice().into())
        );
        let Rule::At(keyframes) = &rules[2] else {
            panic!("expected @keyframes");
        };
        let RuleOrDeclarations::Rule(keyframe) = &keyframes.children[0] else {
            panic!("expected keyframe rule");
        };
        assert_eq!(
            parse_rule_prelude(keyframe),
            ParsedRulePrelude::KeyframeSelectors(vec![0.0, 50.0, 100.0])
        );
        assert_eq!(
            parse_rule_prelude(&rules[3]),
            ParsedRulePrelude::Name(utf16("--accent").into_boxed_slice().into())
        );
        let ParsedRulePrelude::PageSelectors(selectors) = parse_rule_prelude(&rules[4]) else {
            panic!("expected page selectors");
        };
        assert_eq!(selectors.len(), 2);
        assert_eq!(selectors[0].name.as_deref(), Some(utf16("invoice").as_slice()));
        assert_eq!(selectors[0].pseudo_classes, vec![0, 2]);
        assert_eq!(selectors[1].name, None);
        assert_eq!(selectors[1].pseudo_classes, vec![3]);
        assert_eq!(
            parse_rule_prelude(&rules[5]),
            ParsedRulePrelude::Scope {
                start: Some(utf16(".card").into_boxed_slice().into()),
                end: Some(utf16("> .footer").into_boxed_slice().into()),
            }
        );
    }

    #[test]
    fn parses_keyframe_selectors_with_an_unterminated_comment() {
        let values = consume_a_list_of_component_values(tokenize_for_parser(b"50% /*".as_slice())).unwrap();
        assert_eq!(
            parse_keyframe_selectors(&values),
            ParsedRulePrelude::KeyframeSelectors(vec![50.0])
        );
    }

    #[test]
    fn parses_import_and_function_preludes() {
        let rules = parse_stylesheet(
            br#"@import url("theme.css") layer(theme) scope((.card) to (> .end)) supports(display: grid) screen;
                @function --size(--base <length>: 10px, --scale type(<number>)) returns <length> {}"#,
        );
        let ParsedRulePrelude::Import(items) = parse_rule_prelude(&rules[0]) else {
            panic!("expected import prelude");
        };
        assert_eq!(
            items.iter().map(|(_, flags)| *flags).collect::<Vec<_>>(),
            vec![0, 1, 2, 3, 4, 5, 6]
        );
        let ParsedRulePrelude::Function {
            name,
            parameters,
            return_type,
        } = parse_rule_prelude(&rules[1])
        else {
            panic!("expected function prelude");
        };
        assert_eq!(name.as_ref(), utf16("--size"));
        assert_eq!(parameters.len(), 2);
        assert_eq!(parameters[0].0.as_ref(), utf16("--base"));
        assert!(parameters[0].1.is_some());
        assert!(parameters[0].2.is_some());
        assert_eq!(parameters[1].0.as_ref(), utf16("--scale"));
        assert!(parameters[1].1.is_some());
        assert!(parameters[1].2.is_none());
        assert!(return_type.is_some());
    }

    #[test]
    fn rejects_invalid_typed_preludes() {
        for source in [
            "@layer inherit;",
            "@namespace svg;",
            "@keyframes none {}",
            "@property accent {}",
            "@page :unknown {}",
            "@scope (.card) trailing {}",
            "@scope to .footer {}",
        ] {
            let rules = parse_stylesheet(source.as_bytes());
            assert_eq!(parse_rule_prelude(&rules[0]), ParsedRulePrelude::Invalid, "{source}");
        }
    }

    #[test]
    fn page_selector_pseudo_classes_must_be_adjacent() {
        let rules = parse_stylesheet(b"@page named :first {} @page :first :left {}");
        assert!(
            rules
                .iter()
                .all(|rule| parse_rule_prelude(rule) == ParsedRulePrelude::Invalid)
        );
    }
}
