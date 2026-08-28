/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Style values are shared only across the thread-confined CSS object graph.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::{keyword_from_ascii_case_insensitive, keyword_to_generic_font_family};
use crate::css::css_tokenizer::{
    CssNumberType, ParserString, ParserToken, ParserTokenKind, SourcePosition, TokenizerInput, tokenize_for_parser,
};
use crate::css::descriptor_metadata::{CUSTOM_DESCRIPTOR_ID, descriptor_longhands};
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{
    ComponentKind, ComponentSerializationMode, ComponentValue, consume_a_component_value,
    consume_a_list_of_component_values, trim_whitespace,
};
use crate::css::parser::descriptor_parser::{ParsedDescriptor, parse_descriptor};
use crate::css::parser::query_parser::{
    ContainerCondition, Expression, FfiQueryHandle, MediaQuery, QueryKind, ResolveQueryFeature,
    expression_query_handle, ffi_resolver, media_query_handle, parse_container_condition_list_from_component_values,
    parse_media_query_list_from_component_values, parse_supports_condition_from_component_values,
    parse_supports_declaration_from_component_values,
};
use crate::css::parser::syntax::{SyntaxNode, parse_syntax, parse_with_syntax};
use crate::css::parser::value_parser::{
    FfiParseStatus, FfiValueParsingContext, FfiValueParsingContextKind, ParseContext, ParseOutcome,
    is_valid_custom_ident, parse_css_value_with_utf16_source, parse_url_value,
};
use crate::css::property_metadata::property_id;
use crate::css::selector_parser::{RustParsedSelectorList, SelectorType, parse_selector_list_from_component_values};
use crate::css::serialize::serialize_component_values_to_utf16;
use crate::css::style_compute::value_is_computationally_independent;
use crate::css::style_value::{RetainedRequestUrlModifierList, RetainedString, StyleValueData};
use std::borrow::Cow;
use std::collections::HashMap;
use std::ffi::c_void;
use std::fmt;
use std::pin::Pin;
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiPageSelectorItemKind {
    Left = 0,
    Right = 1,
    First = 2,
    Blank = 3,
    Name = 0x80,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiImportPreludeItemKind {
    UrlFunction = 0,
    Layer = 1,
    Scope = 2,
    ScopeStart = 3,
    ScopeEnd = 4,
    Supports = 5,
    Media = 6,
    UrlValue = 7,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiScopePreludeItemKind {
    Start = 0,
    End = 1,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFunctionParameterItemKind {
    Parameter = 0,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiPropertyPreludeItemKind {
    InheritsFalse = 0,
    InheritsTrue = 1,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFontFeatureValuesRuleKind {
    Annotation,
    CharacterVariant,
    HistoricalForms,
    Ornaments,
    Styleset,
    Stylistic,
    Swash,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiRuleKind {
    Qualified,
    Unknown,
    IgnoredVendor,
    Container,
    CounterStyle,
    FontFace,
    FontFeatureValues,
    FontFeatureValuesRule,
    Function,
    Import,
    Keyframes,
    Layer,
    Margin,
    Media,
    Namespace,
    Page,
    Property,
    Scope,
    Supports,
}

const UNUSED_PRELUDE_ITEM_KIND: u8 = 0;

struct ParsedPreludeItem {
    value: Option<ParserString>,
    number_value: f64,
    kind: u8,
    selector_list: *mut c_void,
    query: *const c_void,
    syntax: *const c_void,
    style_value: *const c_void,
}

impl Default for ParsedPreludeItem {
    fn default() -> Self {
        Self {
            value: None,
            number_value: 0.0,
            kind: UNUSED_PRELUDE_ITEM_KIND,
            selector_list: std::ptr::null_mut(),
            query: std::ptr::null(),
            syntax: std::ptr::null(),
            style_value: std::ptr::null(),
        }
    }
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
    style_nested: bool,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct QualifiedRule {
    pub prelude: Vec<ComponentValue>,
    pub prelude_is_selector: bool,
    prelude_is_relative: bool,
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
        has_start: bool,
        has_end: bool,
    },
    Import(ParsedImportPrelude),
    Function {
        name: ParserString,
        parameters: Vec<ParsedFunctionParameter>,
        return_type: Arc<SyntaxNode>,
    },
    Property {
        name: ParserString,
        syntax_source: ParserString,
        syntax: Arc<SyntaxNode>,
        inherits: bool,
        initial_value: Option<ParsedStyleValue>,
    },
    MediaQueries(Vec<MediaQuery>),
    SupportsCondition(Expression),
    ContainerConditions(Vec<ContainerCondition>),
    FontFeatureValuesRule(FfiFontFeatureValuesRuleKind),
}

#[derive(Clone, Debug, PartialEq)]
struct ParsedPageSelector {
    name: Option<ParserString>,
    pseudo_classes: Vec<FfiPageSelectorItemKind>,
}

#[derive(Clone, Debug, PartialEq)]
struct ParsedFunctionParameter {
    name: ParserString,
    syntax: Arc<SyntaxNode>,
    default_value: Option<ParsedStyleValue>,
}

#[derive(Clone, Debug, PartialEq)]
struct ParsedImportPrelude {
    url: ParsedStyleValue,
    url_kind: FfiImportPreludeItemKind,
    layer: Option<ParserString>,
    has_scope: bool,
    scope_start: Option<RustParsedSelectorList>,
    scope_end: Option<RustParsedSelectorList>,
    supports: Option<Expression>,
    media_queries: Vec<MediaQuery>,
}

#[derive(Clone, PartialEq)]
struct ParsedStyleValue(Arc<StyleValueData>);

impl fmt::Debug for ParsedStyleValue {
    fn fmt(&self, formatter: &mut fmt::Formatter<'_>) -> fmt::Result {
        formatter.write_str("ParsedStyleValue(..)")
    }
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
                FfiPageSelectorItemKind::Left
            } else if equals_ascii_case_insensitive(pseudo_class, b"right") {
                FfiPageSelectorItemKind::Right
            } else if equals_ascii_case_insensitive(pseudo_class, b"first") {
                FfiPageSelectorItemKind::First
            } else if equals_ascii_case_insensitive(pseudo_class, b"blank") {
                FfiPageSelectorItemKind::Blank
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

fn parenthesized_values(value: &ComponentValue) -> Option<&[ComponentValue]> {
    let ComponentKind::SimpleBlock {
        opening: ParserTokenKind::OpenParen,
        values,
    } = &value.kind
    else {
        return None;
    };
    Some(values)
}

type ScopeSelectorComponents<'a> = (Option<&'a [ComponentValue]>, Option<&'a [ComponentValue]>);

fn scope_selector_components(values: &[ComponentValue]) -> Option<ScopeSelectorComponents<'_>> {
    let values = non_whitespace(values);
    let mut position = 0;
    let start = values.get(position).and_then(|value| {
        let values = parenthesized_values(value)?;
        position += 1;
        Some(values)
    });
    let end = if values
        .get(position)
        .and_then(|value| value.ident())
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"to"))
    {
        position += 1;
        let values = values.get(position).and_then(|value| parenthesized_values(value))?;
        position += 1;
        Some(values)
    } else {
        None
    };
    if position != values.len() {
        return None;
    }
    Some((start, end))
}

fn parse_scope_prelude(values: &[ComponentValue]) -> ParsedRulePrelude {
    let Some((start, end)) = scope_selector_components(values) else {
        return ParsedRulePrelude::Invalid;
    };
    ParsedRulePrelude::Scope {
        has_start: start.is_some(),
        has_end: end.is_some(),
    }
}

fn skip_whitespace(values: &[ComponentValue], position: &mut usize) {
    while values.get(*position).is_some_and(ComponentValue::is_whitespace) {
        *position += 1;
    }
}

fn component_slice_source(values: &[ComponentValue]) -> ParserString {
    component_list_source(values).into_owned().into_boxed_slice().into()
}

fn parse_import_scope(contents: &[ComponentValue]) -> Option<ScopeSelectorComponents<'_>> {
    let mut position = 0;
    skip_whitespace(contents, &mut position);
    let start = contents.get(position).and_then(|value| {
        let values = parenthesized_values(value)?;
        position += 1;
        Some(values)
    });
    skip_whitespace(contents, &mut position);
    let end = if contents
        .get(position)
        .and_then(|value| value.ident())
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"to"))
    {
        position += 1;
        skip_whitespace(contents, &mut position);
        let values = contents.get(position).and_then(parenthesized_values)?;
        position += 1;
        Some(values)
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
        let values = &contents[position..];
        position = contents.len();
        Some(values)
    } else {
        start
    };
    (position == contents.len()).then_some((start, end))
}

fn parse_import_selector(values: &[ComponentValue], selector_type: SelectorType) -> Option<RustParsedSelectorList> {
    let selectors = parse_selector_list_from_component_values(values, selector_type).ok()?;
    if selectors.is_empty() || selectors.contains_pseudo_element() {
        return None;
    }
    Some(selectors)
}

fn parse_import_url(
    context: &ParseContext,
    value: &ComponentValue,
) -> Option<(ParsedStyleValue, FfiImportPreludeItemKind)> {
    let (url, kind) = match &value.kind {
        ComponentKind::Token(ParserTokenKind::String(url)) => (
            StyleValueData::Url {
                url: RetainedString::from_utf16(url)?,
                url_type: 0,
                modifiers: RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
            },
            FfiImportPreludeItemKind::UrlValue,
        ),
        ComponentKind::Token(ParserTokenKind::Url(_)) => {
            (parse_url_value(context, value)?, FfiImportPreludeItemKind::UrlValue)
        }
        ComponentKind::Function { name, .. } if equals_ascii_case_insensitive(name, b"url") => {
            (parse_url_value(context, value)?, FfiImportPreludeItemKind::UrlFunction)
        }
        _ => return None,
    };
    Some((ParsedStyleValue(Arc::new(url)), kind))
}

fn parse_import_prelude<R>(values: &[ComponentValue], context: &ParseContext, resolve_feature: &R) -> ParsedRulePrelude
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    let mut position = 0;
    skip_whitespace(values, &mut position);
    let Some(url) = values.get(position) else {
        return ParsedRulePrelude::Invalid;
    };
    let Some((url, url_kind)) = parse_import_url(context, url) else {
        return ParsedRulePrelude::Invalid;
    };
    position += 1;
    let mut layer = None;
    let mut has_scope = false;
    let mut scope_start = None;
    let mut scope_end = None;
    let mut supports = None;
    loop {
        skip_whitespace(values, &mut position);
        let Some(value) = values.get(position) else {
            break;
        };
        if layer.is_none()
            && value
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"layer"))
        {
            layer = Some(Vec::new().into_boxed_slice().into());
            position += 1;
            continue;
        }
        if layer.is_none()
            && let Some((name, contents)) = value.function()
            && equals_ascii_case_insensitive(name, b"layer")
            && let Some(layer_name) = parse_layer_name(contents, false)
        {
            layer = Some(layer_name);
            position += 1;
            continue;
        }
        if !has_scope
            && value
                .ident()
                .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"scope"))
        {
            has_scope = true;
            position += 1;
            continue;
        }
        if !has_scope
            && let Some((name, contents)) = value.function()
            && equals_ascii_case_insensitive(name, b"scope")
            && let Some((start, end)) = parse_import_scope(contents)
        {
            if let Some(start) = start {
                let Some(selectors) = parse_import_selector(start, SelectorType::Standalone) else {
                    return ParsedRulePrelude::Invalid;
                };
                scope_start = Some(selectors);
            }
            if let Some(end) = end {
                let Some(selectors) = parse_import_selector(end, SelectorType::Relative) else {
                    return ParsedRulePrelude::Invalid;
                };
                scope_end = Some(selectors);
            }
            has_scope = true;
            position += 1;
            continue;
        }
        if supports.is_none()
            && let Some((name, contents)) = value.function()
            && equals_ascii_case_insensitive(name, b"supports")
        {
            let Some(expression) = parse_supports_condition_from_component_values(contents, &|_, _| false)
                .or_else(|| parse_supports_declaration_from_component_values(contents, &|_, _| false))
            else {
                return ParsedRulePrelude::Invalid;
            };
            supports = Some(expression);
            position += 1;
            continue;
        }
        break;
    }
    let media_queries = parse_media_query_list_from_component_values(&values[position..], resolve_feature);
    ParsedRulePrelude::Import(ParsedImportPrelude {
        url,
        url_kind,
        layer,
        has_scope,
        scope_start,
        scope_end,
        supports,
        media_queries,
    })
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

fn unresolved_contains_arbitrary_substitution_function(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::Unresolved {
            presence_attr: true,
            ..
        } | StyleValueData::Unresolved {
            presence_dashed_function: true,
            ..
        } | StyleValueData::Unresolved { presence_env: true, .. }
            | StyleValueData::Unresolved { presence_if: true, .. }
            | StyleValueData::Unresolved {
                presence_inherit: true,
                ..
            }
            | StyleValueData::Unresolved { presence_var: true, .. }
    )
}

fn parse_function_default(
    context: &ParseContext,
    values: &[ComponentValue],
    syntax: &SyntaxNode,
) -> Option<ParsedStyleValue> {
    let source = component_list_source(values);
    let ParseOutcome::Parsed(unparsed) =
        parse_css_value_with_utf16_source(context, property_id::CUSTOM, values, source.as_ref())
    else {
        return None;
    };
    if crate::css::style_compute::value_is_css_wide_keyword(&unparsed)
        || unresolved_contains_arbitrary_substitution_function(&unparsed)
    {
        return Some(ParsedStyleValue(unparsed));
    }
    let source = serialize_component_values_to_utf16(values, ComponentSerializationMode::Retokenize);
    parse_with_syntax(context, &source, syntax).map(|value| ParsedStyleValue(Arc::new(value)))
}

fn parse_function_prelude(rule: &AtRule, context: &ParseContext) -> ParsedRulePrelude {
    if !rule.has_block {
        return ParsedRulePrelude::Invalid;
    }
    let values = &rule.prelude;
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
            let syntax = match type_source {
                Some(ref source) => {
                    let Some(syntax) = parse_syntax(source.as_ref(), false) else {
                        return ParsedRulePrelude::Invalid;
                    };
                    syntax
                }
                None => SyntaxNode::Universal,
            };
            skip_whitespace(parameter, &mut parameter_position);
            let default_value = if parameter.get(parameter_position).is_some_and(ComponentValue::is_colon) {
                parameter_position += 1;
                let Some(default_value) = parse_function_default(context, &parameter[parameter_position..], &syntax)
                else {
                    return ParsedRulePrelude::Invalid;
                };
                parameter_position = parameter.len();
                Some(default_value)
            } else {
                None
            };
            skip_whitespace(parameter, &mut parameter_position);
            if parameter_position != parameter.len() {
                return ParsedRulePrelude::Invalid;
            }
            parameters.push(ParsedFunctionParameter {
                name: parameter_name.to_vec().into_boxed_slice().into(),
                syntax: Arc::new(syntax),
                default_value,
            });
        }
    }
    skip_whitespace(values, &mut position);
    let return_type = if values
        .get(position)
        .and_then(ComponentValue::ident)
        .is_some_and(|ident| equals_ascii_case_insensitive(ident, b"returns"))
    {
        position += 1;
        let Some(return_type_source) = parse_css_type(values, &mut position) else {
            return ParsedRulePrelude::Invalid;
        };
        let Some(return_type) = parse_syntax(return_type_source.as_ref(), false) else {
            return ParsedRulePrelude::Invalid;
        };
        Arc::new(return_type)
    } else {
        Arc::new(SyntaxNode::Universal)
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

fn at_rule_declarations(rule: &AtRule) -> impl Iterator<Item = &Declaration> {
    rule.children
        .iter()
        .filter_map(|item| match item {
            RuleOrDeclarations::Declarations(declarations) => Some(declarations.as_slice()),
            RuleOrDeclarations::Rule(_) => None,
        })
        .flatten()
}

fn parse_property_prelude(rule: &AtRule, context: &ParseContext) -> ParsedRulePrelude {
    if !rule.has_block {
        return ParsedRulePrelude::Invalid;
    }
    let ParsedRulePrelude::Name(name) = parse_single_name(&rule.prelude, |name| {
        name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) && name.len() > 2
    }) else {
        return ParsedRulePrelude::Invalid;
    };

    let mut syntax_source: Option<ParserString> = None;
    let mut inherits = None;
    let mut initial_value_source = None;
    for declaration in at_rule_declarations(rule) {
        let source = component_list_source(&declaration.value);
        if parse_descriptor(
            context,
            2,
            declaration.name.as_ref(),
            &declaration.value,
            source.as_ref(),
        )
        .is_none()
        {
            continue;
        }
        if equals_ascii_case_insensitive(declaration.name.as_ref(), b"syntax") {
            let values = non_whitespace(&declaration.value);
            syntax_source = values
                .as_slice()
                .first()
                .and_then(|value| value.string())
                .filter(|_| values.len() == 1)
                .map(|source| source.to_vec().into_boxed_slice().into());
        } else if equals_ascii_case_insensitive(declaration.name.as_ref(), b"inherits") {
            let values = non_whitespace(&declaration.value);
            inherits = values
                .as_slice()
                .first()
                .and_then(|value| value.ident())
                .filter(|_| values.len() == 1)
                .and_then(|value| {
                    if equals_ascii_case_insensitive(value, b"true") {
                        Some(true)
                    } else if equals_ascii_case_insensitive(value, b"false") {
                        Some(false)
                    } else {
                        None
                    }
                });
        } else if equals_ascii_case_insensitive(declaration.name.as_ref(), b"initial-value") {
            initial_value_source = Some(serialize_component_values_to_utf16(
                &declaration.value,
                ComponentSerializationMode::Retokenize,
            ));
        }
    }

    let (Some(syntax_source), Some(inherits)) = (syntax_source, inherits) else {
        return ParsedRulePrelude::Invalid;
    };
    let Some(syntax) = parse_syntax(syntax_source.as_ref(), true) else {
        return ParsedRulePrelude::Invalid;
    };
    if initial_value_source.is_none() && !matches!(syntax, SyntaxNode::Universal) {
        return ParsedRulePrelude::Invalid;
    }
    let initial_value = initial_value_source.map(|source| {
        ParsedStyleValue(Arc::new(
            parse_with_syntax(context, &source, &syntax).unwrap_or(StyleValueData::GuaranteedInvalid),
        ))
    });
    if !matches!(syntax, SyntaxNode::Universal)
        && initial_value.as_ref().is_none_or(|value| {
            let value = value.0.as_ref();
            matches!(value, StyleValueData::GuaranteedInvalid)
                || !value_is_computationally_independent(value).unwrap_or_default()
        })
    {
        return ParsedRulePrelude::Invalid;
    }

    ParsedRulePrelude::Property {
        name,
        syntax_source,
        syntax: Arc::new(syntax),
        inherits,
        initial_value,
    }
}

fn parse_rule_prelude<R>(
    rule: &Rule,
    rule_kind: FfiRuleKind,
    parse_context: Option<&ParseContext>,
    resolve_feature: &R,
) -> ParsedRulePrelude
where
    R: Fn(QueryKind, &[u16]) -> Option<(u8, bool)>,
{
    match (rule, rule_kind) {
        (Rule::Qualified(rule), FfiRuleKind::Qualified) if !rule.prelude_is_selector => {
            parse_keyframe_selectors(&rule.prelude)
        }
        (Rule::Qualified(_), FfiRuleKind::Qualified) => ParsedRulePrelude::Unparsed,
        (Rule::At(rule), FfiRuleKind::Layer) => parse_layer_prelude(&rule.prelude, rule.has_block),
        (Rule::At(rule), FfiRuleKind::Property) => parse_context
            .map(|context| parse_property_prelude(rule, context))
            .unwrap_or_else(|| {
                parse_single_name(&rule.prelude, |name| {
                    name.starts_with(&[u16::from(b'-'), u16::from(b'-')]) && name.len() > 2
                })
            }),
        (Rule::At(rule), FfiRuleKind::CounterStyle) => parse_single_name(&rule.prelude, |name| {
            is_valid_custom_ident(name, &["none"])
                && (parse_context.is_some_and(|context| context.is_ua_style_sheet)
                    || ![
                        b"decimal".as_slice(),
                        b"disc",
                        b"square",
                        b"circle",
                        b"disclosure-open",
                        b"disclosure-closed",
                    ]
                    .iter()
                    .any(|reserved| equals_ascii_case_insensitive(name, reserved)))
        }),
        (Rule::At(rule), FfiRuleKind::Keyframes) => {
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
        (Rule::At(rule), FfiRuleKind::Namespace) => parse_namespace_prelude(&rule.prelude),
        (Rule::At(rule), FfiRuleKind::Page) => parse_page_selectors(&rule.prelude),
        (Rule::At(rule), FfiRuleKind::FontFeatureValues) => parse_font_family_names(&rule.prelude),
        (Rule::At(rule), FfiRuleKind::FontFeatureValuesRule) => {
            let (kind, _) = font_feature_values_rule(rule.name.as_ref()).unwrap();
            ParsedRulePrelude::FontFeatureValuesRule(kind)
        }
        (Rule::At(rule), FfiRuleKind::Scope) => parse_scope_prelude(&rule.prelude),
        (Rule::At(rule), FfiRuleKind::Import) => parse_context
            .map(|context| parse_import_prelude(&rule.prelude, context, resolve_feature))
            .unwrap_or(ParsedRulePrelude::Invalid),
        (Rule::At(rule), FfiRuleKind::Function) => parse_context
            .map(|context| parse_function_prelude(rule, context))
            .unwrap_or(ParsedRulePrelude::Invalid),
        (Rule::At(rule), FfiRuleKind::Media) => ParsedRulePrelude::MediaQueries(
            parse_media_query_list_from_component_values(&rule.prelude, resolve_feature),
        ),
        (Rule::At(rule), FfiRuleKind::Supports) => {
            parse_supports_condition_from_component_values(&rule.prelude, &|_, _| false)
                .map(ParsedRulePrelude::SupportsCondition)
                .unwrap_or(ParsedRulePrelude::Invalid)
        }
        (Rule::At(rule), FfiRuleKind::Container) => {
            parse_container_condition_list_from_component_values(&rule.prelude, resolve_feature)
                .map(ParsedRulePrelude::ContainerConditions)
                .unwrap_or(ParsedRulePrelude::Invalid)
        }
        (Rule::At(rule), FfiRuleKind::FontFace | FfiRuleKind::Margin) => {
            if non_whitespace(&rule.prelude).is_empty() {
                ParsedRulePrelude::Empty
            } else {
                ParsedRulePrelude::Invalid
            }
        }
        (Rule::At(_), FfiRuleKind::Unknown | FfiRuleKind::IgnoredVendor) => ParsedRulePrelude::Unparsed,
        _ => unreachable!(),
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

fn has_ignored_vendor_prefix(name: TokenizerInput<'_>) -> bool {
    let starts_with = |expected: &[u8]| {
        name.len() >= expected.len()
            && expected
                .iter()
                .enumerate()
                .all(|(index, &expected)| name.code_unit_at(index) == u16::from(expected))
    };
    !name.is_empty()
        && name.code_unit_at(0) == u16::from(b'-')
        && !starts_with(b"--")
        && !starts_with(b"-libweb-")
        && (0..name.len())
            .filter(|&index| name.code_unit_at(index) == u16::from(b'-'))
            .count()
            > 1
}

/// # Safety
/// The source view must contain exactly one valid pointer when non-empty.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_has_ignored_vendor_prefix(source: FfiUtf16View) -> bool {
    crate::abort_on_panic(|| unsafe { source.units() }.is_some_and(has_ignored_vendor_prefix))
}

fn rule_kind(rule: &Rule) -> FfiRuleKind {
    let Rule::At(rule) = rule else {
        return FfiRuleKind::Qualified;
    };
    at_rule_kind(rule.name.as_ref())
}

fn at_rule_kind(name: &[u16]) -> FfiRuleKind {
    if equals_ascii_case_insensitive(name, b"keyframes") || equals_ascii_case_insensitive(name, b"-webkit-keyframes") {
        FfiRuleKind::Keyframes
    } else if has_ignored_vendor_prefix(name.into()) {
        FfiRuleKind::IgnoredVendor
    } else if equals_ascii_case_insensitive(name, b"container") {
        FfiRuleKind::Container
    } else if equals_ascii_case_insensitive(name, b"counter-style") {
        FfiRuleKind::CounterStyle
    } else if equals_ascii_case_insensitive(name, b"font-face") {
        FfiRuleKind::FontFace
    } else if equals_ascii_case_insensitive(name, b"font-feature-values") {
        FfiRuleKind::FontFeatureValues
    } else if font_feature_values_rule(name).is_some() {
        FfiRuleKind::FontFeatureValuesRule
    } else if equals_ascii_case_insensitive(name, b"function") {
        FfiRuleKind::Function
    } else if equals_ascii_case_insensitive(name, b"import") {
        FfiRuleKind::Import
    } else if equals_ascii_case_insensitive(name, b"layer") {
        FfiRuleKind::Layer
    } else if is_margin_rule_name(name) {
        FfiRuleKind::Margin
    } else if equals_ascii_case_insensitive(name, b"media") {
        FfiRuleKind::Media
    } else if equals_ascii_case_insensitive(name, b"namespace") {
        FfiRuleKind::Namespace
    } else if equals_ascii_case_insensitive(name, b"page") {
        FfiRuleKind::Page
    } else if equals_ascii_case_insensitive(name, b"property") {
        FfiRuleKind::Property
    } else if equals_ascii_case_insensitive(name, b"scope") {
        FfiRuleKind::Scope
    } else if equals_ascii_case_insensitive(name, b"supports") {
        FfiRuleKind::Supports
    } else {
        FfiRuleKind::Unknown
    }
}

fn font_feature_values_rule(name: &[u16]) -> Option<(FfiFontFeatureValuesRuleKind, usize)> {
    if equals_ascii_case_insensitive(name, b"annotation") {
        Some((FfiFontFeatureValuesRuleKind::Annotation, 1))
    } else if equals_ascii_case_insensitive(name, b"character-variant") {
        Some((FfiFontFeatureValuesRuleKind::CharacterVariant, 2))
    } else if equals_ascii_case_insensitive(name, b"historical-forms") {
        Some((FfiFontFeatureValuesRuleKind::HistoricalForms, 1))
    } else if equals_ascii_case_insensitive(name, b"ornaments") {
        Some((FfiFontFeatureValuesRuleKind::Ornaments, 1))
    } else if equals_ascii_case_insensitive(name, b"styleset") {
        Some((FfiFontFeatureValuesRuleKind::Styleset, usize::MAX))
    } else if equals_ascii_case_insensitive(name, b"stylistic") {
        Some((FfiFontFeatureValuesRuleKind::Stylistic, 1))
    } else if equals_ascii_case_insensitive(name, b"swash") {
        Some((FfiFontFeatureValuesRuleKind::Swash, 1))
    } else {
        None
    }
}

fn is_font_feature_value_rule_name(name: &[u16]) -> bool {
    font_feature_values_rule(name).is_some()
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
        let style_nested = self
            .rule_context
            .iter()
            .any(|context| matches!(context, RuleContext::Style | RuleContext::AtScope));
        let mut rule = AtRule {
            name,
            prelude: Vec::with_capacity(4),
            children: Vec::new(),
            has_block: false,
            style_nested,
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
        let style_nested = self
            .rule_context
            .iter()
            .any(|context| matches!(context, RuleContext::Style | RuleContext::AtScope));
        let mut rule = QualifiedRule {
            prelude: Vec::with_capacity(8),
            prelude_is_selector: qualified_context == RuleContext::Style,
            prelude_is_relative: style_nested,
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
    pub descriptor_id: u8,
    pub parse_status: FfiParseStatus,
    pub parsed_value: *const c_void,
    pub font_feature_values_start: usize,
    pub font_feature_value_count: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxDescriptor {
    pub name_offset: usize,
    pub name_length: usize,
    pub descriptor_id: u8,
    pub value: *const c_void,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxRule {
    pub rule_type: u8,
    pub rule_kind: FfiRuleKind,
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
    pub parsed_prelude_syntax: *const c_void,
    pub selector_list: *mut c_void,
    pub descriptors_start: usize,
    pub descriptor_count: usize,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiSyntaxPreludeItem {
    pub value_offset: usize,
    pub value_length: usize,
    pub number_value: f64,
    pub kind: u8,
    pub selector_list: *mut c_void,
    pub query: *const c_void,
    pub syntax: *const c_void,
    pub style_value: *const c_void,
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
    pub descriptors: *const FfiSyntaxDescriptor,
    pub descriptor_count: usize,
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
    pub font_feature_values: *const u32,
    pub font_feature_value_count: usize,
    pub diagnostics: *const FfiSyntaxDiagnostic,
    pub diagnostic_count: usize,
}

pub struct FfiSyntaxParse {
    values: Vec<u16>,
    components: Vec<FfiSyntaxComponent>,
    component_indices: Vec<usize>,
    declarations: Vec<FfiSyntaxDeclaration>,
    descriptors: Vec<FfiSyntaxDescriptor>,
    descriptor_parse_cache: ParsedDescriptorCache,
    rules: Vec<FfiSyntaxRule>,
    items: Vec<FfiSyntaxItem>,
    item_indices: Vec<usize>,
    roots: Vec<usize>,
    prelude_items: Vec<FfiSyntaxPreludeItem>,
    font_feature_values: Vec<u32>,
    selector_lists: Vec<Pin<Box<RustParsedSelectorList>>>,
    query_handles: Vec<Arc<FfiQueryHandle>>,
    diagnostics: Vec<FfiSyntaxDiagnostic>,
    parse_context: *const ParseContext,
    resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
    resolve_query_feature: Option<ResolveQueryFeature>,
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

struct CollectedDescriptor {
    id: u8,
    name: ParserString,
    value: Arc<StyleValueData>,
}

fn descriptor_at_rule(rule_context: RuleContext) -> Option<u8> {
    match rule_context {
        RuleContext::AtFontFace => Some(0),
        RuleContext::AtPage => Some(1),
        RuleContext::AtProperty => Some(2),
        RuleContext::AtCounterStyle => Some(3),
        RuleContext::AtFunction => Some(4),
        _ => None,
    }
}

fn append_collected_descriptor(descriptors: &mut Vec<CollectedDescriptor>, descriptor: CollectedDescriptor) {
    if let Some(index) = descriptors.iter().position(|existing| {
        existing.id == descriptor.id
            && (descriptor.id != CUSTOM_DESCRIPTOR_ID || existing.name.as_ref() == descriptor.name.as_ref())
    }) {
        descriptors.remove(index);
    }
    descriptors.push(descriptor);
}

/// Memoizes `parse_descriptor` outcomes by declaration address, so a rule whose
/// descriptors feed both the declaration array and the descriptor array parses
/// each value once.
type ParsedDescriptorCache = HashMap<usize, Option<ParsedDescriptor>>;

fn collect_descriptors<'a>(
    declarations: impl IntoIterator<Item = &'a Declaration>,
    context: &ParseContext,
    parsed_cache: &mut ParsedDescriptorCache,
) -> Vec<CollectedDescriptor> {
    let mut descriptors = Vec::new();
    for declaration in declarations {
        let Some(at_rule) = descriptor_at_rule(declaration.rule_context) else {
            continue;
        };
        let descriptor = parsed_cache
            .entry(std::ptr::from_ref(declaration) as usize)
            .or_insert_with(|| {
                let source = component_list_source(&declaration.value);
                parse_descriptor(
                    context,
                    at_rule,
                    declaration.name.as_ref(),
                    &declaration.value,
                    source.as_ref(),
                )
            });
        let Some(descriptor) = descriptor.clone() else {
            continue;
        };
        let longhands = descriptor_longhands(at_rule, descriptor.id);
        if !longhands.is_empty()
            && let StyleValueData::Shorthand {
                sub_properties, values, ..
            } = descriptor.value.as_ref()
        {
            for longhand in longhands {
                let Some(index) = sub_properties
                    .as_slice()
                    .iter()
                    .position(|property| *property == longhand.property_id)
                else {
                    continue;
                };
                append_collected_descriptor(
                    &mut descriptors,
                    CollectedDescriptor {
                        id: longhand.descriptor_id,
                        name: declaration.name.clone(),
                        value: values.as_slice()[index].clone().into_arc(),
                    },
                );
            }
            continue;
        }
        append_collected_descriptor(
            &mut descriptors,
            CollectedDescriptor {
                id: descriptor.id,
                name: declaration.name.clone(),
                value: descriptor.value,
            },
        );
    }
    descriptors
}

fn parse_font_feature_values(declaration: &Declaration, maximum_value_count: usize) -> Option<Vec<u32>> {
    if declaration.important {
        return None;
    }
    let values = declaration
        .value
        .iter()
        .filter(|value| !value.is_whitespace())
        .map(|value| match value.kind {
            ComponentKind::Token(ParserTokenKind::Number {
                value,
                number_type: CssNumberType::Integer | CssNumberType::IntegerWithExplicitSign,
            }) if value >= 0.0 && value <= f64::from(u32::MAX) => Some(value as u32),
            _ => None,
        })
        .collect::<Option<Vec<_>>>()?;
    (!values.is_empty() && values.len() <= maximum_value_count).then_some(values)
}

impl FfiSyntaxParse {
    pub(crate) fn new(
        parse_context: *const ParseContext,
        resolve_property_id: Option<unsafe extern "C" fn(*const u16, usize) -> u16>,
        resolve_query_feature: Option<ResolveQueryFeature>,
        preserve_property_source_text: bool,
    ) -> Self {
        Self {
            values: Vec::new(),
            components: Vec::new(),
            component_indices: Vec::new(),
            declarations: Vec::new(),
            descriptors: Vec::new(),
            descriptor_parse_cache: HashMap::new(),
            rules: Vec::new(),
            items: Vec::new(),
            item_indices: Vec::new(),
            roots: Vec::new(),
            prelude_items: Vec::new(),
            font_feature_values: Vec::new(),
            selector_lists: Vec::new(),
            query_handles: Vec::new(),
            diagnostics: Vec::new(),
            parse_context,
            resolve_property_id,
            resolve_query_feature,
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

    fn append_declaration(
        &mut self,
        declaration: &Declaration,
        font_feature_maximum_value_count: Option<usize>,
    ) -> usize {
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
        let (property_id, descriptor_id, parse_status, parsed_value) =
            self.parse_declaration_value(declaration, value_source.as_ref());
        let font_feature_values = font_feature_maximum_value_count
            .and_then(|maximum_value_count| parse_font_feature_values(declaration, maximum_value_count));
        let (font_feature_values_start, font_feature_value_count) = if let Some(values) = font_feature_values {
            let start = self.font_feature_values.len();
            let count = values.len();
            self.font_feature_values.extend(values);
            (start, count)
        } else {
            (usize::MAX, 0)
        };
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
            descriptor_id,
            parse_status,
            parsed_value,
            font_feature_values_start,
            font_feature_value_count,
        });
        index
    }

    fn parse_declaration_value(
        &self,
        declaration: &Declaration,
        source_utf16: &[u16],
    ) -> (u16, u8, FfiParseStatus, *const c_void) {
        if self.parse_context.is_null() {
            return (u16::MAX, u8::MAX, FfiParseStatus::NotHandled, std::ptr::null());
        }
        if !declaration.is_property {
            let Some(at_rule) = descriptor_at_rule(declaration.rule_context) else {
                return (u16::MAX, u8::MAX, FfiParseStatus::NotHandled, std::ptr::null());
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
                    descriptor.id,
                    FfiParseStatus::Parsed,
                    Arc::into_raw(descriptor.value).cast::<c_void>(),
                ),
                None => (u16::MAX, u8::MAX, FfiParseStatus::Invalid, std::ptr::null()),
            };
        }
        let Some(resolve_property_id) = self.resolve_property_id else {
            return (u16::MAX, u8::MAX, FfiParseStatus::NotHandled, std::ptr::null());
        };
        // SAFETY: The declaration name remains live for this callback and the caller owns the callback.
        let property_id = unsafe { resolve_property_id(declaration.name.as_ptr(), declaration.name.len()) };
        if property_id == u16::MAX {
            return (property_id, u8::MAX, FfiParseStatus::NotHandled, std::ptr::null());
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
                u8::MAX,
                FfiParseStatus::Parsed,
                Arc::into_raw(value).cast::<c_void>(),
            ),
            ParseOutcome::Invalid => (property_id, u8::MAX, FfiParseStatus::Invalid, std::ptr::null()),
            ParseOutcome::NotHandled => (property_id, u8::MAX, FfiParseStatus::NotHandled, std::ptr::null()),
        }
    }

    fn append_collected_declaration(&mut self, descriptor: CollectedDescriptor) {
        let (name_offset, name_length) = self.append_value(descriptor.name.as_ref());
        self.declarations.push(FfiSyntaxDeclaration {
            name_offset,
            name_length,
            values_start: 0,
            value_count: 0,
            value_source_offset: 0,
            value_source_length: 0,
            is_property: false,
            important: false,
            original_value_offset: usize::MAX,
            original_value_length: 0,
            original_full_text_offset: usize::MAX,
            original_full_text_length: 0,
            start_line: 0,
            start_column: 0,
            property_id: u16::MAX,
            descriptor_id: descriptor.id,
            parse_status: FfiParseStatus::Parsed,
            parsed_value: Arc::into_raw(descriptor.value).cast(),
            font_feature_values_start: usize::MAX,
            font_feature_value_count: 0,
        });
    }

    fn append_declarations(
        &mut self,
        declarations: &[Declaration],
        font_feature_maximum_value_count: Option<usize>,
    ) -> (usize, usize) {
        let start = self.declarations.len();
        if !self.parse_context.is_null()
            && declarations
                .first()
                .is_some_and(|declaration| descriptor_at_rule(declaration.rule_context).is_some())
        {
            let context = unsafe { &*self.parse_context };
            for descriptor in collect_descriptors(declarations, context, &mut self.descriptor_parse_cache) {
                self.append_collected_declaration(descriptor);
            }
            return (start, self.declarations.len() - start);
        }
        for declaration in declarations {
            self.append_declaration(declaration, font_feature_maximum_value_count);
        }
        (start, declarations.len())
    }

    fn append_items(
        &mut self,
        items: &[RuleOrDeclarations],
        font_feature_maximum_value_count: Option<usize>,
    ) -> (usize, usize) {
        let indices = items
            .iter()
            .map(|item| self.append_item(item, font_feature_maximum_value_count))
            .collect::<Vec<_>>();
        let start = self.item_indices.len();
        self.item_indices.extend(indices);
        (start, items.len())
    }

    fn append_item(&mut self, item: &RuleOrDeclarations, font_feature_maximum_value_count: Option<usize>) -> usize {
        let (item_type, start, count) = match item {
            RuleOrDeclarations::Rule(rule) => (0, self.append_rule(rule), 1),
            RuleOrDeclarations::Declarations(declarations) => {
                let (start, count) = self.append_declarations(declarations, font_feature_maximum_value_count);
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

    fn append_selector_list(&mut self, values: &[ComponentValue], selector_type: SelectorType) -> Option<*mut c_void> {
        let selector_list = parse_selector_list_from_component_values(values, selector_type).ok()?;
        Some(self.retain_selector_list(selector_list))
    }

    fn retain_selector_list(&mut self, selector_list: RustParsedSelectorList) -> *mut c_void {
        let mut selector_list = Box::pin(selector_list);
        let pointer = std::ptr::from_mut(selector_list.as_mut().get_mut()).cast::<c_void>();
        self.selector_lists.push(selector_list);
        pointer
    }

    fn append_query_handle(&mut self, handle: Arc<FfiQueryHandle>) -> *const c_void {
        let pointer = Arc::as_ptr(&handle).cast::<c_void>();
        self.query_handles.push(handle);
        pointer
    }

    fn append_rule_descriptors(&mut self, rule: &Rule) -> (usize, usize) {
        let start = self.descriptors.len();
        let Some(context) = (unsafe { self.parse_context.as_ref() }) else {
            return (start, 0);
        };
        let Rule::At(rule) = rule else {
            return (start, 0);
        };
        let declarations = rule.children.iter().flat_map(|child| match child {
            RuleOrDeclarations::Declarations(declarations) => declarations.as_slice(),
            RuleOrDeclarations::Rule(_) => &[],
        });
        let collected = collect_descriptors(declarations, context, &mut self.descriptor_parse_cache);
        for descriptor in collected {
            let (name_offset, name_length) = self.append_value(descriptor.name.as_ref());
            self.descriptors.push(FfiSyntaxDescriptor {
                name_offset,
                name_length,
                descriptor_id: descriptor.id,
                value: Arc::into_raw(descriptor.value).cast(),
            });
        }
        (start, self.descriptors.len() - start)
    }

    fn append_rule(&mut self, rule: &Rule) -> usize {
        let rule_kind = rule_kind(rule);
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
        let font_feature_maximum_value_count = match rule {
            Rule::At(rule) => font_feature_values_rule(rule.name.as_ref()).map(|(_, maximum)| maximum),
            Rule::Qualified(_) => None,
        };
        let (declarations_start, declaration_count) = self.append_declarations(declarations, None);
        let (children_start, child_count) = self.append_items(children, font_feature_maximum_value_count);
        let (descriptors_start, descriptor_count) = self.append_rule_descriptors(rule);
        let selector_list = match rule {
            Rule::Qualified(rule) if rule.prelude_is_selector => self
                .append_selector_list(
                    &rule.prelude,
                    if rule.prelude_is_relative {
                        SelectorType::Relative
                    } else {
                        SelectorType::Standalone
                    },
                )
                .unwrap_or(std::ptr::null_mut()),
            _ => std::ptr::null_mut(),
        };
        let parse_context = unsafe { self.parse_context.as_ref() };
        let parsed_prelude = self.resolve_query_feature.map_or_else(
            || parse_rule_prelude(rule, rule_kind, parse_context, &|_, _| None),
            |resolve_feature| parse_rule_prelude(rule, rule_kind, parse_context, &ffi_resolver(resolve_feature)),
        );
        let mut parsed_prelude_name = None;
        let mut parsed_prelude_secondary = None;
        let mut parsed_prelude_syntax = std::ptr::null();
        let mut parsed_items: Vec<ParsedPreludeItem> = Vec::new();
        let parsed_prelude_kind =
            match parsed_prelude {
                ParsedRulePrelude::Unparsed => 0,
                ParsedRulePrelude::Invalid => 1,
                ParsedRulePrelude::Empty => 2,
                ParsedRulePrelude::Name(name) => {
                    parsed_prelude_name = Some(name);
                    3
                }
                ParsedRulePrelude::Names(names) => {
                    parsed_items.extend(names.into_iter().map(|name| ParsedPreludeItem {
                        value: Some(name),
                        ..Default::default()
                    }));
                    4
                }
                ParsedRulePrelude::KeyframeSelectors(selectors) => {
                    parsed_items.extend(selectors.into_iter().map(|value| ParsedPreludeItem {
                        number_value: value,
                        ..Default::default()
                    }));
                    5
                }
                ParsedRulePrelude::Namespace { prefix, uri } => {
                    parsed_prelude_name = prefix;
                    parsed_prelude_secondary = Some(uri);
                    6
                }
                ParsedRulePrelude::PageSelectors(selectors) => {
                    for selector in selectors {
                        parsed_items.push(ParsedPreludeItem {
                            value: selector.name,
                            kind: FfiPageSelectorItemKind::Name as u8,
                            ..Default::default()
                        });
                        parsed_items.extend(selector.pseudo_classes.into_iter().map(|pseudo_class| {
                            ParsedPreludeItem {
                                kind: pseudo_class as u8,
                                ..Default::default()
                            }
                        }));
                    }
                    7
                }
                ParsedRulePrelude::FontFamilyNames(names) => {
                    parsed_items.extend(names.into_iter().map(|name| ParsedPreludeItem {
                        value: Some(name),
                        ..Default::default()
                    }));
                    8
                }
                ParsedRulePrelude::Scope { has_start, has_end } => {
                    let Rule::At(at_rule) = rule else {
                        unreachable!();
                    };
                    let Some((start_components, end_components)) = scope_selector_components(prelude) else {
                        unreachable!();
                    };
                    let mut valid = true;
                    if has_start {
                        let start_components = start_components.unwrap();
                        let selector_list = self
                            .append_selector_list(
                                start_components,
                                if at_rule.style_nested {
                                    SelectorType::Relative
                                } else {
                                    SelectorType::Standalone
                                },
                            )
                            .unwrap_or_else(|| {
                                valid = false;
                                std::ptr::null_mut()
                            });
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiScopePreludeItemKind::Start as u8,
                            selector_list,
                            ..Default::default()
                        });
                    }
                    if has_end {
                        let end_components = end_components.unwrap();
                        let selector_list = self
                            .append_selector_list(end_components, SelectorType::Relative)
                            .unwrap_or_else(|| {
                                valid = false;
                                std::ptr::null_mut()
                            });
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiScopePreludeItemKind::End as u8,
                            selector_list,
                            ..Default::default()
                        });
                    }
                    if valid {
                        9
                    } else {
                        parsed_items.clear();
                        1
                    }
                }
                ParsedRulePrelude::Import(import) => {
                    parsed_items.push(ParsedPreludeItem {
                        kind: import.url_kind as u8,
                        style_value: Arc::into_raw(import.url.0).cast(),
                        ..Default::default()
                    });
                    if let Some(layer) = import.layer {
                        parsed_items.push(ParsedPreludeItem {
                            value: Some(layer),
                            kind: FfiImportPreludeItemKind::Layer as u8,
                            ..Default::default()
                        });
                    }
                    if import.has_scope {
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiImportPreludeItemKind::Scope as u8,
                            ..Default::default()
                        });
                    }
                    if let Some(selectors) = import.scope_start {
                        let selectors = self.retain_selector_list(selectors);
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiImportPreludeItemKind::ScopeStart as u8,
                            selector_list: selectors,
                            ..Default::default()
                        });
                    }
                    if let Some(selectors) = import.scope_end {
                        let selectors = self.retain_selector_list(selectors);
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiImportPreludeItemKind::ScopeEnd as u8,
                            selector_list: selectors,
                            ..Default::default()
                        });
                    }
                    if let Some(supports) = import.supports {
                        let supports = self.append_query_handle(expression_query_handle(supports, QueryKind::Supports));
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiImportPreludeItemKind::Supports as u8,
                            query: supports,
                            ..Default::default()
                        });
                    }
                    for media_query in import.media_queries {
                        let media_query = self.append_query_handle(media_query_handle(media_query));
                        parsed_items.push(ParsedPreludeItem {
                            kind: FfiImportPreludeItemKind::Media as u8,
                            query: media_query,
                            ..Default::default()
                        });
                    }
                    10
                }
                ParsedRulePrelude::Function {
                    name,
                    parameters,
                    return_type,
                } => {
                    parsed_prelude_name = Some(name);
                    parsed_prelude_syntax = Arc::into_raw(return_type).cast();
                    for parameter in parameters {
                        parsed_items.push(ParsedPreludeItem {
                            value: Some(parameter.name),
                            kind: FfiFunctionParameterItemKind::Parameter as u8,
                            syntax: Arc::into_raw(parameter.syntax).cast(),
                            style_value: parameter
                                .default_value
                                .map_or(std::ptr::null(), |value| Arc::into_raw(value.0).cast()),
                            ..Default::default()
                        });
                    }
                    11
                }
                ParsedRulePrelude::Property {
                    name,
                    syntax_source,
                    syntax,
                    inherits,
                    initial_value,
                } => {
                    parsed_prelude_name = Some(name);
                    parsed_prelude_secondary = Some(syntax_source);
                    parsed_prelude_syntax = Arc::into_raw(syntax).cast();
                    parsed_items.push(ParsedPreludeItem {
                        kind: if inherits {
                            FfiPropertyPreludeItemKind::InheritsTrue as u8
                        } else {
                            FfiPropertyPreludeItemKind::InheritsFalse as u8
                        },
                        style_value: initial_value.map_or(std::ptr::null(), |value| Arc::into_raw(value.0).cast()),
                        ..Default::default()
                    });
                    15
                }
                ParsedRulePrelude::MediaQueries(queries) => {
                    for query in queries {
                        let query = self.append_query_handle(media_query_handle(query));
                        parsed_items.push(ParsedPreludeItem {
                            query,
                            ..Default::default()
                        });
                    }
                    12
                }
                ParsedRulePrelude::SupportsCondition(expression) => {
                    let query = self.append_query_handle(expression_query_handle(expression, QueryKind::Supports));
                    parsed_items.push(ParsedPreludeItem {
                        query,
                        ..Default::default()
                    });
                    13
                }
                ParsedRulePrelude::ContainerConditions(conditions) => {
                    for condition in conditions {
                        let query = condition
                            .query
                            .map(|expression| {
                                self.append_query_handle(expression_query_handle(expression, QueryKind::Size))
                            })
                            .unwrap_or(std::ptr::null());
                        parsed_items.push(ParsedPreludeItem {
                            value: condition.name,
                            query,
                            ..Default::default()
                        });
                    }
                    14
                }
                ParsedRulePrelude::FontFeatureValuesRule(kind) => {
                    parsed_items.push(ParsedPreludeItem {
                        kind: kind as u8,
                        ..Default::default()
                    });
                    16
                }
            };
        let (parsed_prelude_name_offset, parsed_prelude_name_length) =
            self.append_optional_value(parsed_prelude_name.as_deref());
        let (parsed_prelude_secondary_offset, parsed_prelude_secondary_length) =
            self.append_optional_value(parsed_prelude_secondary.as_deref());
        let parsed_prelude_items_start = self.prelude_items.len();
        for item in parsed_items {
            let (value_offset, value_length) = self.append_optional_value(item.value.as_deref());
            self.prelude_items.push(FfiSyntaxPreludeItem {
                value_offset,
                value_length,
                number_value: item.number_value,
                kind: item.kind,
                selector_list: item.selector_list,
                query: item.query,
                syntax: item.syntax,
                style_value: item.style_value,
            });
        }
        let parsed_prelude_item_count = self.prelude_items.len() - parsed_prelude_items_start;
        let index = self.rules.len();
        self.rules.push(FfiSyntaxRule {
            rule_type,
            rule_kind,
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
            parsed_prelude_syntax,
            selector_list,
            descriptors_start,
            descriptor_count,
        });
        index
    }

    fn append_roots(&mut self, rules: &[Rule]) {
        self.roots = rules.iter().map(|rule| self.append_rule(rule)).collect();
    }

    fn append_root_items(&mut self, items: &[RuleOrDeclarations]) {
        self.roots = items.iter().map(|item| self.append_item(item, None)).collect();
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
            descriptors: self.descriptors.as_ptr(),
            descriptor_count: self.descriptors.len(),
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
            font_feature_values: self.font_feature_values.as_ptr(),
            font_feature_value_count: self.font_feature_values.len(),
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
    resolve_query_feature: ResolveQueryFeature,
) -> *mut FfiSyntaxParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let (mut parser, diagnostics) = Parser::from_source(source, Vec::new());
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, Some(resolve_query_feature), false);
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
    resolve_query_feature: ResolveQueryFeature,
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
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, Some(resolve_query_feature), false);
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
    resolve_query_feature: ResolveQueryFeature,
) -> *mut FfiSyntaxParse {
    crate::abort_on_panic(|| {
        let Some(source) = (unsafe { source.units() }) else {
            return std::ptr::null_mut();
        };
        let tokens = tokenize_for_parser(source);
        let diagnostics = token_diagnostics(&tokens);
        let prelude = consume_a_list_of_component_values(tokens).unwrap_or_default();
        let mut parse = FfiSyntaxParse::new(parse_context, resolve_property_id, Some(resolve_query_feature), false);
        parse.diagnostics = diagnostics;
        parse.append_roots(&[Rule::Qualified(QualifiedRule {
            prelude,
            prelude_is_selector: false,
            prelude_is_relative: false,
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
    resolve_query_feature: ResolveQueryFeature,
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
        let mut parse = FfiSyntaxParse::new(
            parse_context,
            resolve_property_id,
            Some(resolve_query_feature),
            preserve_property_source_text,
        );
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
        Declaration, FfiFontFeatureValuesRuleKind, FfiImportPreludeItemKind, FfiPageSelectorItemKind, FfiRuleKind,
        FfiSyntaxParse, ParsedRulePrelude, Rule, RuleContext, RuleOrDeclarations, SyntaxNode, at_rule_kind,
        collect_descriptors, consume_a_list_of_component_values, has_ignored_vendor_prefix, parse_block_contents,
        parse_font_feature_values, parse_keyframe_selectors, parse_rule, parse_rule_prelude, parse_stylesheet,
        rule_kind, token_diagnostics, tokenize_for_parser,
    };
    use crate::css::parser::value_parser::ParseContext;

    fn utf16(value: &str) -> Vec<u16> {
        value.encode_utf16().collect()
    }

    fn parse_test_rule_prelude(rule: &Rule) -> ParsedRulePrelude {
        parse_rule_prelude(rule, rule_kind(rule), None, &|_, _| None)
    }

    unsafe extern "C" fn discard_interned_string(_: *const u16, _: usize) -> usize {
        0
    }

    fn parse_context() -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            is_substituted_value: false,
            contains_attr_tainted_values: false,
            is_ua_style_sheet: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            intern_utf16_fly_string: Some(discard_interned_string),
            normalize_svg_path_data: None,
            precomputed_svg_paths: std::ptr::null(),
            precomputed_svg_path_count: 0,
            font_format_is_supported: None,
            font_tech_is_supported: None,
            descriptor_integer_resolution_context: std::ptr::null(),
            resolve_descriptor_integer: None,
            random_function_index: std::ptr::null_mut(),
        }
    }

    fn parse_test_rule_prelude_with_context(rule: &Rule) -> ParsedRulePrelude {
        parse_rule_prelude(rule, rule_kind(rule), Some(&parse_context()), &|_, _| None)
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
        let mut parse = FfiSyntaxParse::new(std::ptr::null(), None, None, false);
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
        let mut parse = super::FfiSyntaxParse::new(std::ptr::null(), None, None, false);
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
    fn parses_selector_preludes_from_component_values() {
        let rules = parse_stylesheet(b"svg|a {} :heading(1.0) {} a { > b {} @scope (> .start) to (> .end) {} }");
        let mut parse = FfiSyntaxParse::new(std::ptr::null(), None, None, false);
        parse.append_roots(&rules);

        assert!(!parse.rules[parse.roots[0]].selector_list.is_null());
        assert!(parse.rules[parse.roots[1]].selector_list.is_null());

        let nested_rule = parse
            .rules
            .iter()
            .find(|rule| {
                rule.rule_type == 1
                    && parse.values[rule.prelude_source_offset..rule.prelude_source_offset + rule.prelude_source_length]
                        == utf16("> b ")
            })
            .unwrap();
        assert!(!nested_rule.selector_list.is_null());

        let scope_rule = parse.rules.iter().find(|rule| rule.parsed_prelude_kind == 9).unwrap();
        assert_eq!(scope_rule.parsed_prelude_item_count, 2);
        for item in &parse.prelude_items[scope_rule.parsed_prelude_items_start
            ..scope_rule.parsed_prelude_items_start + scope_rule.parsed_prelude_item_count]
        {
            assert!(!item.selector_list.is_null());
            assert_eq!(item.value_offset, usize::MAX);
        }

        let rules = parse_stylesheet(b"@media all { > b {} @scope (> .start) {} }");
        let mut parse = FfiSyntaxParse::new(std::ptr::null(), None, None, false);
        parse.append_roots(&rules);
        let group_child = parse.rules.iter().find(|rule| rule.rule_type == 1).unwrap();
        assert!(group_child.selector_list.is_null());
        let scope_rule = parse
            .rules
            .iter()
            .find(|rule| rule.rule_type == 0 && rule.parsed_prelude_kind == 1)
            .unwrap();
        assert_eq!(scope_rule.parsed_prelude_kind, 1);
        assert_eq!(scope_rule.parsed_prelude_item_count, 0);
    }

    #[test]
    fn exports_rule_kinds() {
        let expected = [
            ("container", FfiRuleKind::Container),
            ("counter-style", FfiRuleKind::CounterStyle),
            ("font-face", FfiRuleKind::FontFace),
            ("font-feature-values", FfiRuleKind::FontFeatureValues),
            ("annotation", FfiRuleKind::FontFeatureValuesRule),
            ("function", FfiRuleKind::Function),
            ("import", FfiRuleKind::Import),
            ("keyframes", FfiRuleKind::Keyframes),
            ("-webkit-keyframes", FfiRuleKind::Keyframes),
            ("layer", FfiRuleKind::Layer),
            ("top-left", FfiRuleKind::Margin),
            ("media", FfiRuleKind::Media),
            ("namespace", FfiRuleKind::Namespace),
            ("page", FfiRuleKind::Page),
            ("property", FfiRuleKind::Property),
            ("scope", FfiRuleKind::Scope),
            ("supports", FfiRuleKind::Supports),
            ("unknown", FfiRuleKind::Unknown),
            ("-webkit-unknown", FfiRuleKind::IgnoredVendor),
            ("-libweb-unknown", FfiRuleKind::Unknown),
            ("-unknown", FfiRuleKind::Unknown),
            ("--unknown", FfiRuleKind::Unknown),
        ];
        for (name, expected) in expected {
            assert_eq!(at_rule_kind(&utf16(name)), expected, "{name}");
        }
        assert!(has_ignored_vendor_prefix(b"-webkit-unknown".into()));
        assert!(!has_ignored_vendor_prefix(b"-libweb-unknown".into()));
        assert_eq!(rule_kind(&parse_stylesheet(b"a {}")[0]), FfiRuleKind::Qualified);
    }

    #[test]
    fn parses_query_preludes_from_component_values() {
        unsafe extern "C" fn resolve_query_feature(_: u8, _: *const u16, _: usize) -> u16 {
            0
        }

        let rules = parse_stylesheet(
            b"@media screen and (width > 1px) {} @supports (display: grid) {} @container card (width > 1px), style(--theme: dark) {} @supports {}",
        );
        let mut parse = FfiSyntaxParse::new(std::ptr::null(), None, Some(resolve_query_feature), false);
        parse.append_roots(&rules);

        let media = &parse.rules[parse.roots[0]];
        assert_eq!(media.parsed_prelude_kind, 12);
        assert_eq!(media.parsed_prelude_item_count, 1);
        assert!(!parse.prelude_items[media.parsed_prelude_items_start].query.is_null());

        let supports = &parse.rules[parse.roots[1]];
        assert_eq!(supports.parsed_prelude_kind, 13);
        assert_eq!(supports.parsed_prelude_item_count, 1);
        assert!(!parse.prelude_items[supports.parsed_prelude_items_start].query.is_null());

        let container = &parse.rules[parse.roots[2]];
        assert_eq!(container.parsed_prelude_kind, 14);
        assert_eq!(container.parsed_prelude_item_count, 2);
        let conditions = &parse.prelude_items[container.parsed_prelude_items_start
            ..container.parsed_prelude_items_start + container.parsed_prelude_item_count];
        assert_ne!(conditions[0].value_offset, usize::MAX);
        assert!(!conditions[0].query.is_null());
        assert_eq!(conditions[1].value_offset, usize::MAX);
        assert!(!conditions[1].query.is_null());

        let invalid_supports = &parse.rules[parse.roots[3]];
        assert_eq!(invalid_supports.parsed_prelude_kind, 1);
        assert_eq!(invalid_supports.parsed_prelude_item_count, 0);
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
            parse_test_rule_prelude(&rules[0]),
            ParsedRulePrelude::Names(vec![
                utf16("reset.theme").into_boxed_slice().into(),
                utf16("widgets").into_boxed_slice().into()
            ])
        );
        assert_eq!(
            parse_test_rule_prelude(&rules[1]),
            ParsedRulePrelude::Namespace {
                prefix: Some(utf16("svg").into_boxed_slice().into()),
                uri: utf16("urn:svg").into_boxed_slice().into(),
            }
        );
        assert_eq!(
            parse_test_rule_prelude(&rules[2]),
            ParsedRulePrelude::Name(utf16("fade").into_boxed_slice().into())
        );
        let Rule::At(keyframes) = &rules[2] else {
            panic!("expected @keyframes");
        };
        let RuleOrDeclarations::Rule(keyframe) = &keyframes.children[0] else {
            panic!("expected keyframe rule");
        };
        assert_eq!(
            parse_test_rule_prelude(keyframe),
            ParsedRulePrelude::KeyframeSelectors(vec![0.0, 50.0, 100.0])
        );
        assert_eq!(
            parse_test_rule_prelude(&rules[3]),
            ParsedRulePrelude::Name(utf16("--accent").into_boxed_slice().into())
        );
        let ParsedRulePrelude::PageSelectors(selectors) = parse_test_rule_prelude(&rules[4]) else {
            panic!("expected page selectors");
        };
        assert_eq!(selectors.len(), 2);
        assert_eq!(selectors[0].name.as_deref(), Some(utf16("invoice").as_slice()));
        assert_eq!(
            selectors[0].pseudo_classes,
            vec![FfiPageSelectorItemKind::Left, FfiPageSelectorItemKind::First]
        );
        assert_eq!(selectors[1].name, None);
        assert_eq!(selectors[1].pseudo_classes, vec![FfiPageSelectorItemKind::Blank]);
        assert_eq!(
            parse_test_rule_prelude(&rules[5]),
            ParsedRulePrelude::Scope {
                has_start: true,
                has_end: true,
            }
        );
    }

    #[test]
    fn rejects_non_overridable_counter_style_names_outside_ua_style_sheets() {
        let rules = parse_stylesheet(b"@counter-style decimal {} @counter-style custom {}");
        assert_eq!(
            parse_test_rule_prelude_with_context(&rules[0]),
            ParsedRulePrelude::Invalid
        );
        assert_eq!(
            parse_test_rule_prelude_with_context(&rules[1]),
            ParsedRulePrelude::Name(utf16("custom").into_boxed_slice().into())
        );

        let mut context = parse_context();
        context.is_ua_style_sheet = true;
        assert_eq!(
            parse_rule_prelude(&rules[0], rule_kind(&rules[0]), Some(&context), &|_, _| None),
            ParsedRulePrelude::Name(utf16("decimal").into_boxed_slice().into())
        );
    }

    #[test]
    fn expands_and_deduplicates_descriptors_in_last_valid_order() {
        use crate::css::descriptor_metadata::descriptor_metadata;

        let rules = parse_stylesheet(b"@page { margin: 1px 2px 3px 4px; margin-right: 5px; size: a4; size: invalid; }");
        let Rule::At(page) = &rules[0] else {
            panic!("expected @page");
        };
        let declarations = page.children.iter().flat_map(|child| match child {
            RuleOrDeclarations::Declarations(declarations) => declarations.as_slice(),
            RuleOrDeclarations::Rule(_) => &[],
        });
        let descriptors = collect_descriptors(declarations, &parse_context(), &mut std::collections::HashMap::new());
        let id = |name: &str| descriptor_metadata(1, &utf16(name)).unwrap().id;
        assert_eq!(
            descriptors.iter().map(|descriptor| descriptor.id).collect::<Vec<_>>(),
            [
                id("margin-top"),
                id("margin-bottom"),
                id("margin-left"),
                id("margin-right"),
                id("size"),
            ]
        );
    }

    #[test]
    fn keeps_custom_descriptor_names_distinct_when_deduplicating() {
        let items = parse_block_contents(b"--first: 1; --second: 2; --first: 3", vec![RuleContext::AtFunction]);
        let RuleOrDeclarations::Declarations(declarations) = &items[0] else {
            panic!("expected declarations");
        };
        let descriptors = collect_descriptors(declarations, &parse_context(), &mut std::collections::HashMap::new());
        assert_eq!(descriptors.len(), 2);
        assert_eq!(descriptors[0].name.as_ref(), utf16("--second"));
        assert_eq!(descriptors[1].name.as_ref(), utf16("--first"));
    }

    #[test]
    fn parses_font_feature_value_rules_and_integer_lists() {
        let rules = parse_stylesheet(
            b"@font-feature-values Test { @styleset { nice: 1 +2 0; bad: 1.5; } @historical-forms { old: 3; } @character-variant { pair: 1 2; too-many: 1 2 3; } }",
        );
        let Rule::At(outer) = &rules[0] else {
            panic!("expected @font-feature-values");
        };
        let subrules = outer
            .children
            .iter()
            .filter_map(|child| match child {
                RuleOrDeclarations::Rule(rule) => Some(rule),
                RuleOrDeclarations::Declarations(_) => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(
            parse_test_rule_prelude(subrules[1]),
            ParsedRulePrelude::FontFeatureValuesRule(FfiFontFeatureValuesRuleKind::HistoricalForms)
        );
        let Rule::At(styleset) = subrules[0] else {
            panic!("expected @styleset");
        };
        let RuleOrDeclarations::Declarations(styleset_declarations) = &styleset.children[0] else {
            panic!("expected declarations");
        };
        assert_eq!(
            parse_font_feature_values(&styleset_declarations[0], usize::MAX),
            Some(vec![1, 2, 0])
        );
        assert_eq!(parse_font_feature_values(&styleset_declarations[1], usize::MAX), None);
        let Rule::At(character_variant) = subrules[2] else {
            panic!("expected @character-variant");
        };
        let RuleOrDeclarations::Declarations(character_variant_declarations) = &character_variant.children[0] else {
            panic!("expected declarations");
        };
        assert_eq!(
            parse_font_feature_values(&character_variant_declarations[0], 2),
            Some(vec![1, 2])
        );
        assert_eq!(parse_font_feature_values(&character_variant_declarations[1], 2), None);
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
        let ParsedRulePrelude::Import(import) = parse_test_rule_prelude_with_context(&rules[0]) else {
            panic!("expected import prelude");
        };
        assert_eq!(import.url_kind, FfiImportPreludeItemKind::UrlFunction);
        assert_eq!(import.layer.as_deref(), Some(utf16("theme").as_slice()));
        assert!(import.has_scope);
        assert!(import.scope_start.is_some());
        assert!(import.scope_end.is_some());
        assert!(import.supports.is_some());
        assert_eq!(import.media_queries.len(), 1);
        let ParsedRulePrelude::Function {
            name,
            parameters,
            return_type,
        } = parse_test_rule_prelude_with_context(&rules[1])
        else {
            panic!("expected function prelude");
        };
        assert_eq!(name.as_ref(), utf16("--size"));
        assert_eq!(parameters.len(), 2);
        assert_eq!(parameters[0].name.as_ref(), utf16("--base"));
        assert!(!matches!(parameters[0].syntax.as_ref(), SyntaxNode::Universal));
        assert!(parameters[0].default_value.is_some());
        assert_eq!(parameters[1].name.as_ref(), utf16("--scale"));
        assert!(!matches!(parameters[1].syntax.as_ref(), SyntaxNode::Universal));
        assert!(parameters[1].default_value.is_none());
        assert!(!matches!(return_type.as_ref(), SyntaxNode::Universal));
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
            assert_eq!(
                parse_test_rule_prelude(&rules[0]),
                ParsedRulePrelude::Invalid,
                "{source}"
            );
        }
    }

    #[test]
    fn page_selector_pseudo_classes_must_be_adjacent() {
        let rules = parse_stylesheet(b"@page named :first {} @page :first :left {}");
        assert!(
            rules
                .iter()
                .all(|rule| parse_test_rule_prelude(rule) == ParsedRulePrelude::Invalid)
        );
    }
}
